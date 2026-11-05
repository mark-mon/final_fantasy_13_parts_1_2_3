// FF13 VR - loaded by ffxiiiimg.exe as d3d9.dll (proxy to the system d3d9.dll).
//
// Milestone 1: D3D9 -> D3D9Ex upgrade, config.txt, logging, frame capture, screenshots,
// the flat game image on a virtual screen in the headset (XrBridge), and scripted input for unattended tests.
#include "globals.h"
#include "d3d9hooks.h"
#include "input.h"
#include "hookutil.h"
#include "xrbridge/client.h"
#include <d3d9.h>

namespace fvr {
Globals& G() { static Globals g; return g; }
float SecondsSinceStart() { return (GetTickCount() - G().startTick) / 1000.0f; }
}
using namespace fvr;

// ------------------------------------------------------------------ default config.txt
static const char kDefaultConfig[] =
// generated from tools\config_play.txt (keep the two the same)
"# FF XIII trilogy VR configuration (lives next to the game exe: ffxiiiimg.exe, ffxiii2img.exe or LRFF13.exe).\n"
"# Lines starting with # are comments. Missing settings use their defaults.\n"
"# Restart the game after editing.\n"
"\n"
"# ---- General ----\n"
"# 1 = VR output (starts XrBridgeHost.exe), 0 = mod loads but only does diagnostics\n"
"vr_enabled = 1\n"
"# 0=error 1=warn 2=info 3=debug 4=trace\n"
"log_level = 2\n"
"\n"
"# ---- Display ----\n"
"# 1 = always run the game in a window (recommended while developing)\n"
"force_windowed = 1\n"
"# 1 = run the game on a D3D9Ex device (required for VR output)\n"
"upgrade_to_ex = 1\n"
"# 3D resolution per eye = the headset's recommended resolution x render_scale (1.00 = 100%, 0.75 = 75%, 1.25 = 125%).\n"
"# 0 = the game's own resolution. render_width / render_height (both > 0) set an exact size instead.\n"
"render_scale = 1.00\n"
"render_width = 0\n"
"render_height = 0\n"
"# MSAA of the 3D scene at that resolution: game (4x, as the game asks), 0, 2, 4\n"
"render_msaa = game\n"
"# Phase lock: the game runs an exact, even 60 fps locked to the headset's refresh, so game frames are shown in a\n"
"# fixed rhythm (90 Hz: 2,1,2,1 / 72 Hz: 1,1,1,1,2 / 120 Hz: 2,2) instead of drifting. 0 = off.\n"
"headset_sync = 1\n"
"# Phase lock below 60 Hz too: when a frame-generation layer (e.g. OFXR Bridge 3X) runs the VR output at 30 Hz, the\n"
"# game still runs 60 and every second frame goes out at an even spacing, instead of an uneven pick from a free-running\n"
"# game. Check ff13vr.log: \"headset sync on: headset 30.00 Hz\" means it is active; \"headset 90.00 Hz\" means OFXR\n"
"# reports the full rate and the normal lock already gives an even 30. 0 = off.\n"
"sync_below_60 = 0\n"
"# Camera reprojection: between game frames the image is turned with the game camera, so camera pans look smooth at\n"
"# 72/90 Hz instead of moving in steps. Only rotation is corrected: your character (which the camera orbits) may\n"
"# wobble slightly while panning. 0 = off.\n"
"# Headset test: near objects ghost badly (the camera orbits the character), so it is off by default.\n"
"reproject = 0\n"
"\n"
"# ---- Virtual screen (current milestone: the normal game image on a big screen in the headset) ----\n"
"# distance and width in meters\n"
"screen_distance = 2.5\n"
"screen_width = 3.2\n"
"# Key that recenters the view (virtual-key code; 0x77 = F8)\n"
"recenter_key = 0x77\n"
"\n"
"# ---- Stereo 3D (field, battles, real-time scenes; menus and movies stay on the flat screen) ----\n"
"# 1 = render the 3D scene in stereo, 0 = flat screen only\n"
"stereo = 1\n"
"# native = both eyes every frame (scene drawn twice), aer = alternate eyes (one eye per frame, lighter on the GPU)\n"
"vr_method = native\n"
"# native only: 1 = draw the right eye once per render pass (far fewer render-target switches, which cost some GPUs a\n"
"# lot), 0 = right after every left-eye draw (the old way; slightly less CPU work)\n"
"native_batch = 1\n"
"# 1 = headset FOV (fills your view), 0 = game FOV (a smaller window into the world; use if things vanish at the edges)\n"
"fov_mode = 1\n"
"# The game only draws what its own camera sees (62 x 38 degrees), so things at the edges of your view (e.g. a ceiling\n"
"# when you look up) can be missing. This widens the game camera's culling by this factor (1 = off). Costs frame rate\n"
"# (more objects drawn, twice in native): on a 3080 Ti, 1.5 = -8%, 2.0 = -20%, 3.0 = -37%. 3.0 covers looking up ~40 deg.\n"
"cull_fov = 1.5\n"
"# game units per meter (larger = world looks smaller)\n"
"world_scale = 1.0\n"
"# HUD panel in front of you (meters)\n"
"hud_distance = 1.5\n"
"hud_width = 1.6\n"
"hud_offset_y = -0.05\n"
"\n"
"# ---- In-game settings menu ----\n"
"# Open / close: hold the pad buttons below (300 ms) or press menu_key. l3r3 = both stick clicks, ltrt = both\n"
"# triggers, none = keyboard only. menu_key is a virtual-key code (0x2D = Insert).\n"
"menu_chord = l3r3\n"
"menu_hold_ms = 300\n"
"menu_key = 0x2D\n"
"\n"
"# ---- Diagnostics ----\n"
"# Key that captures one frame of draw calls (virtual-key code; 0x7A = F11)\n"
"capture_key = 0x7A\n"
"# Also capture automatically at this frame number (0 = off)\n"
"capture_at_frame = 0\n"
"# Save a screenshot of the game image every N seconds to FF13VR\\run_*\\shots\\ (0 = off), at most screenshot_max\n"
"screenshot_interval = 0\n"
"screenshot_max = 30\n"
"\n"
"# ---- Test automation ----\n"
"# 1 = play the input script below after start (used to load a save without anyone at the PC)\n"
"autoload = 0\n"
"# Commands separated by '|' (';' starts a comment here):\n"
"#   wait <seconds>, key <DIK name> [ms], button <n> [ms], pad <A|B|X|Y|START|UP|...> [ms], vk <virtual key> [ms],\n"
"#   shot, capture, vrdump, recenter, devices, log <text>, quit\n"
"# Title screen -> Load Game -> first save -> Yes -> 'Data loaded' OK -> recap 'Start' (tested with the Chapter 9 save)\n"
"autoload_script = wait 12 | key RETURN | wait 5 | key RETURN | wait 5 | key RETURN | wait 5 | key RETURN | wait 3 | key RETURN | wait 6 | key RETURN | wait 25 | key RETURN | wait 15 | shot\n"
"# Quit the game automatically after N seconds (0 = never)\n"
"quit_after = 0\n"
"\n"
"# ---- Performance diagnostics (written to FF13VR\\ff13vr.log every 10 s) ----\n"
"# GPU busy time / render-thread CPU time per frame and the game's frame-time histogram\n"
"perf_diag = 0\n"
"pacing_diag = 0\n";

