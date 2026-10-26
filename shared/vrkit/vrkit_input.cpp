#include "vrkit_input.h"
#include "vrkit_options.h"
#include <windows.h>
#include <cstdlib>
#include <xinput.h>
#include "MinHook.h"

namespace vrkit { namespace input {

namespace {
Settings g_set;
using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
struct Hooked { const wchar_t* dll; void* target; GetStateFn orig; GetStateFn origEx; void* targetEx; };
Hooked g_mods[3] = { { L"xinput1_4.dll" }, { L"xinput1_3.dll" }, { L"xinput9_1_0.dll" } };
GetStateFn g_read = nullptr;                // an original XInputGetState for our own polling
volatile bool g_block = false, g_chordHeld = false;
DWORD g_fakePacket[4] = {};

volatile bool g_untilRelease = false;   // after closing: keep blocking until the pad is released
DWORD Filter(DWORD r, DWORD user, XINPUT_STATE* st) {
    if (r == ERROR_SUCCESS && st && g_untilRelease && !g_block) {
        const XINPUT_GAMEPAD& p = st->Gamepad;
        if (!p.wButtons && p.bLeftTrigger < 30 && p.bRightTrigger < 30) g_untilRelease = false;
    }
    // while all chord buttons are down together, the game doesn't see them (no stray L3 / R3 actions)
    if (r == ERROR_SUCCESS && st && g_set.chordButtons && (st->Gamepad.wButtons & g_set.chordButtons) == g_set.chordButtons)
        st->Gamepad.wButtons &= ~g_set.chordButtons;
    if (r == ERROR_SUCCESS && st && (g_block || g_chordHeld || g_untilRelease)) {
        DWORD pkt = ++g_fakePacket[user & 3];
        ZeroMemory(st, sizeof(*st));
        st->dwPacketNumber = pkt;
    }
    return r;
}
// One hook function per DLL (each needs its own trampoline).
template <int N> DWORD WINAPI HkGet(DWORD u, XINPUT_STATE* s) { return Filter(g_mods[N].orig(u, s), u, s); }
template <int N> DWORD WINAPI HkGetEx(DWORD u, XINPUT_STATE* s) { return Filter(g_mods[N].origEx(u, s), u, s); }
void* const kHk[3] = { (void*)HkGet<0>, (void*)HkGet<1>, (void*)HkGet<2> };
void* const kHkEx[3] = { (void*)HkGetEx<0>, (void*)HkGetEx<1>, (void*)HkGetEx<2> };

// polling state
WORD g_prevButtons = 0; bool g_prevKeys[256] = {};
DWORD g_chordStart = 0; bool g_chordFired = false;
DWORD g_repeatAt[4] = {}; uint32_t g_repeatDir = 0;
} // namespace

void Configure(const Settings& s) { g_set = s; }

bool InstallHooks() {
    bool any = false;
    for (int k = 0; k < 3; ++k) {
        Hooked& h = g_mods[k];
        HMODULE m = GetModuleHandleW(h.dll);
        if (!m) continue;
        if (!h.target) {
            void* p = (void*)GetProcAddress(m, "XInputGetState");
            if (p && MH_CreateHook(p, kHk[k], (void**)&h.orig) == MH_OK && MH_EnableHook(p) == MH_OK) { h.target = p; Log("vrkit: hooked XInputGetState in %ls", h.dll); }
            void* px = (void*)GetProcAddress(m, (LPCSTR)100);   // XInputGetStateEx (ordinal 100, includes the guide button)
            if (px && px != p && MH_CreateHook(px, kHkEx[k], (void**)&h.origEx) == MH_OK && MH_EnableHook(px) == MH_OK) h.targetEx = px;
        }
        // our own polling must bypass our filter: when g_read is the export we just hooked (taken by the fallback
        // below before the DLL was hooked), switch to the trampoline. Through the hook, the chord buttons were masked
        // while held together (L3+R3 never opened the menu) and the whole pad read as zero while the menu was open.
        if (h.orig && (!g_read || g_read == (GetStateFn)h.target)) {
            if (g_read) Log("vrkit: own pad polling now bypasses the hook in %ls", h.dll);
            g_read = h.orig;
        }
        any = any || h.target;
    }
    if (!g_read) {   // the game hasn't loaded XInput (yet): read the pad ourselves
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        if (!m) m = LoadLibraryW(L"xinput1_3.dll");
        if (m) g_read = (GetStateFn)GetProcAddress(m, "XInputGetState");
    }
    return any;
}

void SetBlocking(bool b) { if (g_block && !b) g_untilRelease = true; g_block = b; }
volatile bool g_chordDown = false;   // all chord buttons down right now (before the hold time)
bool GameInputBlocked() { return g_block || g_chordHeld || g_untilRelease || g_chordDown; }
volatile LONG g_injected = 0;
void Inject(uint32_t b) { InterlockedOr(&g_injected, (LONG)b); }
volatile int g_padUser = -1;
int PadUser() { return g_padUser; }

uint32_t Poll() {
    uint32_t out = (uint32_t)InterlockedExchange(&g_injected, 0);
    // --- gamepad: every connected pad, merged. Only the first one used to be read, but with SteamVR / Steam Input
    // running, XInput slot 0 can be a virtual or idle pad while the real one is in slot 1 (FF XIII trilogy VR, user's PC).
    XINPUT_STATE st = {}; bool pad = false;
    int padUser = -1;
    static DWORD s_connLogged = 0xFFFFFFFF; static WORD s_prevByUser[4] = {}; static int s_pressLogs = 0;
    DWORD conn = 0;
    if (g_read) for (DWORD u = 0; u < 4; ++u) {
        XINPUT_STATE s = {};
        if (g_read(u, &s) != ERROR_SUCCESS) continue;
        conn |= 1u << u;
        if (!pad) { padUser = (int)u; pad = true; }
        XINPUT_GAMEPAD& m = st.Gamepad; const XINPUT_GAMEPAD& g = s.Gamepad;
        m.wButtons |= g.wButtons;
        if (g.bLeftTrigger > m.bLeftTrigger) m.bLeftTrigger = g.bLeftTrigger;
        if (g.bRightTrigger > m.bRightTrigger) m.bRightTrigger = g.bRightTrigger;
        if (abs(g.sThumbLX) > abs(m.sThumbLX)) m.sThumbLX = g.sThumbLX;
        if (abs(g.sThumbLY) > abs(m.sThumbLY)) m.sThumbLY = g.sThumbLY;
        WORD newly = g.wButtons & ~s_prevByUser[u];
        s_prevByUser[u] = g.wButtons;
        if (newly && s_pressLogs < 20) { s_pressLogs++; Log("vrkit: pad %lu buttons 0x%04X", u, (unsigned)g.wButtons); }
    }
    if (conn != s_connLogged) { s_connLogged = conn; Log("vrkit: XInput pads connected: %s%s%s%s%s", conn & 1 ? "0 " : "", conn & 2 ? "1 " : "", conn & 4 ? "2 " : "", conn & 8 ? "3 " : "", conn ? "" : "none"); }
    g_padUser = padUser;
    WORD b = pad ? st.Gamepad.wButtons : 0;
    float lt = pad ? st.Gamepad.bLeftTrigger / 255.f : 0.f, rt = pad ? st.Gamepad.bRightTrigger / 255.f : 0.f;
    // stick as d-pad
    if (pad) {
        const SHORT dz = 16000;
        if (st.Gamepad.sThumbLY > dz) b |= XINPUT_GAMEPAD_DPAD_UP;
        if (st.Gamepad.sThumbLY < -dz) b |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (st.Gamepad.sThumbLX > dz) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (st.Gamepad.sThumbLX < -dz) b |= XINPUT_GAMEPAD_DPAD_LEFT;
    }
    // after closing, blocking ends once the pad is released (also for games that never call XInput)
    if (g_untilRelease && !g_block && pad && !st.Gamepad.wButtons && st.Gamepad.bLeftTrigger < 30 && st.Gamepad.bRightTrigger < 30)
        g_untilRelease = false;
    if (g_untilRelease && !pad) g_untilRelease = false;
    WORD pressed = b & ~g_prevButtons;
    g_prevButtons = b;
    // --- open / close chord (default L3 + R3)
    DWORD now = GetTickCount();
    bool chord = pad && (g_set.chordButtons || g_set.chordTriggers);
    if (g_set.chordButtons) chord = chord && (st.Gamepad.wButtons & g_set.chordButtons) == g_set.chordButtons;
    if (g_set.chordTriggers) chord = chord && lt > g_set.triggerThreshold && rt > g_set.triggerThreshold;
    g_chordDown = chord;
    if (chord) {
        if (!g_chordStart) g_chordStart = now;
        if (!g_chordFired && now - g_chordStart >= (DWORD)g_set.chordHoldMs) { g_chordFired = true; out |= Toggle; }
        g_chordHeld = g_chordFired;   // after it fired, keep the game from seeing the triggers until released
    } else { g_chordStart = 0; g_chordFired = false; g_chordHeld = false; }
    // --- keyboard (every key is queried each frame so its edge state stays right). Esc is not used: the
    // game would see it too (keyboard input is not blocked) and most games open their own menu on it.
    auto key = [&](int vk) { bool d = (GetAsyncKeyState(vk) & 0x8000) != 0; bool p = d && !g_prevKeys[vk]; g_prevKeys[vk] = d; return p; };
    auto held = [&](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    bool kTog = g_set.keyboardToggleVk ? key(g_set.keyboardToggleVk) : false;
    bool kRet = key(VK_RETURN), kBack = key(VK_BACK), kPgUp = key(VK_PRIOR), kPgDn = key(VK_NEXT), kDel = key(VK_DELETE);
    if (kTog) out |= Toggle;
    if ((pressed & XINPUT_GAMEPAD_A) || kRet) out |= Accept;
    if ((pressed & XINPUT_GAMEPAD_B) || kBack) out |= Back;
    if ((pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) || kPgUp) out |= PagePrev;
    if ((pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) || kPgDn) out |= PageNext;
    if ((pressed & XINPUT_GAMEPAD_Y) || kDel) out |= Reset;
    // directions with auto-repeat (first repeat after 400 ms, then every 90 ms)
    uint32_t dirs = 0;
    if (b & XINPUT_GAMEPAD_DPAD_UP || held(VK_UP)) dirs |= Up;
    if (b & XINPUT_GAMEPAD_DPAD_DOWN || held(VK_DOWN)) dirs |= Down;
    if (b & XINPUT_GAMEPAD_DPAD_LEFT || held(VK_LEFT)) dirs |= Left;
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT || held(VK_RIGHT)) dirs |= Right;
    const uint32_t bits[4] = { Up, Down, Left, Right };
    for (int k = 0; k < 4; ++k) {
        if (!(dirs & bits[k])) { g_repeatAt[k] = 0; continue; }
        if (!g_repeatAt[k]) { out |= bits[k]; g_repeatAt[k] = now + 400; }
        else if ((int)(now - g_repeatAt[k]) >= 0) { out |= bits[k]; g_repeatAt[k] = now + 90; }
    }
    return out;
}

} } // namespace vrkit::input
