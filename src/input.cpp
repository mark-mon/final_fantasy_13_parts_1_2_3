// Input hooks and the scripted test player.
//
// DirectInput: DirectInput8Create is detoured; the IDirectInput8::CreateDevice it returns is detoured, and every
// device created through it gets GetDeviceState / GetDeviceData / SetDataFormat / Acquire detours. The injected
// state (keys / joystick buttons the script holds down) is OR-ed into what the game reads. GetDeviceData emits
// synthetic press/release events for every change of the injected state, per device.
// XInput: XInputGetState of any loaded xinput DLL is detoured the same way (buttons OR-ed in).
//
// While the autoload script is running, "not acquired"/"input lost" errors are turned into empty successful reads,
// so injection keeps working when the game window is not in the foreground (unattended test runs).
#define DIRECTINPUT_VERSION 0x0800
#include "input.h"
#include "globals.h"
#include "d3d9hooks.h"
#include "hookutil.h"
#include "vtbl.h"
#include "MinHook.h"
#include "xrbridge/client.h"
#include "stereo.h"
#include "camfov.h"
#include "menu.h"
#include "vrkit_input.h"
#include <dinput.h>
#include <Xinput.h>
#include <vector>
#include <map>
#include <string>
#include <sstream>
#include <cstdio>

namespace fvr {

// ------------------------------------------------------------------ injected state (shared between threads)
static CRITICAL_SECTION g_cs;
static INIT_ONCE g_csOnce = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK InitCs(PINIT_ONCE, PVOID, PVOID*) { InitializeCriticalSection(&g_cs); return TRUE; }
struct Lock {
    Lock() { InitOnceExecuteOnce(&g_csOnce, InitCs, nullptr, nullptr); EnterCriticalSection(&g_cs); }
    ~Lock() { LeaveCriticalSection(&g_cs); }
};

static uint8_t g_keyDown[256];        // DIK codes held by the script
static uint8_t g_joyDown[32];         // DirectInput joystick buttons held by the script
static WORD g_padDown = 0;            // XInput buttons held by the script
static bool g_padVirtual = false;     // the script uses XInput: report pad 0 connected even without a real pad
static volatile bool g_scriptActive = false;

enum DevKind { DEV_UNKNOWN, DEV_KEYBOARD, DEV_MOUSE, DEV_JOYSTICK };
struct DevInfo {
    DevKind kind = DEV_UNKNOWN;
    int variant = 0;                   // 0 = ANSI interface, 1 = wide
    DWORD dataSize = 0;                // from SetDataFormat
    uint8_t reported[256] = {};        // injected state last reported through GetDeviceData (keys or 32 buttons)
    DWORD seq = 0x7F000000;            // sequence numbers of synthetic events
    unsigned stateCalls = 0, dataCalls = 0, acquires = 0, failures = 0;
    std::string name;
    LONG axisMid = 32767;              // centre of the joystick's axis range (for a neutral pad while the menu is open)
    bool haveMid = false;
};
static std::map<void*, DevInfo> g_devs;
static const char* KindName(DevKind k) {
    switch (k) { case DEV_KEYBOARD: return "keyboard"; case DEV_MOUSE: return "mouse"; case DEV_JOYSTICK: return "joystick"; default: return "unknown"; }
}

// A neutral device state: no keys, no buttons, centred axes (range from the device), POV released.
static void Neutral(void* self, DevInfo& d, DWORD cb, LPVOID data) {
    if (d.kind == DEV_JOYSTICK && cb >= sizeof(DIJOYSTATE)) {
        if (!d.haveMid) {
            DIPROPRANGE r{};
            r.diph.dwSize = sizeof(r); r.diph.dwHeaderSize = sizeof(DIPROPHEADER);
            r.diph.dwObj = DIJOFS_X; r.diph.dwHow = DIPH_BYOFFSET;
            // A and W interfaces share the vtable layout up to GetProperty
            if (SUCCEEDED(((IDirectInputDevice8A*)self)->GetProperty(DIPROP_RANGE, &r.diph))) d.axisMid = (r.lMin + r.lMax) / 2;
            d.haveMid = true;
            LOGI("input: joystick axis centre %ld (for the settings menu)", d.axisMid);
        }
        DIJOYSTATE* js = (DIJOYSTATE*)data;
        js->lX = js->lY = js->lZ = js->lRx = js->lRy = js->lRz = d.axisMid;
        for (int i = 0; i < 4; ++i) js->rgdwPOV[i] = (DWORD)-1;
        memset(js->rgbButtons, 0, sizeof(js->rgbButtons));
        if (cb >= sizeof(DIJOYSTATE2)) memset(((DIJOYSTATE2*)data)->rgbButtons + 32, 0, 96);
    } else if (d.kind == DEV_KEYBOARD || d.kind == DEV_MOUSE) {
        memset(data, 0, cb);
    }
}

// ------------------------------------------------------------------ DirectInput device hooks (A and W variants)
template <int V> struct DevHooks {
    typedef HRESULT(STDMETHODCALLTYPE* GetDeviceState_t)(void*, DWORD, LPVOID);
    typedef HRESULT(STDMETHODCALLTYPE* GetDeviceData_t)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
    typedef HRESULT(STDMETHODCALLTYPE* SetDataFormat_t)(void*, LPCDIDATAFORMAT);
    typedef HRESULT(STDMETHODCALLTYPE* Acquire_t)(void*);
    static GetDeviceState_t oGetDeviceState;
    static GetDeviceData_t oGetDeviceData;
    static SetDataFormat_t oSetDataFormat;
    static Acquire_t oAcquire;

    static HRESULT STDMETHODCALLTYPE GetDeviceState(void* self, DWORD cb, LPVOID data) {
        HRESULT hr = oGetDeviceState(self, cb, data);
        Lock l;
        auto it = g_devs.find(self);
        if (it == g_devs.end()) return hr;
        DevInfo& d = it->second;
        d.stateCalls++;
        if (FAILED(hr)) d.failures++;
        if (!data) return hr;
        if (SUCCEEDED(hr) && MenuBlocksGameInput()) {   // the settings menu is open: the game sees no input
            Neutral(self, d, cb, data);
            return hr;
        }
        if (d.kind == DEV_KEYBOARD && cb >= 256) {
            if (FAILED(hr) && g_scriptActive) { memset(data, 0, cb); hr = DI_OK; }
            if (SUCCEEDED(hr))
                for (int i = 0; i < 256; ++i) if (g_keyDown[i]) ((uint8_t*)data)[i] |= 0x80;
        } else if (d.kind == DEV_JOYSTICK && cb >= sizeof(DIJOYSTATE)) {
            if (FAILED(hr) && g_scriptActive) {
                memset(data, 0, cb);
                DIJOYSTATE* js = (DIJOYSTATE*)data;
                for (int i = 0; i < 4; ++i) js->rgdwPOV[i] = (DWORD)-1;
                js->lX = js->lY = js->lRx = js->lRy = 32767;   // centred for the usual 0..65535 range
                hr = DI_OK;
            }
            if (SUCCEEDED(hr))
                for (int i = 0; i < 32; ++i) if (g_joyDown[i]) ((DIJOYSTATE*)data)->rgbButtons[i] |= 0x80;
        }
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE GetDeviceData(void* self, DWORD cbObj, LPDIDEVICEOBJECTDATA rgdod, LPDWORD inOut, DWORD flags) {
        DWORD capacity = inOut ? *inOut : 0;
        HRESULT hr = oGetDeviceData(self, cbObj, rgdod, inOut, flags);
        Lock l;
        auto it = g_devs.find(self);
        if (it == g_devs.end()) return hr;
        DevInfo& d = it->second;
        d.dataCalls++;
        if (FAILED(hr)) d.failures++;
        if (d.kind != DEV_KEYBOARD && d.kind != DEV_JOYSTICK) return hr;
        if (SUCCEEDED(hr) && inOut && rgdod && MenuBlocksGameInput()) { *inOut = 0; return hr; }   // menu open: drop events
        if (FAILED(hr)) {
            if (!g_scriptActive || !inOut) return hr;
            *inOut = 0;
            hr = DI_OK;
        }
        if (!rgdod || !inOut || cbObj < 16) return hr;          // count query / flush
        const uint8_t* want = d.kind == DEV_KEYBOARD ? g_keyDown : g_joyDown;
        int n = d.kind == DEV_KEYBOARD ? 256 : 32;
        for (int i = 0; i < n && *inOut < capacity; ++i) {
            if (!!want[i] == !!d.reported[i]) continue;
            auto* e = (DIDEVICEOBJECTDATA*)((uint8_t*)rgdod + (size_t)(*inOut) * cbObj);
            memset(e, 0, cbObj);
            e->dwOfs = d.kind == DEV_KEYBOARD ? (DWORD)i : (DWORD)DIJOFS_BUTTON(i);
            e->dwData = want[i] ? 0x80 : 0;
            e->dwTimeStamp = GetTickCount();
            e->dwSequence = d.seq++;
            (*inOut)++;
            if (!(flags & DIGDD_PEEK)) d.reported[i] = want[i];
        }
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE SetDataFormat(void* self, LPCDIDATAFORMAT fmt) {
        HRESULT hr = oSetDataFormat(self, fmt);
        Lock l;
        auto it = g_devs.find(self);
        if (it != g_devs.end() && fmt) {
            DevInfo& d = it->second;
            d.dataSize = fmt->dwDataSize;
            if (d.kind == DEV_UNKNOWN) {
                if (fmt->dwDataSize == 256) d.kind = DEV_KEYBOARD;
                else if (fmt->dwDataSize >= sizeof(DIJOYSTATE)) d.kind = DEV_JOYSTICK;
            }
            LOGI("input: device %p (%s '%s') SetDataFormat size %lu, %lu objects, flags %lx -> %08lX", self, KindName(d.kind),
                 d.name.c_str(), fmt->dwDataSize, fmt->dwNumObjs, fmt->dwFlags, hr);
        }
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE Acquire(void* self) {
        HRESULT hr = oAcquire(self);
        Lock l;
        auto it = g_devs.find(self);
        if (it != g_devs.end()) {
            if (it->second.acquires++ < 3) LOGD("input: Acquire %p (%s) -> %08lX", self, KindName(it->second.kind), hr);
        }
        return hr;
    }

    static void Install(void* dev) {
        if (!oGetDeviceState) {
            oGetDeviceState = (GetDeviceState_t)HookVtable(dev, VT_DIDEV_GetDeviceState, (void*)&GetDeviceState);
            oGetDeviceData = (GetDeviceData_t)HookVtable(dev, VT_DIDEV_GetDeviceData, (void*)&GetDeviceData);
            oSetDataFormat = (SetDataFormat_t)HookVtable(dev, VT_DIDEV_SetDataFormat, (void*)&SetDataFormat);
            oAcquire = (Acquire_t)HookVtable(dev, VT_DIDEV_Acquire, (void*)&Acquire);
            LOGI("input: device hooks (%s) %s", V ? "W" : "A",
                 oGetDeviceState && oGetDeviceData && oSetDataFormat && oAcquire ? "installed" : "INCOMPLETE");
        }
    }
};
template <int V> typename DevHooks<V>::GetDeviceState_t DevHooks<V>::oGetDeviceState = nullptr;
template <int V> typename DevHooks<V>::GetDeviceData_t DevHooks<V>::oGetDeviceData = nullptr;
template <int V> typename DevHooks<V>::SetDataFormat_t DevHooks<V>::oSetDataFormat = nullptr;
template <int V> typename DevHooks<V>::Acquire_t DevHooks<V>::oAcquire = nullptr;

// If the A and W device vtables share an implementation, the second Install gets nullptr trampolines from MinHook
// (target already hooked) and installs nothing: the first variant's detour then handles both.
template <int V> static void InstallDevice(void* dev) { DevHooks<V>::Install(dev); }

// ------------------------------------------------------------------ IDirectInput8::CreateDevice
template <int V> struct DiHooks {
    typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(void*, REFGUID, void**, LPUNKNOWN);
    static CreateDevice_t oCreateDevice;
    static HRESULT STDMETHODCALLTYPE CreateDevice(void* self, REFGUID guid, void** out, LPUNKNOWN outer) {
        HRESULT hr = oCreateDevice(self, guid, out, outer);
        if (SUCCEEDED(hr) && out && *out) {
            DevInfo d;
            d.variant = V;
            if (IsEqualGUID(guid, GUID_SysKeyboard) || IsEqualGUID(guid, GUID_SysKeyboardEm) || IsEqualGUID(guid, GUID_SysKeyboardEm2)) d.kind = DEV_KEYBOARD;
            else if (IsEqualGUID(guid, GUID_SysMouse) || IsEqualGUID(guid, GUID_SysMouseEm) || IsEqualGUID(guid, GUID_SysMouseEm2)) d.kind = DEV_MOUSE;
            // product name + type through GetDeviceInfo (A and W structs differ only in the strings)
            if (V) {
                DIDEVICEINSTANCEW di{sizeof(di)};
                if (SUCCEEDED(((IDirectInputDevice8W*)*out)->GetDeviceInfo(&di))) {
                    char nm[260]; WideCharToMultiByte(CP_UTF8, 0, di.tszProductName, -1, nm, sizeof(nm), nullptr, nullptr);
                    d.name = nm;
                    BYTE t = GET_DIDEVICE_TYPE(di.dwDevType);
                    if (d.kind == DEV_UNKNOWN && (t == DI8DEVTYPE_JOYSTICK || t == DI8DEVTYPE_GAMEPAD || t == DI8DEVTYPE_DRIVING ||
                                                  t == DI8DEVTYPE_FLIGHT || t == DI8DEVTYPE_1STPERSON || t == DI8DEVTYPE_SUPPLEMENTAL)) d.kind = DEV_JOYSTICK;
                }
            } else {
                DIDEVICEINSTANCEA di{sizeof(di)};
                if (SUCCEEDED(((IDirectInputDevice8A*)*out)->GetDeviceInfo(&di))) {
                    d.name = di.tszProductName;
                    BYTE t = GET_DIDEVICE_TYPE(di.dwDevType);
                    if (d.kind == DEV_UNKNOWN && (t == DI8DEVTYPE_JOYSTICK || t == DI8DEVTYPE_GAMEPAD || t == DI8DEVTYPE_DRIVING ||
                                                  t == DI8DEVTYPE_FLIGHT || t == DI8DEVTYPE_1STPERSON || t == DI8DEVTYPE_SUPPLEMENTAL)) d.kind = DEV_JOYSTICK;
                }
            }
            LOGI("input: CreateDevice%s -> %p (%s '%s')", V ? "W" : "A", *out, KindName(d.kind), d.name.c_str());
            { Lock l; g_devs[*out] = d; }
            InstallDevice<V>(*out);
        }
        return hr;
    }
};
template <int V> typename DiHooks<V>::CreateDevice_t DiHooks<V>::oCreateDevice = nullptr;

typedef HRESULT(WINAPI* DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static DirectInput8Create_t oDirectInput8Create = nullptr;
static HRESULT WINAPI Hk_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID iid, LPVOID* out, LPUNKNOWN outer) {
    HRESULT hr = oDirectInput8Create(inst, ver, iid, out, outer);
    bool wide = IsEqualIID(iid, IID_IDirectInput8W);
    LOGI("input: DirectInput8Create(ver %lx, %s) -> %08lX %p", ver, wide ? "W" : IsEqualIID(iid, IID_IDirectInput8A) ? "A" : "other IID", hr, out ? *out : nullptr);
    if (SUCCEEDED(hr) && out && *out) {
        if (wide) { if (!DiHooks<1>::oCreateDevice) DiHooks<1>::oCreateDevice = (DiHooks<1>::CreateDevice_t)HookVtable(*out, VT_DI_CreateDevice, (void*)&DiHooks<1>::CreateDevice); }
        else { if (!DiHooks<0>::oCreateDevice) DiHooks<0>::oCreateDevice = (DiHooks<0>::CreateDevice_t)HookVtable(*out, VT_DI_CreateDevice, (void*)&DiHooks<0>::CreateDevice); }
    }
    return hr;
}

// ------------------------------------------------------------------ XInput
typedef DWORD(WINAPI* XInputGetState_t)(DWORD, XINPUT_STATE*);
static const wchar_t* kXInputDlls[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll", L"xinput1_2.dll", L"xinput1_1.dll"};
static const int kNumXInput = sizeof(kXInputDlls) / sizeof(kXInputDlls[0]);
static bool g_xiHooked[kNumXInput];
static unsigned g_xiCalls[kNumXInput];
template <int M> struct XiHook {
    static XInputGetState_t orig;
    static DWORD WINAPI GetState(DWORD idx, XINPUT_STATE* st) {
        DWORD r = orig(idx, st);
        g_xiCalls[M]++;
        if (idx != 0 || !st) return r;
        WORD add = g_padDown;
        if (r != ERROR_SUCCESS) {
            if (!(g_scriptActive && g_padVirtual)) return r;
            static DWORD packet = 1;
            memset(st, 0, sizeof(*st));
            st->dwPacketNumber = add ? ++packet : packet;
            r = ERROR_SUCCESS;
        }
        if (add) { st->Gamepad.wButtons |= add; st->dwPacketNumber++; }
        // the settings menu is open (or its chord held): the game sees a neutral pad. This hook sits on the game's
        // XInput DLL (Lightning Returns: xinput1_3), so vrkit's own hook can't be installed there.
        if (r == ERROR_SUCCESS && MenuBlocksGameInput()) {
            static DWORD packet = 0x10000;
            memset(&st->Gamepad, 0, sizeof(st->Gamepad));
            st->dwPacketNumber = ++packet;
        }
        return r;
    }
};
template <int M> XInputGetState_t XiHook<M>::orig = nullptr;

template <int M> static void TryHookXInput() {
    if (g_xiHooked[M]) return;
    HMODULE h = GetModuleHandleW(kXInputDlls[M]);
    if (!h) return;
    void* p = (void*)GetProcAddress(h, "XInputGetState");
    if (!p) return;
    g_xiHooked[M] = true;
    if (MH_CreateHook(p, (void*)&XiHook<M>::GetState, (void**)&XiHook<M>::orig) == MH_OK && MH_EnableHook(p) == MH_OK)
        LOGI("input: hooked XInputGetState in %S", kXInputDlls[M]);
    else LOGE("input: failed to hook XInputGetState in %S", kXInputDlls[M]);
}

// ------------------------------------------------------------------ install
void InstallInputHooks() {
    static bool mhInit = [] { MH_STATUS s = MH_Initialize(); return s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED; }();
    if (!mhInit) return;
    if (!oDirectInput8Create) {
        HMODULE di = GetModuleHandleW(L"dinput8.dll");
        if (di) {
            void* p = (void*)GetProcAddress(di, "DirectInput8Create");
            if (p && MH_CreateHook(p, (void*)&Hk_DirectInput8Create, (void**)&oDirectInput8Create) == MH_OK && MH_EnableHook(p) == MH_OK)
                LOGI("input: hooked DirectInput8Create");
            else { LOGE("input: could not hook DirectInput8Create"); oDirectInput8Create = (DirectInput8Create_t)p; }
        }
    }
    TryHookXInput<0>(); TryHookXInput<1>(); TryHookXInput<2>(); TryHookXInput<3>(); TryHookXInput<4>();
}

void LogInputDevices() {
    Lock l;
    LOGI("input: %u DirectInput devices, DirectInput8Create %s", (unsigned)g_devs.size(), oDirectInput8Create ? "hooked" : "not hooked (dinput8 not loaded?)");
    for (auto& kv : g_devs)
        LOGI("input:   %p %s '%s' (%s) data %lu bytes, GetDeviceState %u, GetDeviceData %u, failures %u, acquires %u", kv.first,
             KindName(kv.second.kind), kv.second.name.c_str(), kv.second.variant ? "W" : "A", kv.second.dataSize, kv.second.stateCalls,
             kv.second.dataCalls, kv.second.failures, kv.second.acquires);
    for (int i = 0; i < kNumXInput; ++i)
        if (g_xiHooked[i]) LOGI("input:   %S XInputGetState calls %u", kXInputDlls[i], g_xiCalls[i]);
}

// ------------------------------------------------------------------ script
static const struct { const char* name; uint8_t dik; } kDik[] = {
    {"ESCAPE", 0x01}, {"ESC", 0x01}, {"1", 0x02}, {"2", 0x03}, {"3", 0x04}, {"4", 0x05}, {"5", 0x06}, {"6", 0x07}, {"7", 0x08},
    {"8", 0x09}, {"9", 0x0A}, {"0", 0x0B}, {"MINUS", 0x0C}, {"EQUALS", 0x0D}, {"BACK", 0x0E}, {"BACKSPACE", 0x0E}, {"TAB", 0x0F},
    {"Q", 0x10}, {"W", 0x11}, {"E", 0x12}, {"R", 0x13}, {"T", 0x14}, {"Y", 0x15}, {"U", 0x16}, {"I", 0x17}, {"O", 0x18}, {"P", 0x19},
    {"LBRACKET", 0x1A}, {"RBRACKET", 0x1B}, {"RETURN", 0x1C}, {"ENTER", 0x1C}, {"LCONTROL", 0x1D}, {"A", 0x1E}, {"S", 0x1F},
    {"D", 0x20}, {"F", 0x21}, {"G", 0x22}, {"H", 0x23}, {"J", 0x24}, {"K", 0x25}, {"L", 0x26}, {"SEMICOLON", 0x27},
    {"APOSTROPHE", 0x28}, {"GRAVE", 0x29}, {"LSHIFT", 0x2A}, {"BACKSLASH", 0x2B}, {"Z", 0x2C}, {"X", 0x2D}, {"C", 0x2E},
    {"V", 0x2F}, {"B", 0x30}, {"N", 0x31}, {"M", 0x32}, {"COMMA", 0x33}, {"PERIOD", 0x34}, {"SLASH", 0x35}, {"RSHIFT", 0x36},
    {"MULTIPLY", 0x37}, {"LMENU", 0x38}, {"LALT", 0x38}, {"SPACE", 0x39}, {"CAPITAL", 0x3A}, {"F1", 0x3B}, {"F2", 0x3C},
    {"F3", 0x3D}, {"F4", 0x3E}, {"F5", 0x3F}, {"F6", 0x40}, {"F7", 0x41}, {"F8", 0x42}, {"F9", 0x43}, {"F10", 0x44},
    {"NUMPAD7", 0x47}, {"NUMPAD8", 0x48}, {"NUMPAD9", 0x49}, {"SUBTRACT", 0x4A}, {"NUMPAD4", 0x4B}, {"NUMPAD5", 0x4C},
    {"NUMPAD6", 0x4D}, {"ADD", 0x4E}, {"NUMPAD1", 0x4F}, {"NUMPAD2", 0x50}, {"NUMPAD3", 0x51}, {"NUMPAD0", 0x52},
    {"DECIMAL", 0x53}, {"F11", 0x57}, {"F12", 0x58}, {"NUMPADENTER", 0x9C}, {"RCONTROL", 0x9D}, {"DIVIDE", 0xB5},
    {"RMENU", 0xB8}, {"RALT", 0xB8}, {"HOME", 0xC7}, {"UP", 0xC8}, {"PRIOR", 0xC9}, {"PGUP", 0xC9}, {"LEFT", 0xCB},
    {"RIGHT", 0xCD}, {"END", 0xCF}, {"DOWN", 0xD0}, {"NEXT", 0xD1}, {"PGDN", 0xD1}, {"INSERT", 0xD2}, {"DELETE", 0xD3},
};
static const struct { const char* name; WORD bit; } kPad[] = {
    {"UP", XINPUT_GAMEPAD_DPAD_UP}, {"DOWN", XINPUT_GAMEPAD_DPAD_DOWN}, {"LEFT", XINPUT_GAMEPAD_DPAD_LEFT},
    {"RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT}, {"START", XINPUT_GAMEPAD_START}, {"BACK", XINPUT_GAMEPAD_BACK},
    {"LS", XINPUT_GAMEPAD_LEFT_THUMB}, {"RS", XINPUT_GAMEPAD_RIGHT_THUMB}, {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER},
    {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER}, {"A", XINPUT_GAMEPAD_A}, {"B", XINPUT_GAMEPAD_B}, {"X", XINPUT_GAMEPAD_X},
    {"Y", XINPUT_GAMEPAD_Y},
};

static int ParseDik(const std::string& s) {
    if (s.size() > 2 && (s[1] == 'x' || s[1] == 'X')) return (int)strtol(s.c_str(), nullptr, 16);
    std::string u = s;
    for (auto& c : u) c = (char)toupper((unsigned char)c);
    if (u.compare(0, 4, "DIK_") == 0) u = u.substr(4);
    for (auto& k : kDik) if (u == k.name) return k.dik;
    return -1;
}
static int ParsePad(const std::string& s) {
    std::string u = s;
    for (auto& c : u) c = (char)toupper((unsigned char)c);
    for (auto& k : kPad) if (u == k.name) return k.bit;
    return -1;
}

struct Cmd { std::string op; std::vector<std::string> args; std::string text; };
static std::vector<Cmd> g_script;
static size_t g_pc = 0;
static float g_next = 0;
struct Hold { int kind; int code; float until; };   // kind 0 key, 1 joy button, 2 pad, 3 vk
static std::vector<Hold> g_holds;
static bool g_scriptLoaded = false;

static void LoadScript() {
    g_scriptLoaded = true;
    if (!G().s.autoload) return;
    std::stringstream ss(G().s.autoloadScript);
    std::string part;
    while (std::getline(ss, part, '|')) {   // ';' can't be used: the config parser treats it as a comment
        std::stringstream ps(part);
        Cmd c;
        ps >> c.op;
        if (c.op.empty()) continue;
        for (auto& ch : c.op) ch = (char)tolower((unsigned char)ch);
        std::string a;
        while (ps >> a) c.args.push_back(a);
        c.text = part.substr(part.find_first_not_of(" \t"));
        if (c.op == "pad") g_padVirtual = true;
        g_script.push_back(c);
    }
    LOGI("script: %u commands, autoload on", (unsigned)g_script.size());
    g_scriptActive = !g_script.empty();
}

static void PressVk(int vk, bool down) {
    HWND w = G().gameWnd;
    if (!w) return;
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LPARAM lp = 1 | (sc << 16) | (down ? 0 : (3u << 30));
    PostMessageW(w, down ? WM_KEYDOWN : WM_KEYUP, (WPARAM)vk, lp);
}

static void SetHold(int kind, int code, bool down) {
    Lock l;
    switch (kind) {
    case 0: g_keyDown[code & 255] = down; break;
    case 1: g_joyDown[code & 31] = down; break;
    case 2: if (down) g_padDown |= (WORD)code; else g_padDown &= (WORD)~code; break;
    case 3: PressVk(code, down); break;
    }
}

static void RunCommand(const Cmd& c, float now) {
    auto argf = [&](size_t i, float def) { return c.args.size() > i ? (float)atof(c.args[i].c_str()) : def; };
    LOGI("script @%.1fs: %s", now, c.text.c_str());
    if (c.op == "wait") { g_next = now + argf(0, 1); return; }
    if (c.op == "key" || c.op == "button" || c.op == "pad" || c.op == "vk") {
        if (c.args.empty()) { LOGE("script: '%s' needs an argument", c.op.c_str()); return; }
        int kind = c.op == "key" ? 0 : c.op == "button" ? 1 : c.op == "pad" ? 2 : 3;
        int code = kind == 0 ? ParseDik(c.args[0]) : kind == 2 ? ParsePad(c.args[0]) : (int)strtol(c.args[0].c_str(), nullptr, 0);
        if (code < 0) { LOGE("script: unknown key/button '%s'", c.args[0].c_str()); return; }
        float ms = argf(1, 150);
        SetHold(kind, code, true);
        g_holds.push_back({kind, code, now + ms / 1000});
        g_next = now + ms / 1000 + 0.15f;   // release + a short gap before the next command
        return;
    }
    if (c.op == "shot") { RequestScreenshot(); return; }
    if (c.op == "capture") { RequestCapture(); return; }
    if (c.op == "quit") { RequestQuit(); return; }
    if (c.op == "log") return;   // the line itself was logged above
    if (c.op == "devices") { LogInputDevices(); return; }
    if (c.op == "vrdump") { static uint32_t n = 0; xrb::ClientRequestDump(++n); return; }   // host saves its next frame
    if (c.op == "recenter") { xrb::ClientRequestRecenter(); return; }
    if (c.op == "atlas") { RequestAtlasShot(); return; }
    if (c.op == "mem") { LogMemory("script"); return; }
    if (c.op == "fovscan") { CamFovScan(); return; }
    if (c.op == "menu") {   // menu open | close | <up|down|left|right|accept|reset|next|prev>
        static const std::map<std::string, uint32_t> kBtn = {{"up", vrkit::input::Up}, {"down", vrkit::input::Down}, {"left", vrkit::input::Left},
            {"right", vrkit::input::Right}, {"accept", vrkit::input::Accept}, {"reset", vrkit::input::Reset}, {"next", vrkit::input::PageNext},
            {"prev", vrkit::input::PagePrev}, {"open", vrkit::input::Toggle}, {"close", vrkit::input::Back}};
        auto it = c.args.empty() ? kBtn.end() : kBtn.find(c.args[0]);
        if (it == kBtn.end()) LOGE("script: menu needs open|close|up|down|left|right|accept|reset|next|prev");
        else vrkit::input::Inject(it->second);
        return;
    }
    if (c.op == "lrclip") { G().s.lrClipFix = argf(0, 1) != 0; return; }   // LR: 1 = one clip-space correction, 0 = as t5
    if (c.op == "lrdepth") { G().s.lrDepthStereo = argf(0, 1) != 0; return; }   // LR depth pass: 1 = per eye, 0 = mono
    if (c.op == "batch") { G().s.nativeBatch = argf(0, 1) != 0; return; }   // native right eye: 1 = batched, 0 = per draw
    if (c.op == "fovset") { CamFovSet(argf(0, 1)); return; }
    if (c.op == "campos") {
        float p[3];
        StereoCameraPos(p);
        LOGI("script: camera at %.3f %.3f %.3f (frame %ld)", p[0], p[1], p[2], G().frame);
        return;
    }
    LOGE("script: unknown command '%s'", c.op.c_str());
}

void InputTick() {
    if (!g_scriptLoaded) LoadScript();
    float now = SecondsSinceStart();
    for (size_t i = 0; i < g_holds.size();) {
        if (now >= g_holds[i].until) { SetHold(g_holds[i].kind, g_holds[i].code, false); g_holds.erase(g_holds.begin() + i); }
        else ++i;
    }
    while (g_scriptActive && g_pc < g_script.size() && now >= g_next) RunCommand(g_script[g_pc++], now);
    if (g_scriptActive && g_pc >= g_script.size() && g_holds.empty() && now >= g_next) {
        g_scriptActive = false;
        LOGI("script: finished at %.1fs", now);
        LogInputDevices();
    }
    // hotkey: one-frame capture
    static bool prevCap = false;
    bool cap = G().s.captureKey && (GetAsyncKeyState(G().s.captureKey) & 0x8000) && GetForegroundWindow() == G().gameWnd;
    if (cap && !prevCap) RequestCapture();
    prevCap = cap;
}

} // namespace fvr