static void LoadSettings() {
    auto& c = G().cfg;
    auto& s = G().s;
    s.vrEnabled = c.GetBool("vr_enabled", true);
    s.logLevel = c.GetInt("log_level", 2);
    s.forceWindowed = c.GetBool("force_windowed", true);
    s.renderScale = c.GetFloat("render_scale", 1.0f);
    {
        std::string m = c.GetStr("render_msaa", "game");
        s.renderMsaa = (m == "game" || m.empty()) ? -1 : atoi(m.c_str());
    }
    s.renderWidth = c.GetInt("render_width", 0);
    s.renderHeight = c.GetInt("render_height", 0);
    s.upgradeToEx = c.GetBool("upgrade_to_ex", true);
    s.managedStaging = c.GetStr("managed_textures", "staging") != "dynamic";
    s.screenDistance = c.GetFloat("screen_distance", 2.5f);
    s.screenWidth = c.GetFloat("screen_width", 3.2f);
    s.recenterKey = c.GetInt("recenter_key", VK_F8);
    s.stereo = c.GetBool("stereo", true);
    {
        std::string m = c.GetStr("vr_method", "native");
        for (auto& ch : m) ch = (char)tolower((unsigned char)ch);
        s.vrMethod = m == "aer" ? VR_AER : VR_NATIVE;
    }
    s.fovMode = c.GetInt("fov_mode", 1);
    s.worldScale = c.GetFloat("world_scale", 1.0f);
    s.hudDistance = c.GetFloat("hud_distance", 1.5f);
    s.hudWidth = c.GetFloat("hud_width", 1.6f);
    s.hudOffsetY = c.GetFloat("hud_offset_y", -0.05f);
    s.debugHeadYaw = c.GetFloat("debug_head_yaw", 0);
    s.debugHeadPitch = c.GetFloat("debug_head_pitch", 0);
    s.debugNativeNoSwitch = c.GetBool("debug_native_noswitch", false);
    s.nativeBatch = c.GetBool("native_batch", true);
    s.lrDepthStereo = c.GetBool("lr_depth_stereo", true);
    s.lrClipFix = c.GetBool("lr_clip_fix", true);
    s.screenRecipFix = c.GetBool("screen_recip_fix", true);
    s.lrFlatUntilPlay = c.GetBool("lr_flat_until_play", true);
    s.lrStillUntilPlay = c.GetBool("lr_still_until_play", true);
    s.menuChord = c.GetStr("menu_chord", "l3r3");
    s.menuHoldMs = c.GetInt("menu_hold_ms", 300);
    s.menuKey = c.GetInt("menu_key", VK_INSERT);
    s.captureKey = c.GetInt("capture_key", VK_F11);
    s.captureAtFrame = c.GetInt("capture_at_frame", 0);
    s.pacingDiag = c.GetBool("pacing_diag", false);
    s.perfDiag = c.GetBool("perf_diag", false);
    s.queryNoWait = c.GetBool("query_nowait", false);
    s.frameRate = c.GetInt("frame_rate", 0);
    s.headsetSync = c.GetBool("headset_sync", true);
    s.syncBelow60 = c.GetBool("sync_below_60", false);
    s.reproject = c.GetBool("reproject", false);
    s.cullFov = (float)c.GetFloat("cull_fov", 1.5);
    s.debugSyncPeriodMs = c.GetFloat("debug_sync_period_ms", 0);
    s.screenshotInterval = c.GetFloat("screenshot_interval", 0);
    s.screenshotMax = c.GetInt("screenshot_max", 30);
    s.autoload = c.GetBool("autoload", false);
    s.autoloadScript = c.GetStr("autoload_script", "");
    s.quitAfter = c.GetFloat("quit_after", 0);
}

// ------------------------------------------------------------------ real d3d9.dll
static HMODULE g_real = nullptr;
static INIT_ONCE g_realOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK LoadReal(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);   // SysWOW64 for this 32-bit process
    wcscat_s(path, L"\\d3d9.dll");
    g_real = LoadLibraryW(path);
    if (!g_real) LOGE("Could not load system %S (%lu)", path, GetLastError());
    return TRUE;
}

extern "C" HMODULE RealD3D9Module() {
    InitOnceExecuteOnce(&g_realOnce, LoadReal, nullptr, nullptr);
    return g_real;
}

extern "C" FARPROC RealD3D9Proc(const char* name) {
    InitOnceExecuteOnce(&g_realOnce, LoadReal, nullptr, nullptr);
    FARPROC p = g_real ? GetProcAddress(g_real, name) : nullptr;
    if (!p) LOGE("system d3d9.dll has no export %s", name);
    return p;
}

// Pass-through exports the game doesn't use directly (d3dx9 and tools may). Resolved on first call.
#define PASSTHRU(name)                                                       \
    extern "C" void* p_##name = nullptr;                                     \
    extern "C" __declspec(naked) void Proxy_##name() {                       \
        __asm { mov eax, dword ptr [p_##name] }                              \
        __asm { test eax, eax }                                              \
        __asm { jnz go }                                                     \
        __asm { push offset sz_##name }                                      \
        __asm { call RealD3D9Proc }                                          \
        __asm { add esp, 4 }                                                 \
        __asm { mov dword ptr [p_##name], eax }                              \
        __asm { go: jmp eax }                                                \
    }
#define NAMESTR(name) extern "C" const char sz_##name[] = #name;

NAMESTR(D3DPERF_BeginEvent) NAMESTR(D3DPERF_EndEvent) NAMESTR(D3DPERF_GetStatus) NAMESTR(D3DPERF_QueryRepeatFrame)
NAMESTR(D3DPERF_SetMarker) NAMESTR(D3DPERF_SetOptions) NAMESTR(D3DPERF_SetRegion) NAMESTR(DebugSetLevel)
NAMESTR(DebugSetMute) NAMESTR(Direct3D9EnableMaximizedWindowedModeShim) NAMESTR(Direct3DShaderValidatorCreate9)
NAMESTR(PSGPError) NAMESTR(PSGPSampleTexture) NAMESTR(Direct3DCreate9On12) NAMESTR(Direct3DCreate9On12Ex)

PASSTHRU(D3DPERF_BeginEvent) PASSTHRU(D3DPERF_EndEvent) PASSTHRU(D3DPERF_GetStatus) PASSTHRU(D3DPERF_QueryRepeatFrame)
PASSTHRU(D3DPERF_SetMarker) PASSTHRU(D3DPERF_SetOptions) PASSTHRU(D3DPERF_SetRegion) PASSTHRU(DebugSetLevel)
PASSTHRU(DebugSetMute) PASSTHRU(Direct3D9EnableMaximizedWindowedModeShim) PASSTHRU(Direct3DShaderValidatorCreate9)
PASSTHRU(PSGPError) PASSTHRU(PSGPSampleTexture) PASSTHRU(Direct3DCreate9On12) PASSTHRU(Direct3DCreate9On12Ex)

extern "C" HRESULT WINAPI Proxy_Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out) {
    typedef HRESULT(WINAPI * F)(UINT, IDirect3D9Ex**);
    static F f = (F)RealD3D9Proc("Direct3DCreate9Ex");
    LOGI("Direct3DCreate9Ex(%u) called by the game", sdk);
    OnGameStartingGraphics();   // first D3D use by the game
    SetGameNativeEx();          // it wants Ex itself: don't hide the Ex interfaces from it
    HRESULT hr = f ? f(sdk, out) : E_FAIL;
    if (SUCCEEDED(hr) && *out) OnDirect3DCreated(*out, true);
    return hr;
}

extern "C" IDirect3D9* WINAPI Proxy_Direct3DCreate9(UINT sdk) {
    LOGI("Direct3DCreate9(%u) called by the game", sdk);
    OnGameStartingGraphics();   // first D3D use by the game
    if (G().s.upgradeToEx) {
        typedef HRESULT(WINAPI * FEx)(UINT, IDirect3D9Ex**);
        static FEx fex = (FEx)RealD3D9Proc("Direct3DCreate9Ex");
        IDirect3D9Ex* ex = nullptr;
        if (fex && SUCCEEDED(fex(sdk, &ex)) && ex) {
            LOGI("Upgraded to IDirect3D9Ex %p", ex);
            OnDirect3DCreated(ex, true);
            return ex;   // IDirect3D9Ex derives from IDirect3D9
        }
        LOGE("Direct3DCreate9Ex failed - falling back to plain D3D9 (no VR output possible)");
    }
    typedef IDirect3D9*(WINAPI * F)(UINT);
    static F f = (F)RealD3D9Proc("Direct3DCreate9");
    IDirect3D9* d = f ? f(sdk) : nullptr;
    if (d) OnDirect3DCreated(d, false);
    return d;
}

// ------------------------------------------------------------------ crash logging
// Logs access violations and similar faults (first-chance, so it also sees ones the game handles itself) with the
// faulting module+offset and the return addresses found on the stack. Addresses only; no code is read.
static std::string ModOff(uintptr_t a) {
    HMODULE m = nullptr;
    char buf[MAX_PATH + 32];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m) && m) {
        wchar_t p[MAX_PATH];
        GetModuleFileNameW(m, p, MAX_PATH);
        const wchar_t* n = wcsrchr(p, L'\\');
        static HMODULE self = [] {
            HMODULE h = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&RealD3D9Module, &h);
            return h;
        }();
        if (m == self) sprintf_s(buf, "FF13VR+%lx", (unsigned long)(a - (uintptr_t)m));   // this proxy (see dist\d3d9.map)
        else sprintf_s(buf, "%S+%lx", n ? n + 1 : p, (unsigned long)(a - (uintptr_t)m));
    } else sprintf_s(buf, "%08lx", (unsigned long)a);
    return buf;
}

thread_local bool fvr::t_memProbe = false;

static LONG CALLBACK CrashHandler(PEXCEPTION_POINTERS ep) {
    if (t_memProbe) return EXCEPTION_CONTINUE_SEARCH;   // a memory scan expects faults and handles them itself
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_PRIV_INSTRUCTION && code != 0xC0000409 /*stack buffer overrun*/)
        return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG count = 0;
    if (InterlockedIncrement(&count) > 8) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    uintptr_t addr = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    LOGE("EXCEPTION %08lX at %s (frame %ld, thread %lu)%s", code, ModOff(addr).c_str(), G().frame, GetCurrentThreadId(),
         code == EXCEPTION_ACCESS_VIOLATION ? (ep->ExceptionRecord->ExceptionInformation[0] ? " writing" : " reading") : "");
    if (code == EXCEPTION_ACCESS_VIOLATION) LOGE("  address %08lx", (unsigned long)ep->ExceptionRecord->ExceptionInformation[1]);
    LOGE("  eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx", c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi,
         c->Edi, c->Ebp, c->Esp);
    // return-address candidates: stack values that point into a loaded module
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    std::string line;
    int n = 0;
    for (uintptr_t p = c->Esp; p + 4 <= (uintptr_t)tib->StackBase && p < c->Esp + 8192 && n < 24; p += 4) {
        uintptr_t v = *(uintptr_t*)p;
        HMODULE m = nullptr;
        if (v < 0x10000 || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)v, &m)) continue;
        line += ModOff(v) + " ";
        n++;
    }
    LOGE("  stack: %s", line.c_str());
    DumpApiTrace();
    xrb::LogFlush();
    return EXCEPTION_CONTINUE_SEARCH;
}

// ------------------------------------------------------------------ startup
static void Init() {
    G().startTick = GetTickCount();
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    G().dllDir = dir;
    {
        std::wstring exe = path;
        exe = exe.substr(exe.find_last_of(L"\\/") + 1);
        for (auto& ch : exe) ch = (wchar_t)towlower(ch);
        if (exe == L"ffxiiiimg.exe") { G().game = GAME_FF13; G().gameName = "FINAL FANTASY XIII"; }
        else if (exe == L"ffxiii2img.exe") { G().game = GAME_FF13_2; G().gameName = "FINAL FANTASY XIII-2"; }
        else if (exe == L"lrff13.exe") { G().game = GAME_LR; G().gameName = "LIGHTNING RETURNS"; }
    }
    G().outDir = dir + L"FF13VR\\";
    CreateDirectoryW(G().outDir.c_str(), nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t run[64];
    swprintf_s(run, L"run_%04d%02d%02d_%02d%02d%02d\\", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    G().runDir = G().outDir + run;
    CreateDirectoryW(G().runDir.c_str(), nullptr);

    std::wstring cfgPath = dir + L"config.txt";
    xrb::WriteDefaultConfig(cfgPath.c_str(), kDefaultConfig);
    G().cfg.Load(cfgPath.c_str());
    LoadSettings();
    xrb::LogInit((G().outDir + L"ff13vr.log").c_str(), G().s.logLevel);
    LOGI("FF13 VR " __DATE__ " " __TIME__ " loaded into %S (base %p), game: %s", path, (void*)GameModule().base, G().gameName);
    G().cfg.Dump();
    LOGI("output of this run: %S", G().runDir.c_str());
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        Init();
        AddVectoredExceptionHandler(0, CrashHandler);
        InstallInputHooks();   // dinput8 is a static import of the game: already mapped, safe to detour
    } else if (reason == DLL_PROCESS_DETACH) {
        xrb::ClientShutdown();
        LOGI("Unloading");
        xrb::LogFlush();
    }
    return TRUE;
}
