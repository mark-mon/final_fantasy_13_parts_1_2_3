// Direct3D 9 hooks.
//
// Device creation: the IDirect3D9Ex object handed to the game (see Proxy_Direct3DCreate9) has its CreateDevice
// detoured to CreateDeviceEx, so the game runs on an IDirect3DDevice9Ex (needed for shared textures / XrBridge).
// D3D9Ex rejects D3DPOOL_MANAGED, so managed textures become DEFAULT+DYNAMIC (lockable) and managed buffers DEFAULT.
//
// Device methods are inline-detoured (MinHook on the implementation, so every caller is seen). Per Present:
// end/start of a one-frame capture, input script tick, screenshots, the VR panel (the backbuffer is copied into the
// HUD region of an XrBridge atlas and shown in menu mode, i.e. as a flat screen in front of the user), quit.
#define PSAPI_VERSION 2
#include "d3d9hooks.h"
#include "globals.h"
#include "hookutil.h"
#include "vtbl.h"
#include "capture.h"
#include "imageio.h"
#include "input.h"
#include "stereo.h"
#include "pacing.h"
#include "rtscale.h"
#include "camfov.h"
#include "stereobatch.h"
#include "menu.h"
#include "xrbridge/client.h"
#include "xrbridge/ring_d3d9.h"
#include <psapi.h>
#include <intrin.h>
#include <unordered_map>
#include <vector>
#include <deque>
#include <cstdio>

namespace fvr {

// ------------------------------------------------------------------ internal-call guard and requests
static thread_local int t_internal = 0;
InternalScope::InternalScope() { ++t_internal; }
InternalScope::~InternalScope() { --t_internal; }
typedef InternalScope Internal;
bool InternalCall() { return t_internal > 0; }

static volatile bool g_wantShot = false, g_wantCapture = false, g_wantQuit = false, g_wantAtlasShot = false;
void RequestScreenshot() { g_wantShot = true; }
void RequestAtlasShot() { g_wantAtlasShot = true; }
void RequestCapture() { g_wantCapture = true; }
void RequestQuit() { g_wantQuit = true; }

static bool g_isEx = false;
static IDirect3DDevice9Ex* g_devEx = nullptr;   // the game's device (not AddRef'd)
static HWND g_devWnd = nullptr;

struct Stats {
    unsigned texRedirect = 0, texRedirectStatic = 0, texFail = 0, cubeRedirect = 0, volRedirect = 0, vbRedirect = 0, ibRedirect = 0;
    unsigned createFail = 0, updateFail = 0, presentFail = 0;
    unsigned draws = 0;   // this frame
    unsigned long long drawsTotal = 0;
};
static Stats st;

static void LogPP(const char* what, const D3DPRESENT_PARAMETERS* pp) {
    if (!pp) return;
    LOGI("%s: %ux%u fmt %s x%u, msaa %d/%lu, swap %d, wnd %p, windowed %d, autoDS %d (%s), flags %lx, refresh %u, interval %x", what,
         pp->BackBufferWidth, pp->BackBufferHeight, FormatName(pp->BackBufferFormat), pp->BackBufferCount, (int)pp->MultiSampleType,
         pp->MultiSampleQuality, (int)pp->SwapEffect, pp->hDeviceWindow, pp->Windowed, pp->EnableAutoDepthStencil,
         FormatName(pp->AutoDepthStencilFormat), pp->Flags, pp->FullScreen_RefreshRateInHz, pp->PresentationInterval);
}

// Turns a fullscreen request into a window of the same size (keeps the back buffer size the game asked for).
static void MakeWindowed(D3DPRESENT_PARAMETERS* pp, HWND wnd) {
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    if (!wnd || !pp->BackBufferWidth || !pp->BackBufferHeight) return;
    LONG style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
    SetWindowLongW(wnd, GWL_STYLE, style);
    SetWindowLongW(wnd, GWL_EXSTYLE, GetWindowLongW(wnd, GWL_EXSTYLE) & ~WS_EX_TOPMOST);
    RECT r{0, 0, (LONG)pp->BackBufferWidth, (LONG)pp->BackBufferHeight};
    AdjustWindowRect(&r, style, FALSE);
    SetWindowPos(wnd, HWND_NOTOPMOST, 40, 40, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS);
    LOGI("force_windowed: window %p set to %ldx%ld client %ux%u", wnd, r.right - r.left, r.bottom - r.top, pp->BackBufferWidth, pp->BackBufferHeight);
}

static D3DDISPLAYMODEEX FullscreenMode(const D3DPRESENT_PARAMETERS* pp) {
    D3DDISPLAYMODEEX m{sizeof(m)};
    m.Width = pp->BackBufferWidth; m.Height = pp->BackBufferHeight;
    m.RefreshRate = pp->FullScreen_RefreshRateInHz;
    m.Format = pp->BackBufferFormat;
    m.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    return m;
}

// ------------------------------------------------------------------ API trace (dumped by the crash handler)
struct TraceEntry { LONG frame; const char* what; uintptr_t a, b, c; long hr; };
static TraceEntry g_trace[64];
static volatile LONG g_traceN = 0;
static void Trace(const char* what, uintptr_t a = 0, uintptr_t b = 0, uintptr_t c = 0, long hr = 0) {
    LONG i = InterlockedIncrement(&g_traceN) - 1;
    g_trace[i & 63] = TraceEntry{G().frame, what, a, b, c, hr};
}
void DumpApiTrace() {
    LONG n = g_traceN;
    LOGE("last D3D calls (oldest first):");
    for (LONG i = n > 64 ? n - 64 : 0; i < n; ++i) {
        const TraceEntry& e = g_trace[i & 63];
        LOGE("  f%ld %s %lx %lx %lx -> %08lx", e.frame, e.what, (unsigned long)e.a, (unsigned long)e.b, (unsigned long)e.c, (unsigned long)e.hr);
    }
}
#define WH(w, h) (((uintptr_t)(w) << 16) | (uintptr_t)(h))

// ------------------------------------------------------------------ hook declarations
#define DECL(name, ret, ...) typedef ret(STDMETHODCALLTYPE* name##_t)(__VA_ARGS__); static name##_t o_##name = nullptr;
DECL(CreateDevice, HRESULT, IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**)
DECL(CreateDeviceEx, HRESULT, IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**)
DECL(CreateAdditionalSwapChain, HRESULT, IDirect3DDevice9*, D3DPRESENT_PARAMETERS*, IDirect3DSwapChain9**)
DECL(ResetEx, HRESULT, IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*)
DECL(Reset, HRESULT, IDirect3DDevice9*, D3DPRESENT_PARAMETERS*)
DECL(Present, HRESULT, IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*)
DECL(SCPresent, HRESULT, IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD)
DECL(CreateTexture, HRESULT, IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*)
DECL(CreateVolumeTexture, HRESULT, IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9**, HANDLE*)
DECL(CreateCubeTexture, HRESULT, IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9**, HANDLE*)
DECL(CreateVertexBuffer, HRESULT, IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*)
DECL(CreateIndexBuffer, HRESULT, IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*)
DECL(SetRenderTarget, HRESULT, IDirect3DDevice9*, DWORD, IDirect3DSurface9*)
DECL(SetDepthStencilSurface, HRESULT, IDirect3DDevice9*, IDirect3DSurface9*)
DECL(Clear, HRESULT, IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD)
DECL(SetViewport, HRESULT, IDirect3DDevice9*, const D3DVIEWPORT9*)
DECL(SetTexture, HRESULT, IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*)
DECL(DrawPrimitive, HRESULT, IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT)
DECL(DrawIndexedPrimitive, HRESULT, IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT)
DECL(DrawPrimitiveUP, HRESULT, IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT)
DECL(DrawIndexedPrimitiveUP, HRESULT, IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT)
DECL(CreateVertexShader, HRESULT, IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**)
DECL(SetVertexShader, HRESULT, IDirect3DDevice9*, IDirect3DVertexShader9*)
DECL(SetVertexShaderConstantF, HRESULT, IDirect3DDevice9*, UINT, const float*, UINT)
DECL(CreatePixelShader, HRESULT, IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**)
DECL(SetPixelShader, HRESULT, IDirect3DDevice9*, IDirect3DPixelShader9*)
DECL(SetPixelShaderConstantF, HRESULT, IDirect3DDevice9*, UINT, const float*, UINT)
DECL(StretchRect, HRESULT, IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE)
DECL(SetRenderState, HRESULT, IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD)
DECL(SetScissorRect, HRESULT, IDirect3DDevice9*, const RECT*)
DECL(SetStreamSource, HRESULT, IDirect3DDevice9*, UINT, IDirect3DVertexBuffer9*, UINT, UINT)
DECL(SetVertexDeclaration, HRESULT, IDirect3DDevice9*, IDirect3DVertexDeclaration9*)
DECL(SetSamplerState, HRESULT, IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD)
DECL(SetTextureStageState, HRESULT, IDirect3DDevice9*, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD)
DECL(SetIndices, HRESULT, IDirect3DDevice9*, IDirect3DIndexBuffer9*)
DECL(SetFVF, HRESULT, IDirect3DDevice9*, DWORD)
DECL(SetStreamSourceFreq, HRESULT, IDirect3DDevice9*, UINT, UINT)
DECL(SetVertexShaderConstantI, HRESULT, IDirect3DDevice9*, UINT, const int*, UINT)
DECL(SetVertexShaderConstantB, HRESULT, IDirect3DDevice9*, UINT, const BOOL*, UINT)
DECL(SetPixelShaderConstantI, HRESULT, IDirect3DDevice9*, UINT, const int*, UINT)
DECL(SetPixelShaderConstantB, HRESULT, IDirect3DDevice9*, UINT, const BOOL*, UINT)
DECL(SetClipPlane, HRESULT, IDirect3DDevice9*, DWORD, const float*)
DECL(EndScene, HRESULT, IDirect3DDevice9*)
DECL(BeginScene, HRESULT, IDirect3DDevice9*)
DECL(UpdateTexture, HRESULT, IDirect3DDevice9*, IDirect3DBaseTexture9*, IDirect3DBaseTexture9*)
DECL(UpdateSurface, HRESULT, IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const POINT*)
DECL(CreateRenderTarget, HRESULT, IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*)
DECL(CreateDepthStencilSurface, HRESULT, IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*)
DECL(GetRenderTargetData, HRESULT, IDirect3DDevice9*, IDirect3DSurface9*, IDirect3DSurface9*)
DECL(ColorFill, HRESULT, IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, D3DCOLOR)
DECL(CreateQuery, HRESULT, IDirect3DDevice9*, D3DQUERYTYPE, IDirect3DQuery9**)
DECL(QIssue, HRESULT, IDirect3DQuery9*, DWORD)
DECL(QGetData, HRESULT, IDirect3DQuery9*, void*, DWORD, DWORD)
DECL(DevQI, HRESULT, IDirect3DDevice9*, REFIID, void**)
DECL(D3DQI, HRESULT, IDirect3D9*, REFIID, void**)
DECL(TexLockRect, HRESULT, IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD)
DECL(TexUnlockRect, HRESULT, IDirect3DTexture9*, UINT)
DECL(TexAddDirtyRect, HRESULT, IDirect3DTexture9*, const RECT*)
DECL(TexGetLevelDesc, HRESULT, IDirect3DTexture9*, UINT, D3DSURFACE_DESC*)
DECL(CubeGetLevelDesc, HRESULT, IDirect3DCubeTexture9*, UINT, D3DSURFACE_DESC*)
DECL(SurfGetDesc, HRESULT, IDirect3DSurface9*, D3DSURFACE_DESC*)
DECL(SurfLockRect, HRESULT, IDirect3DSurface9*, D3DLOCKED_RECT*, const RECT*, DWORD)
DECL(SurfUnlockRect, HRESULT, IDirect3DSurface9*)
DECL(CubeAddDirtyRect, HRESULT, IDirect3DCubeTexture9*, D3DCUBEMAP_FACES, const RECT*)
DECL(CubeLockRect, HRESULT, IDirect3DCubeTexture9*, D3DCUBEMAP_FACES, UINT, D3DLOCKED_RECT*, const RECT*, DWORD)
DECL(CubeUnlockRect, HRESULT, IDirect3DCubeTexture9*, D3DCUBEMAP_FACES, UINT)
DECL(VolLockBox, HRESULT, IDirect3DVolumeTexture9*, UINT, D3DLOCKED_BOX*, const D3DBOX*, DWORD)
DECL(VBLock, HRESULT, IDirect3DVertexBuffer9*, UINT, UINT, void**, DWORD)
DECL(IBLock, HRESULT, IDirect3DIndexBuffer9*, UINT, UINT, void**, DWORD)
#undef DECL

// ------------------------------------------------------------------ hide D3D9Ex from the game
// d3dx9 (and possibly the game) change behaviour when QueryInterface finds IDirect3DDevice9Ex - e.g. D3DX refuses
// D3DPOOL_MANAGED on Ex devices before our CreateTexture redirect ever sees the call. Only the system d3d9.dll and
// this DLL get to see the Ex interfaces.
extern "C" HMODULE RealD3D9Module();
// A game that creates D3D9Ex itself (Lightning Returns) gets its Ex interfaces from everyone.
static bool g_gameNativeEx = false;
void SetGameNativeEx() { g_gameNativeEx = true; }
static bool CallerMaySeeEx(void* ret) {
    if (g_gameNativeEx) return true;
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)ret, &m)) return false;
    static HMODULE self = [] {
        HMODULE h = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&CallerMaySeeEx, &h);
        return h;
    }();
    return m == self || m == RealD3D9Module();
}
static void LogHidden(const char* what, void* ret) {
    static int n = 0;
    if (n++ >= 10) return;
    HMODULE m = nullptr;
    wchar_t p[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)ret, &m)) GetModuleFileNameW(m, p, MAX_PATH);
    const wchar_t* b = wcsrchr(p, L'\\');
    LOGI("hid %s from QueryInterface called by %S", what, b ? b + 1 : p);
}
static HRESULT STDMETHODCALLTYPE Hk_DevQI(IDirect3DDevice9* self, REFIID iid, void** out) {
    if (IsEqualIID(iid, IID_IDirect3DDevice9Ex) && !InternalCall() && !CallerMaySeeEx(_ReturnAddress())) {
        LogHidden("IDirect3DDevice9Ex", _ReturnAddress());
        if (out) *out = nullptr;
        return E_NOINTERFACE;
    }
    return o_DevQI(self, iid, out);
}
static HRESULT STDMETHODCALLTYPE Hk_D3DQI(IDirect3D9* self, REFIID iid, void** out) {
    if (IsEqualIID(iid, IID_IDirect3D9Ex) && !InternalCall() && !CallerMaySeeEx(_ReturnAddress())) {
        LogHidden("IDirect3D9Ex", _ReturnAddress());
        if (out) *out = nullptr;
        return E_NOINTERFACE;
    }
    return o_D3DQI(self, iid, out);
}

// ------------------------------------------------------------------ managed pool emulation
// D3D9Ex has no D3DPOOL_MANAGED. Redirecting to DEFAULT+DYNAMIC works, but D3D9Ex keeps dynamic textures mapped in
// the process, and the game's 32-bit address space (2 GB, the exe is not large-address-aware) ran out in battles.
// So managed textures become plain DEFAULT textures (no address space), marked as emulated (the texture and every
// level surface, which records its container/face/level), and they keep *reporting* D3DPOOL_MANAGED
// (GetLevelDesc / GetDesc) so D3DX and the game take their normal lock path. Locks (through the texture or a level
// surface) are served from a SYSTEMMEM staging texture with the same mip chain (so even 2x2 DXT levels are valid),
// uploaded per level with UpdateSurface on unlock and freed a few frames after the last unlock (kept that long so
// D3DX can read back a level it just wrote, e.g. while generating mipmaps).
// Reporting DEFAULT instead made D3DX take a special upload path that crashes inside the D3D9Ex runtime
// (IDirect3DSurface9::GetContainer / AddDirtyRect on a DEFAULT texture) - that was the battle crash.
static const GUID kEmuGuid = {0x2f1d6b90, 0x0c4e, 0x4a51, {0x8e, 0x3d, 0x5b, 0x21, 0x9a, 0x77, 0x40, 0xc6}};
struct EmuInfo { DWORD magic; IDirect3DBaseTexture9* container; UINT face, level; };   // private data of each resource
static const DWORD kEmuMagic = 0x454D5531;

struct Staging {
    IDirect3DBaseTexture9* sys = nullptr;   // SYSTEMMEM texture or cube with the container's levels
    bool cube = false;
    UINT w = 0, levels = 0;
    D3DFORMAT fmt = D3DFMT_UNKNOWN;
    int locks = 0;                           // levels currently locked
    struct LevelLock { bool locked = false, readOnly = false; RECT rect{}; };
    LevelLock lk[6][16];
    LONG unlockFrame = 0;
    size_t bytes = 0;
};
static std::unordered_map<IDirect3DBaseTexture9*, Staging> g_staging;   // key: the emulated texture
static CRITICAL_SECTION g_stagingCs;
static bool g_stagingCsInit = [] { InitializeCriticalSection(&g_stagingCs); return true; }();
static unsigned g_emuLocks = 0, g_emuFails = 0;
static size_t g_stagingBytes = 0;

static bool GetEmu(IDirect3DResource9* r, EmuInfo* out) {
    EmuInfo e{};
    DWORD sz = sizeof(e);
    if (!r || FAILED(r->GetPrivateData(kEmuGuid, &e, &sz)) || sz != sizeof(e) || e.magic != kEmuMagic) return false;
    if (out) *out = e;
    return true;
}
static bool IsEmulated(IDirect3DResource9* r) { return GetEmu(r, nullptr); }
static void MarkOne(IDirect3DResource9* r, IDirect3DBaseTexture9* c, UINT face, UINT level) {
    EmuInfo e{kEmuMagic, c, face, level};
    r->SetPrivateData(kEmuGuid, &e, sizeof(e), 0);
}
static void MarkEmulated(IDirect3DTexture9* t) {
    Internal in;
    MarkOne(t, t, 0, 0);
    for (DWORD l = 0; l < t->GetLevelCount(); ++l) {
        IDirect3DSurface9* s = nullptr;
        if (SUCCEEDED(t->GetSurfaceLevel(l, &s)) && s) { MarkOne(s, t, 0, l); s->Release(); }
    }
}
static void MarkEmulated(IDirect3DCubeTexture9* t) {
    Internal in;
    MarkOne(t, t, 0, 0);
    for (int f = 0; f < 6; ++f)
        for (DWORD l = 0; l < t->GetLevelCount(); ++l) {
            IDirect3DSurface9* s = nullptr;
            if (SUCCEEDED(t->GetCubeMapSurface((D3DCUBEMAP_FACES)f, l, &s)) && s) { MarkOne(s, t, (UINT)f, l); s->Release(); }
        }
}
static size_t SurfBytes(UINT w, UINT h, D3DFORMAT f) {
    switch (f) {
    case D3DFMT_DXT1: return (size_t)((w + 3) / 4) * ((h + 3) / 4) * 8;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: return (size_t)((w + 3) / 4) * ((h + 3) / 4) * 16;
    default: return (size_t)w * h * 4;
    }
}
static IDirect3DSurface9* LevelSurface(IDirect3DBaseTexture9* t, bool cube, UINT face, UINT level) {
    IDirect3DSurface9* s = nullptr;
    Internal in;
    if (cube) ((IDirect3DCubeTexture9*)t)->GetCubeMapSurface((D3DCUBEMAP_FACES)face, level, &s);
    else ((IDirect3DTexture9*)t)->GetSurfaceLevel(level, &s);
    return s;   // AddRef'd
}

static HRESULT EmuLock(const EmuInfo& e, D3DLOCKED_RECT* lr, const RECT* r, DWORD fl) {
    IDirect3DBaseTexture9* c = e.container;
    bool cube = c->GetType() == D3DRTYPE_CUBETEXTURE;
    D3DSURFACE_DESC d0{}, d{};
    {
        Internal in;
        if (cube) { ((IDirect3DCubeTexture9*)c)->GetLevelDesc(0, &d0); ((IDirect3DCubeTexture9*)c)->GetLevelDesc(e.level, &d); }
        else { ((IDirect3DTexture9*)c)->GetLevelDesc(0, &d0); ((IDirect3DTexture9*)c)->GetLevelDesc(e.level, &d); }
    }
    if (e.face >= 6 || e.level >= 16) return D3DERR_INVALIDCALL;
    EnterCriticalSection(&g_stagingCs);
    Staging& s = g_staging[c];
    HRESULT hr = D3D_OK;
    UINT levels = c->GetLevelCount();
    if (s.lk[e.face][e.level].locked) hr = D3DERR_INVALIDCALL;
    else {
        if (s.sys && (s.cube != cube || s.w != d0.Width || s.levels != levels || s.fmt != d0.Format) && s.locks == 0) {   // stale
            g_stagingBytes -= s.bytes;
            s.sys->Release();
            s = Staging();
        }
        if (!s.sys) {
            Internal in;
            if (cube) hr = g_devEx->CreateCubeTexture(d0.Width, levels, 0, d0.Format, D3DPOOL_SYSTEMMEM, (IDirect3DCubeTexture9**)&s.sys, nullptr);
            else hr = g_devEx->CreateTexture(d0.Width, d0.Height, levels, 0, d0.Format, D3DPOOL_SYSTEMMEM, (IDirect3DTexture9**)&s.sys, nullptr);
            if (SUCCEEDED(hr)) {
                s.cube = cube; s.w = d0.Width; s.levels = levels; s.fmt = d0.Format;
                s.bytes = SurfBytes(d0.Width, d0.Height, d0.Format) * 4 / 3 * (cube ? 6 : 1);
                g_stagingBytes += s.bytes;
            }
        }
        IDirect3DSurface9* ss = SUCCEEDED(hr) ? LevelSurface(s.sys, cube, e.face, e.level) : nullptr;
        if (SUCCEEDED(hr) && !ss) hr = D3DERR_INVALIDCALL;
        if (SUCCEEDED(hr)) { Internal in; hr = ss->LockRect(lr, r, fl & (D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)); }
        if (ss) ss->Release();
        if (SUCCEEDED(hr)) {
            auto& l = s.lk[e.face][e.level];
            l.locked = true;
            l.readOnly = (fl & D3DLOCK_READONLY) != 0;
            l.rect = r ? *r : RECT{0, 0, (LONG)d.Width, (LONG)d.Height};
            s.locks++;
            g_emuLocks++;
        }
    }
    if (FAILED(hr) && g_emuFails++ < 20)
        LOGE("managed emulation: lock of %ux%u %s (face %u level %u) failed %08lX", d.Width, d.Height, FormatName(d.Format), e.face, e.level, hr);
    LeaveCriticalSection(&g_stagingCs);
    return hr;
}

static HRESULT EmuUnlock(const EmuInfo& e) {
    if (e.face >= 6 || e.level >= 16) return D3DERR_INVALIDCALL;
    EnterCriticalSection(&g_stagingCs);
    auto it = g_staging.find(e.container);
    HRESULT hr = D3DERR_INVALIDCALL;
    if (it != g_staging.end() && it->second.lk[e.face][e.level].locked) {
        Staging& s = it->second;
        auto& l = s.lk[e.face][e.level];
        IDirect3DSurface9* ss = LevelSurface(s.sys, s.cube, e.face, e.level);
        IDirect3DSurface9* dst = LevelSurface(e.container, s.cube, e.face, e.level);
        Internal in;
        if (ss) ss->UnlockRect();
        l.locked = false;
        s.locks--;
        s.unlockFrame = G().frame;
        hr = D3D_OK;
        if (!l.readOnly && ss && dst) {
            POINT pt{l.rect.left, l.rect.top};
            hr = g_devEx->UpdateSurface(ss, &l.rect, dst, &pt);
            if (FAILED(hr) && g_emuFails++ < 20) LOGE("managed emulation: upload of level %u (%s) failed %08lX", e.level, FormatName(s.fmt), hr);
        }
        if (ss) ss->Release();
        if (dst) dst->Release();
    }
    LeaveCriticalSection(&g_stagingCs);
    return hr;
}

// Present: free staging textures that have been idle for a few frames.
static void EmuTrim() {
    EnterCriticalSection(&g_stagingCs);
    LONG f = G().frame;
    for (auto it = g_staging.begin(); it != g_staging.end();) {
        if (it->second.locks == 0 && f - it->second.unlockFrame >= 3) {
            if (it->second.sys) { it->second.sys->Release(); g_stagingBytes -= it->second.bytes; }
            it = g_staging.erase(it);
        } else ++it;
    }
    LeaveCriticalSection(&g_stagingCs);
}

// AddDirtyRect: a no-op for emulated textures (uploads happen at unlock). On a real DEFAULT texture the D3D9Ex
// runtime can crash in it, and it is meaningless there, so it is answered with D3D_OK as well.
static unsigned g_dirtyIgnored = 0;
static HRESULT STDMETHODCALLTYPE Hk_TexAddDirtyRect(IDirect3DTexture9* t, const RECT* r) {
    D3DSURFACE_DESC d{};
    { Internal in; t->GetLevelDesc(0, &d); }
    if (d.Pool == D3DPOOL_DEFAULT) { g_dirtyIgnored++; return D3D_OK; }
    return o_TexAddDirtyRect(t, r);
}
static HRESULT STDMETHODCALLTYPE Hk_CubeAddDirtyRect(IDirect3DCubeTexture9* t, D3DCUBEMAP_FACES f, const RECT* r) {
    D3DSURFACE_DESC d{};
    { Internal in; t->GetLevelDesc(0, &d); }
    if (d.Pool == D3DPOOL_DEFAULT) { g_dirtyIgnored++; return D3D_OK; }
    return o_CubeAddDirtyRect(t, f, r);
}

// Emulated resources report D3DPOOL_MANAGED, and resized render targets their original size, to the game (and D3DX) only.
// d3d9.dll itself must see the truth: it validates e.g. StretchRect rects through these same methods (with the lie,
// a scaled rect looked out of bounds -> D3DERR_INVALIDCALL -> the game crashed at the battle transition).
static HRESULT STDMETHODCALLTYPE Hk_TexGetLevelDesc(IDirect3DTexture9* t, UINT lv, D3DSURFACE_DESC* d) {
    HRESULT hr = o_TexGetLevelDesc(t, lv, d);
    if (SUCCEEDED(hr) && d && !InternalCall() && !CallerMaySeeEx(_ReturnAddress())) {
        if (d->Pool == D3DPOOL_DEFAULT && (d->Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) {
            ScaleInfo si;
            if (RtGetInfo(t, &si)) { UINT w = si.origW >> lv, h = si.origH >> lv; d->Width = w ? w : 1; d->Height = h ? h : 1; }
        } else if (d->Pool == D3DPOOL_DEFAULT && IsEmulated(t)) d->Pool = D3DPOOL_MANAGED;
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_CubeGetLevelDesc(IDirect3DCubeTexture9* t, UINT lv, D3DSURFACE_DESC* d) {
    HRESULT hr = o_CubeGetLevelDesc(t, lv, d);
    if (SUCCEEDED(hr) && d && !InternalCall() && !CallerMaySeeEx(_ReturnAddress()) && d->Pool == D3DPOOL_DEFAULT && IsEmulated(t)) d->Pool = D3DPOOL_MANAGED;
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_SurfGetDesc(IDirect3DSurface9* s, D3DSURFACE_DESC* d) {
    HRESULT hr = o_SurfGetDesc(s, d);
    if (SUCCEEDED(hr) && d && !InternalCall() && !CallerMaySeeEx(_ReturnAddress())) {
        if (d->Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) {
            ScaleInfo si;
            if (RtGetInfo(s, &si)) { d->Width = si.origW; d->Height = si.origH; }
        } else if (d->Pool == D3DPOOL_DEFAULT && IsEmulated(s)) d->Pool = D3DPOOL_MANAGED;
    }
    return hr;
}

static void FlushForLock(DWORD fl, bool buffer);
static HRESULT STDMETHODCALLTYPE Hk_SurfLockRect(IDirect3DSurface9* s, D3DLOCKED_RECT* lr, const RECT* r, DWORD fl) {
    if (!InternalCall()) FlushForLock(fl, false);
    EmuInfo e;
    if (!InternalCall() && GetEmu(s, &e)) return EmuLock(e, lr, r, fl);
    HRESULT hr = o_SurfLockRect(s, lr, r, fl);
    if (!InternalCall()) Trace("SurfLockRect", (uintptr_t)s, r ? 1 : 0, RtGetInfo(s, nullptr), hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_SurfUnlockRect(IDirect3DSurface9* s) {
    EmuInfo e;
    if (!InternalCall() && GetEmu(s, &e)) return EmuUnlock(e);
    return o_SurfUnlockRect(s);
}

static unsigned g_lockFails = 0;
static DWORD g_renderThread = 0;
// A lock that may change data the recorded right-eye draws still need: draw them first (render thread only).
static void FlushForLock(DWORD fl, bool buffer) {
    if (!BatchOpen() || GetCurrentThreadId() != g_renderThread || !g_devEx) return;
    if (fl & D3DLOCK_READONLY) return;
    if (buffer && (fl & D3DLOCK_NOOVERWRITE)) return;   // appends: the data already drawn stays
    BatchFlush(g_devEx, BF_LOCK);
}
static HRESULT STDMETHODCALLTYPE Hk_TexLockRect(IDirect3DTexture9* t, UINT lv, D3DLOCKED_RECT* lr, const RECT* r, DWORD fl) {
    if (!InternalCall()) FlushForLock(fl, false);
    if (!InternalCall() && IsEmulated(t)) return EmuLock({kEmuMagic, t, 0, lv}, lr, r, fl);
    HRESULT hr = o_TexLockRect(t, lv, lr, r, fl);
    if (!InternalCall()) Trace("TexLockRect", (uintptr_t)t, lv, RtGetInfo(t, nullptr), hr);
    if (FAILED(hr) && g_lockFails++ < 30) {
        D3DSURFACE_DESC d{}; t->GetLevelDesc(lv, &d);
        LOGE("Texture LockRect failed %08lX: level %u of %ux%u %s usage %lx pool %d, flags %lx", hr, lv, d.Width, d.Height, FormatName(d.Format), d.Usage, (int)d.Pool, fl);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_TexUnlockRect(IDirect3DTexture9* t, UINT lv) {
    if (!InternalCall() && IsEmulated(t)) return EmuUnlock({kEmuMagic, t, 0, lv});
    return o_TexUnlockRect(t, lv);
}
static HRESULT STDMETHODCALLTYPE Hk_CubeLockRect(IDirect3DCubeTexture9* t, D3DCUBEMAP_FACES f, UINT lv, D3DLOCKED_RECT* lr, const RECT* r, DWORD fl) {
    if (!InternalCall()) FlushForLock(fl, false);
    if (!InternalCall() && IsEmulated(t)) return EmuLock({kEmuMagic, t, (UINT)f, lv}, lr, r, fl);
    HRESULT hr = o_CubeLockRect(t, f, lv, lr, r, fl);
    if (FAILED(hr) && g_lockFails++ < 30) {
        D3DSURFACE_DESC d{}; t->GetLevelDesc(lv, &d);
        LOGE("Cube LockRect failed %08lX: face %d level %u of %u %s usage %lx pool %d, flags %lx", hr, (int)f, lv, d.Width, FormatName(d.Format), d.Usage, (int)d.Pool, fl);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_CubeUnlockRect(IDirect3DCubeTexture9* t, D3DCUBEMAP_FACES f, UINT lv) {
    if (!InternalCall() && IsEmulated(t)) return EmuUnlock({kEmuMagic, t, (UINT)f, lv});
    return o_CubeUnlockRect(t, f, lv);
}
static HRESULT STDMETHODCALLTYPE Hk_VolLockBox(IDirect3DVolumeTexture9* t, UINT lv, D3DLOCKED_BOX* lb, const D3DBOX* b, DWORD fl) {
    if (!InternalCall()) FlushForLock(fl, false);
    HRESULT hr = o_VolLockBox(t, lv, lb, b, fl);
    if (FAILED(hr) && g_lockFails++ < 30) LOGE("Volume LockBox failed %08lX: level %u flags %lx", hr, lv, fl);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_VBLock(IDirect3DVertexBuffer9* b, UINT off, UINT sz, void** p, DWORD fl) {
    if (!InternalCall()) FlushForLock(fl, true);
    HRESULT hr = o_VBLock(b, off, sz, p, fl);
    if (FAILED(hr) && g_lockFails++ < 30) {
        D3DVERTEXBUFFER_DESC d{}; b->GetDesc(&d);
        LOGE("VB Lock failed %08lX: %u+%u of %u, usage %lx pool %d, flags %lx", hr, off, sz, d.Size, d.Usage, (int)d.Pool, fl);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_IBLock(IDirect3DIndexBuffer9* b, UINT off, UINT sz, void** p, DWORD fl) {
    if (!InternalCall()) FlushForLock(fl, true);
    HRESULT hr = o_IBLock(b, off, sz, p, fl);
    if (FAILED(hr) && g_lockFails++ < 30) {
        D3DINDEXBUFFER_DESC d{}; b->GetDesc(&d);
        LOGE("IB Lock failed %08lX: %u+%u of %u, usage %lx pool %d, flags %lx", hr, off, sz, d.Size, d.Usage, (int)d.Pool, fl);
    }
    return hr;
}
// Installs the hook once (several call sites may try; a later attempt must never overwrite a working trampoline).
#define HOOK_ONCE(obj, name, slot)                                                              \
    do {                                                                                        \
        static bool tried = false;                                                              \
        if (!o_##name && !tried && (obj)) {                                                     \
            tried = true;                                                                       \
            name##_t t = (name##_t)HookVtable((obj), slot, (void*)&Hk_##name);                  \
            if (t) o_##name = t;                                                                \
            LOGI("hook " #name " %s", t ? "installed" : "FAILED");                            \
        }                                                                                       \
    } while (0)

// ------------------------------------------------------------------ VR panel (XrBridge, menu mode)
static xrb::D3D9Ring g_ring;
static bool g_clientInit = false;
static UINT g_ringW = 0, g_ringH = 0;
static unsigned g_vrFrames = 0;
static unsigned g_vrSkipped = 0;   // frames not shown: no free ring slot (the host still held them all)

static void VrStart() {
    if (!G().s.vrEnabled || g_clientInit) return;
    g_clientInit = true;
    xrb::ClientSettings cs;
    cs.instanceName = "FF13VR";
    cs.appName = "Final Fantasy XIII VR";
    cs.hostExePath = G().dllDir + L"XrBridgeHost.exe";
    cs.hostLogPath = G().outDir + L"xrhost.log";
    cs.logLevel = G().s.logLevel;
    cs.menuDistance = G().s.screenDistance;
    cs.menuWidth = G().s.screenWidth;
    cs.hudDistance = G().s.hudDistance;
    cs.hudWidth = G().s.hudWidth;
    cs.hudOffsetY = G().s.hudOffsetY;
    cs.hudPremultiplied = true;   // the stereo HUD texture is premultiplied (menu mode ignores alpha)
    xrb::ClientInit(cs);
}

// The ring is shared by the render thread (VrFrame, Reset) and the phase-lock submitter thread.
static CRITICAL_SECTION g_ringCs;
static bool g_ringCsInit = [] { InitializeCriticalSection(&g_ringCs); return true; }();
struct RingLock { RingLock() { EnterCriticalSection(&g_ringCs); } ~RingLock() { LeaveCriticalSection(&g_ringCs); } };
static bool g_vrEnded = false;   // VrFrame put this frame into a slot

// ------------------------------------------------------------------ camera-aware reprojection (reproject = 1)
// The host turns each eye image with the game camera between game frames (XrBridge SLOT_REPROJECT). It needs the
// camera's angular velocity in view space (= the headset's reference space) and the time each eye image shows.
// Content time = the frame's hand-over time with the phase lock (even 60 fps), else the time of its Present.
static double g_contentNs = 0;                    // set by BeforePresent for the frame being presented
static struct { long frame; double ns; } g_ftRing[8] = {};
static int g_ftNext = 0;
static float g_angVel[3] = {};
static unsigned g_rpFrames = 0, g_rpCuts = 0;
static double g_rpSpeed = 0;

static void RecordContentTime(LONGLONG qpc) {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    g_contentNs = (double)qpc / (double)f.QuadPart * 1e9;
    g_ftRing[g_ftNext] = {G().frame - 1, g_contentNs};   // the number the frame had while rendering (Present increments it)
    g_ftNext = (g_ftNext + 1) % 8;
}

static bool BuildMotion(xrb::IpcMotion* m) {
    if (!G().s.reproject) return false;
    float rot[3][3];
    long sf, ef[2];
    if (!StereoSceneCamera(rot, &sf, ef)) return false;
    auto timeOf = [](long fr, double* ns) {
        for (auto& r : g_ftRing) if (r.frame == fr && r.ns) { *ns = r.ns; return true; }
        return false;
    };
    double ts;
    if (!timeOf(sf, &ts)) return false;           // the eye images are older than the last 8 frames
    for (int e = 0; e < 2; ++e) {
        double te;
        m->contentTime[e] = (int64_t)(timeOf(ef[e], &te) ? te : ts);
    }
    static float prev[3][3];
    static long prevFrame = -1;
    static double prevNs = 0;
    if (sf != prevFrame) {
        // rotation of view-space content from the previous scene to this one: x_k = M x_(k-1), M = Rk * R(k-1)^T
        float M[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) M[i][j] = rot[i][0] * prev[j][0] + rot[i][1] * prev[j][1] + rot[i][2] * prev[j][2];
        double dt = (ts - prevNs) / 1e9;
        g_angVel[0] = g_angVel[1] = g_angVel[2] = 0;
        if (prevFrame >= 0 && sf - prevFrame <= 3 && dt > 0.002 && dt < 0.1) {
            double w = sqrt(fmax(0.0, 1.0 + M[0][0] + M[1][1] + M[2][2])) / 2;
            double v[3] = {M[2][1] - M[1][2], M[0][2] - M[2][0], M[1][0] - M[0][1]};   // = 4w * (x, y, z)
            double s = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            double angle = 2 * atan2(s / 2, w);   // |(x,y,z)| = s / (4w) = sin(angle/2), w = cos(angle/2)
            if (angle > 0.5) g_rpCuts++;          // > ~30 degrees in one frame: a camera cut, not a pan
            else if (s > 1e-9) for (int k = 0; k < 3; ++k) g_angVel[k] = (float)(v[k] / s * angle / dt);
        }
        memcpy(prev, rot, sizeof(prev));
        prevFrame = sf;
        prevNs = ts;
    }
    memcpy(m->angVel, g_angVel, sizeof(g_angVel));
    g_rpFrames++;
    g_rpSpeed += sqrt(g_angVel[0] * g_angVel[0] + g_angVel[1] * g_angVel[1] + g_angVel[2] * g_angVel[2]) * 57.2958;
    return true;
}

static void VrFrameImpl(IDirect3DDevice9* dev);
static bool VrFrame(IDirect3DDevice9* dev) {
    RingLock lock;
    g_vrEnded = false;
    VrFrameImpl(dev);
    return g_vrEnded;
}

static void VrFrameImpl(IDirect3DDevice9* dev) {
    if (!G().s.vrEnabled || !g_devEx || !g_clientInit) return;
    static int lastState = -1;
    int state = xrb::ClientHostVisible() ? 2 : xrb::ClientHostConnected() ? 1 : 0;
    if (state != lastState) {
        const char* err = xrb::ClientHostError();
        LOGI("vr: host %s%s%s", state == 2 ? "visible" : state == 1 ? "connected (not visible)" : "not connected", *err ? " - error: " : "", err);
        lastState = state;
    }
    if (state == 0) return;
    IDirect3DSurface9* bb = nullptr;
    if (!(bb = GameBackBuffer(dev))) return;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    if (!g_ring.Valid() || d.Width != g_ringW || d.Height != g_ringH) {
        // atlas = [left eye | right eye | HUD], all at the back-buffer size. In menu mode only the HUD region is used
        // (the whole game image on the flat screen); in stereo the eyes hold the 3D scene and the HUD the UI.
        // eyes at the 3D scene resolution (the headset's size when render targets are scaled), HUD at the game's
        UINT eyeW = G().s.stereo ? RtSceneW() : 64, eyeH = G().s.stereo ? RtSceneH() : 64;
        if (!eyeW || !eyeH) { eyeW = d.Width; eyeH = d.Height; }
        // HUD region: the game's aspect (the quad takes its shape from it), as wide as an eye. The UI target may be
        // scaled non-uniformly (headset aspect), so the HUD texture is resampled into this 16:9 rect.
        UINT hudW = G().s.stereo ? eyeW : d.Width;
        UINT hudH = G().s.stereo ? (UINT)((double)hudW * d.Height / d.Width + 0.5) : d.Height;
        if (!g_ring.Create(g_devEx, eyeW, eyeH, hudW, hudH)) { bb->Release(); LOGE("vr: ring creation failed"); G().s.vrEnabled = false; return; }
        g_ringW = d.Width; g_ringH = d.Height;
    }
    int slot = g_ring.Begin();
    if (slot >= 0) {
        IDirect3DSurface9* atlas = g_ring.Surface(slot);
        RECT hr = g_ring.HudRect();
        static int fails = 0;
        static bool lastStereo = false;
        bool stereo = StereoFrameReady();
        if (stereo != lastStereo) { LOGI("vr: %s", stereo ? "stereo 3D scene + HUD" : "flat screen (menu mode)"); lastStereo = stereo; }
        xrb::IpcPose pose[2] = {};
        xrb::IpcFov fov[2] = {};
        int64_t t = 0;
        uint32_t flags = xrb::SLOT_HUD_VISIBLE;
        xrb::IpcMotion motion{};
        if (stereo) {
            for (int e = 0; e < 2; ++e) {
                RECT er = g_ring.EyeRect(e);
                HRESULT r = dev->StretchRect(StereoEyeSurface(e), nullptr, atlas, &er, D3DTEXF_NONE);
                if (FAILED(r) && fails++ < 5) LOGE("vr: StretchRect eye %d -> atlas failed %08lX", e, r);
            }
            HRESULT r = dev->StretchRect(StereoHudSurface(), nullptr, atlas, &hr, D3DTEXF_LINEAR);   // back to 16:9
            if (FAILED(r) && fails++ < 5) LOGE("vr: StretchRect HUD -> atlas failed %08lX", r);
            MenuRender(dev, atlas, hr);   // the settings menu over the HUD panel
            StereoPoses(pose, fov, &t);
            if (BuildMotion(&motion)) flags |= xrb::SLOT_REPROJECT;
        } else {
            HRESULT r = dev->StretchRect(bb, nullptr, atlas, &hr, D3DTEXF_LINEAR);
            if (FAILED(r) && fails++ < 5) LOGE("vr: StretchRect backbuffer -> atlas failed %08lX", r);
            MenuRender(dev, atlas, hr);   // over the flat screen
            if (!xrb::ClientGetLatestPoses(pose, fov, &t))
                for (int e = 0; e < 2; ++e) { pose[e].qw = 1; fov[e] = {-0.8f, 0.8f, 0.8f, -0.8f}; }
            flags |= xrb::SLOT_MENU_MODE;
        }
        g_ring.End(slot, pose, fov, t, flags, false, &motion);
        g_vrEnded = true;
        g_vrFrames++;
        if (g_wantAtlasShot) {   // debug: what we hand to the host
            g_wantAtlasShot = false;
            CreateDirectoryW((G().runDir + L"shots").c_str(), nullptr);
            wchar_t p[MAX_PATH];
            static unsigned n = 0;
            swprintf_s(p, L"%sshots\\atlas_%03u_f%06ld", G().runDir.c_str(), n++, G().frame);
            std::string note;
            SaveSurfaceImage(dev, atlas, p, &note);
            LOGI("atlas image %S (%s): %s", wcsrchr(p, L'\\') + 1, stereo ? "stereo" : "menu", note.c_str());
        }
    } else g_vrSkipped++;
    bb->Release();
}

// Phase lock submitter: hands each frame's slot to the host at its scheduled time (PacingHeadsetSync), on its own
// thread so the render thread never waits for the GPU. While it runs, the ring's automatic submission is off.
static std::deque<LONGLONG> g_handovers;      // scheduled QPC times, one per slot in the ring (guarded by g_ringCs)
static HANDLE g_subEvent = nullptr, g_subThread = nullptr;
static volatile LONG g_subDone = 0, g_subLate = 0, g_subDropped = 0;

static DWORD WINAPI Submitter(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    for (;;) {
        LONGLONG due = 0;
        {
            RingLock lock;
            if (!g_handovers.empty()) due = g_handovers.front();
        }
        if (!due) { WaitForSingleObject(g_subEvent, 20); continue; }
        for (;;) {   // until the hand-over time: sleep while far away, spin the last ~1.5 ms
            LARGE_INTEGER n;
            QueryPerformanceCounter(&n);
            double remainMs = (double)(due - n.QuadPart) * 1000.0 / f.QuadPart;
            if (remainMs <= 0) break;
            if (remainMs > 1.5) Sleep(1); else YieldProcessor();
        }
        DWORD t0 = GetTickCount();
        bool done = false, none = false;
        while (!done && GetTickCount() - t0 < 40) {
            {
                RingLock lock;
                if (!g_ring.Valid() || !g_ring.PendingCount()) none = true;
                else done = g_ring.PumpOne();
            }
            if (done || none) break;
            Sleep(0);   // the GPU is still finishing the frame
        }
        LARGE_INTEGER n;
        QueryPerformanceCounter(&n);
        if (done) {
            InterlockedIncrement(&g_subDone);
            if ((double)(n.QuadPart - due) * 1000.0 / f.QuadPart > 1.0) InterlockedIncrement(&g_subLate);
        } else InterlockedIncrement(&g_subDropped);
        RingLock lock;
        if (!g_handovers.empty()) g_handovers.pop_front();
    }
}

static void SyncSetMode(bool on) {
    static bool cur = false;
    if (on == cur) return;
    cur = on;
    RingLock lock;
    g_ring.SetAutoPump(!on);
    if (on && !g_subThread) {
        g_subEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_subThread = CreateThread(nullptr, 0, Submitter, nullptr, 0, nullptr);
    }
}

// ------------------------------------------------------------------ per-frame work
// 32-bit address space: the game exe is not large-address-aware (2 GB). Reports used/free and the largest free block,
// which is what D3D allocations fail on ("out of video memory").
static void LogAddressSpace(const char* when) {
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t p = 0x10000;
    size_t freeTotal = 0, largest = 0, committed = 0, reserved = 0;
    while (p < 0xFFFF0000u && VirtualQuery((void*)p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_FREE) { freeTotal += mbi.RegionSize; if (mbi.RegionSize > largest) largest = mbi.RegionSize; }
        else if (mbi.State == MEM_COMMIT) committed += mbi.RegionSize;
        else reserved += mbi.RegionSize;
        uintptr_t n = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (n <= p) break;
        p = n;
    }
    LOGI("address space (%s): committed %u MB, reserved %u MB, free %u MB, largest free block %u MB", when,
         (unsigned)(committed >> 20), (unsigned)(reserved >> 20), (unsigned)(freeTotal >> 20), (unsigned)(largest >> 20));
}
void LogMemory(const char* when) { LogAddressSpace(when); }

static void LogModules() {
    HMODULE mods[512];
    DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return;
    std::string line;
    for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 512; ++i) {
        wchar_t p[MAX_PATH];
        if (!GetModuleFileNameW(mods[i], p, MAX_PATH)) continue;
        const wchar_t* n = wcsrchr(p, L'\\');
        char b[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, n ? n + 1 : p, -1, b, sizeof(b), nullptr, nullptr);
        line += b; line += ' ';
    }
    LOGI("loaded modules: %s", line.c_str());
}

static void SaveScreenshot(IDirect3DDevice9* dev, const char* why) {
    static unsigned n = 0;
    IDirect3DSurface9* bb = nullptr;
    if (!(bb = GameBackBuffer(dev))) return;
    CreateDirectoryW((G().runDir + L"shots").c_str(), nullptr);
    wchar_t p[MAX_PATH];
    swprintf_s(p, L"%sshots\\shot_%03u_f%06ld_%04.0fs", G().runDir.c_str(), n++, G().frame, SecondsSinceStart());
    std::string note;
    bool ok = SaveSurfaceImage(dev, bb, p, &note);
    LOGI("screenshot (%s) %S: %s%s", why, wcsrchr(p, L'\\') + 1, note.c_str(), ok ? "" : " FAILED");
    bb->Release();
}

static thread_local bool t_inPresent = false;

// ------------------------------------------------------------------ the game's queries
// Statistics per query type (perf_diag), and query_nowait: the game's EVENT queries report "GPU done" immediately,
// so it doesn't wait for the GPU to finish a frame before starting the next one.
struct QStat { unsigned created = 0, issues = 0, gets = 0, notReady = 0; double waitMs = 0; };
static QStat g_qs[16];
static std::unordered_map<IDirect3DQuery9*, LONGLONG> g_qWaitStart;
static LARGE_INTEGER g_qpf{};
static const char* QTypeName(D3DQUERYTYPE t) {
    switch (t) {
    case D3DQUERYTYPE_EVENT: return "event";
    case D3DQUERYTYPE_OCCLUSION: return "occlusion";
    case D3DQUERYTYPE_TIMESTAMP: return "timestamp";
    case D3DQUERYTYPE_TIMESTAMPDISJOINT: return "disjoint";
    case D3DQUERYTYPE_TIMESTAMPFREQ: return "freq";
    default: return "other";
    }
}
static HRESULT STDMETHODCALLTYPE Hk_QIssue(IDirect3DQuery9* q, DWORD fl) {
    if (!InternalCall()) { unsigned t = (unsigned)q->GetType() & 15; g_qs[t].issues++; }
    return o_QIssue(q, fl);
}
static HRESULT STDMETHODCALLTYPE Hk_QGetData(IDirect3DQuery9* q, void* data, DWORD size, DWORD fl) {
    if (InternalCall()) return o_QGetData(q, data, size, fl);
    D3DQUERYTYPE type = q->GetType();
    if (type == D3DQUERYTYPE_EVENT && G().s.queryNoWait) {   // "the GPU is done" - never makes the game wait
        if (data && size >= sizeof(BOOL)) *(BOOL*)data = TRUE;
        g_qs[type & 15].gets++;
        return S_OK;
    }
    HRESULT hr = o_QGetData(q, data, size, fl);
    if (type == D3DQUERYTYPE_TIMESTAMP && G().s.queryNoWait && data && size == sizeof(UINT64)) {
        // The game spins on its GPU timestamps until the GPU has finished the frame. Answer with the latest value we
        // have instead of "not ready": its timings lag a frame, but CPU and GPU can overlap.
        static std::unordered_map<IDirect3DQuery9*, UINT64> last;
        if (hr == S_OK) last[q] = *(UINT64*)data;
        else if (hr == S_FALSE) {
            auto it = last.find(q);
            if (it != last.end()) { *(UINT64*)data = it->second; hr = S_OK; }
        }
    }
    QStat& s = g_qs[type & 15];
    s.gets++;
    if (G().s.perfDiag) {
        if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        auto it = g_qWaitStart.find(q);
        if (hr == S_FALSE) {
            s.notReady++;
            if (it == g_qWaitStart.end()) g_qWaitStart[q] = now.QuadPart;
        } else if (it != g_qWaitStart.end()) {
            s.waitMs += (double)(now.QuadPart - it->second) * 1000.0 / g_qpf.QuadPart;
            g_qWaitStart.erase(it);
        }
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_CreateQuery(IDirect3DDevice9* self, D3DQUERYTYPE t, IDirect3DQuery9** out) {
    HRESULT hr = o_CreateQuery(self, t, out);
    if (!InternalCall() && out && *out && SUCCEEDED(hr)) {
        g_qs[t & 15].created++;
        static int logged = 0;
        if (logged++ < 20) LOGI("game creates a %s query", QTypeName(t));
        static bool hooked = false;
        if (!hooked) {
            hooked = true;
            o_QIssue = (QIssue_t)HookVtable(*out, VT_QUERY_Issue, (void*)&Hk_QIssue);
            o_QGetData = (QGetData_t)HookVtable(*out, VT_QUERY_GetData, (void*)&Hk_QGetData);
            LOGI("query hooks %s", o_QIssue && o_QGetData ? "installed" : "FAILED");
        }
    }
    return hr;
}
static void QueryLogStatus(unsigned frames) {
    if (!G().s.perfDiag || !frames) return;
    for (int t = 0; t < 16; ++t) {
        QStat& s = g_qs[t];
        if (!s.issues && !s.gets) continue;
        LOGI("perf: game %s queries: %u created, per frame %.1f issued, %.1f GetData (%.1f not ready), waited %.2f ms",
             QTypeName((D3DQUERYTYPE)t), s.created, (double)s.issues / frames, (double)s.gets / frames, (double)s.notReady / frames,
             s.waitMs / frames);
        s.issues = s.gets = s.notReady = 0;
        s.waitMs = 0;
    }
}

// ------------------------------------------------------------------ GPU / CPU frame timing (perf_diag = 1)
// Per frame: GPU time from the frame's first draw to the end of its Present (timestamp queries, read 4 frames later
// without blocking), the GPU interval between frame ends, and the render thread's CPU time. GPU busy ~ interval means
// GPU-bound; busy well below the interval means the GPU waits for the CPU / sync points.
struct PerfQ { IDirect3DQuery9 *disj = nullptr, *freq = nullptr, *t0 = nullptr, *t1 = nullptr; bool started = false, issued = false; };
static PerfQ g_pq[4];
static int g_pqIdx = 0;
static UINT64 g_pLastT1 = 0;
static unsigned g_pN = 0, g_pDisjoint = 0;
static double g_pBusy = 0, g_pBusyMax = 0, g_pInterval = 0, g_pCpu = 0;
static ULONGLONG g_pLastCpu = 0;
static unsigned g_pCpuFrames = 0;

static void PerfFrameStart(IDirect3DDevice9* dev) {
    if (!G().s.perfDiag) return;
    PerfQ& q = g_pq[g_pqIdx];
    if (q.started || q.issued) return;   // already started, or the slot's results are not back yet
    Internal in;
    if (!q.t0) {
        if (FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &q.disj)) || FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &q.freq)) ||
            FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q.t0)) || FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q.t1))) {
            static bool warned = false;
            if (!warned) { warned = true; LOGW("perf: timestamp queries not supported"); }
            return;
        }
    }
    q.disj->Issue(D3DISSUE_BEGIN);
    q.t0->Issue(D3DISSUE_END);
    q.started = true;
}

static void PerfCollect() {
    Internal in;
    for (auto& q : g_pq) {
        if (!q.issued) continue;
        BOOL dj = TRUE; UINT64 fr = 0, a = 0, b = 0;
        if (q.disj->GetData(&dj, sizeof(dj), 0) != S_OK || q.freq->GetData(&fr, sizeof(fr), 0) != S_OK ||
            q.t0->GetData(&a, sizeof(a), 0) != S_OK || q.t1->GetData(&b, sizeof(b), 0) != S_OK) continue;
        q.issued = false;
        if (dj || !fr) { g_pDisjoint++; g_pLastT1 = 0; continue; }
        double busy = (double)(b - a) * 1000.0 / fr;
        g_pBusy += busy;
        if (busy > g_pBusyMax) g_pBusyMax = busy;
        if (g_pLastT1 && b > g_pLastT1) g_pInterval += (double)(b - g_pLastT1) * 1000.0 / fr;
        g_pLastT1 = b;
        g_pN++;
    }
}

static void PerfFrameEnd(IDirect3DDevice9* dev) {
    if (!G().s.perfDiag) return;
    {
        FILETIME c, e, k, u;
        if (GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) {
            ULONGLONG t = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
            if (g_pLastCpu) { g_pCpu += (t - g_pLastCpu) / 10000.0; g_pCpuFrames++; }
            g_pLastCpu = t;
        }
    }
    PerfQ& q = g_pq[g_pqIdx];
    if (q.started) {
        Internal in;
        q.t1->Issue(D3DISSUE_END);
        q.freq->Issue(D3DISSUE_END);
        q.disj->Issue(D3DISSUE_END);
        q.started = false;
        q.issued = true;
        g_pqIdx = (g_pqIdx + 1) & 3;
    }
    PerfCollect();
}

// Time spent inside D3D calls on the render thread (perf_diag): where the driver blocks.
enum CallTime { CT_DRAW, CT_RIGHT, CT_HUD, CT_CLEAR, CT_STRETCH, CT_PRESENT, CT_VR, CT_COUNT };
static const char* kCtName[CT_COUNT] = {"draws", "right-eye switch+draw", "HUD draws", "Clear", "StretchRect", "Present",
                                        "VR hand-over+sync"};
static LONGLONG g_ct[CT_COUNT] = {};
static unsigned g_ctN[CT_COUNT] = {};
static inline LONGLONG CtNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
struct CtScope {
    int c; LONGLONG t0;
    explicit CtScope(int cat) : c(cat), t0(G().s.perfDiag ? CtNow() : 0) {}
    ~CtScope() { if (t0) { g_ct[c] += CtNow() - t0; g_ctN[c]++; } }
};

static void PerfLogStatus(double wallMsPerFrame) {
    if (!G().s.perfDiag) return;
    {
        static LARGE_INTEGER f{};
        if (!f.QuadPart) QueryPerformanceFrequency(&f);
        static long lastFrame = 0;
        long fr = G().frame;
        double n = fr > lastFrame ? (double)(fr - lastFrame) : 1;
        lastFrame = fr;
        std::string s;
        char b[96];
        for (int i = 0; i < CT_COUNT; ++i) {
            sprintf_s(b, "%s %.2f ms (%.0f)%s", kCtName[i], g_ct[i] * 1000.0 / f.QuadPart / n, g_ctN[i] / n, i + 1 < CT_COUNT ? ", " : "");
            s += b;
        }
        for (int i = 0; i < CT_COUNT; ++i) { g_ct[i] = 0; g_ctN[i] = 0; }
        LOGI("perf: per frame inside D3D calls (calls): %s", s.c_str());
    }
    double n = g_pN ? g_pN : 1, nc = g_pCpuFrames ? g_pCpuFrames : 1;
    LOGI("perf: frame %.2f ms; GPU busy %.2f ms avg (max %.2f), GPU frame interval %.2f ms; render thread CPU %.2f ms "
         "(%u GPU samples, %u disjoint)",
         wallMsPerFrame, g_pBusy / n, g_pBusyMax, g_pN > 1 ? g_pInterval / (n - 1) : 0.0, g_pCpu / nc, g_pN, g_pDisjoint);
    g_pN = g_pDisjoint = g_pCpuFrames = 0;
    g_pBusy = g_pBusyMax = g_pInterval = g_pCpu = 0;
}

static void BeforePresent(IDirect3DDevice9* dev) {
    Trace("Present");
    BatchFlush(dev, BF_PRESENT);   // normally empty already (EndScene)
    g_renderThread = GetCurrentThreadId();
    MenuInit(dev);   // in-game settings menu (once)
    MenuUpdate();    // input + layout; drawn into the headset image in VrFrame
    Internal in;
    if (Capturing()) CaptureEnd(dev);
    LONG f = InterlockedIncrement(&G().frame);
    if (f == 1) LOGI("first Present (%.1f s after start)", SecondsSinceStart());
    st.drawsTotal += st.draws;

    // periodic status
    static DWORD lastTick = GetTickCount();
    static LONG lastFrame = 0;
    static unsigned long long lastDraws = 0;
    DWORD now = GetTickCount();
    if (now - lastTick >= 10000) {
        float sec = (now - lastTick) / 1000.f;
        LONG frames = f - lastFrame;
        LOGI("status: frame %ld, %.1f fps, %.0f draws/frame, vr frames %.1f/s (%u skipped: ring full), redirects tex %u (+%u static) cube %u vol %u vb %u ib %u, "
             "failures create %u update %u present %u",
             f, frames / sec, frames ? (double)(st.drawsTotal - lastDraws) / frames : 0.0, g_vrFrames / sec, g_vrSkipped, st.texRedirect,
             st.texRedirectStatic, st.cubeRedirect, st.volRedirect, st.vbRedirect, st.ibRedirect, st.createFail, st.updateFail, st.presentFail);
        PerfLogStatus(frames ? sec * 1000.0 / frames : 0.0);
        QueryLogStatus((unsigned)frames);
        lastTick = now; lastFrame = f; lastDraws = st.drawsTotal; g_vrFrames = g_vrSkipped = 0;
        StereoLogStatus();
        PacingLogStatus();
        PacingSyncLogStatus();
        CamFovLogStatus();
        BatchLogStatus();
        if (g_subDone || g_subDropped)
            LOGI("vr: phase lock submitted %ld frames (%ld more than 1 ms after their time: GPU not done), %ld dropped", g_subDone, g_subLate,
                 g_subDropped);
        g_subDone = g_subLate = g_subDropped = 0;
        if (g_rpFrames)
            LOGI("vr: reprojection data on %u frames, avg camera turn speed %.1f deg/s, %u camera cuts", g_rpFrames,
                 g_rpSpeed / g_rpFrames, g_rpCuts);
        g_rpFrames = g_rpCuts = 0;
        g_rpSpeed = 0;
        LogAddressSpace("status");
        if (G().s.managedStaging)
            LOGI("managed emulation: %u locks, %u staging surfaces (%u KB) alive, %u AddDirtyRect on DEFAULT ignored", g_emuLocks,
                 (unsigned)g_staging.size(), (unsigned)(g_stagingBytes >> 10), g_dirtyIgnored);
    }
    st.draws = 0;
    EmuTrim();
    if (f == 300) LogModules();
    if (f % 120 == 0) InstallInputHooks();   // XInput DLLs may load late

    InputTick();
    CamFovTick();
    if (G().s.recenterKey && (GetAsyncKeyState(G().s.recenterKey) & 1) && GetForegroundWindow() == G().gameWnd) xrb::ClientRequestRecenter();

    // screenshots
    static float nextShot = 0;
    static int intervalShots = 0;
    float t = SecondsSinceStart();
    if (G().s.screenshotInterval > 0 && t >= nextShot && intervalShots < G().s.screenshotMax) {
        if (nextShot > 0) { SaveScreenshot(dev, "interval"); intervalShots++; }
        nextShot = t + G().s.screenshotInterval;
    }
    if (g_wantShot) { g_wantShot = false; SaveScreenshot(dev, "requested"); }

    // Phase lock: this frame's hand-over time, the copy into an XrBridge slot, the submitter hands it over at that
    // time, and the game may run at most one frame ahead (exactly 60 frames per second).
    LONGLONG handover = 0;
    bool sync = PacingHeadsetSync(&handover);
    SyncSetMode(sync);
    if (sync) RecordContentTime(handover);
    else { LARGE_INTEGER q; QueryPerformanceCounter(&q); RecordContentTime(q.QuadPart); }
    if (VrFrame(dev) && sync) {
        { RingLock lock; g_handovers.push_back(handover); }
        SetEvent(g_subEvent);
    }
    if (sync) PacingSyncThrottle(handover);

    if (g_wantQuit || (G().s.quitAfter > 0 && t >= G().s.quitAfter)) {
        LOGI("quitting (%s) at frame %ld, %.1f s", g_wantQuit ? "script" : "quit_after", f, t);
        LogInputDevices();
        xrb::ClientShutdown();
        xrb::LogFlush();
        TerminateProcess(GetCurrentProcess(), 0);
    }
}

static void AfterPresent(IDirect3DDevice9* dev, HRESULT hr) {
    PerfFrameEnd(dev);
    if (FAILED(hr) && st.presentFail++ < 10) LOGW("Present returned %08lX", hr);
    {
        Internal in;
        StereoBeginFrame(dev);   // eye + head pose for the frame that starts now
    }
    LONG f = G().frame;
    if (g_wantCapture || (G().s.captureAtFrame > 0 && f == G().s.captureAtFrame)) {
        g_wantCapture = false;
        Internal in;
        CaptureBegin(dev, (unsigned)f);
    }
}

// ------------------------------------------------------------------ device hooks
// The swap chain the game presents through. FF XIII / XIII-2: the device's own. Lightning Returns: an additional one
// (the device's back buffer is 1x1). Everything that means "the game's back buffer" asks GameBackBuffer.
static IDirect3DSwapChain9* g_presentSC = nullptr;   // AddRef'd
IDirect3DSurface9* GameBackBuffer(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = nullptr;
    Internal in;
    if (g_presentSC) g_presentSC->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (!bb && dev) dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    return bb;
}
static void NotePresentSwapChain(IDirect3DSwapChain9* sc) {
    if (sc == g_presentSC) return;
    if (g_presentSC) g_presentSC->Release();
    g_presentSC = sc;
    if (sc) {
        sc->AddRef();
        D3DPRESENT_PARAMETERS pp{};
        { Internal in; sc->GetPresentParameters(&pp); }
        LOGI("the game presents through swap chain %p (%ux%u)", sc, pp.BackBufferWidth, pp.BackBufferHeight);
    }
}

static HRESULT STDMETHODCALLTYPE Hk_CreateAdditionalSwapChain(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* pp, IDirect3DSwapChain9** out) {
    if (InternalCall() || !pp) return o_CreateAdditionalSwapChain(self, pp, out);
    LogPP("CreateAdditionalSwapChain requested", pp);
    HRESULT hr = o_CreateAdditionalSwapChain(self, pp, out);
    LOGI("CreateAdditionalSwapChain -> %08lX, %p", hr, SUCCEEDED(hr) && out ? *out : nullptr);
    // a game whose device back buffer is tiny renders to this one: decide the 3D resolution before it makes targets
    if (SUCCEEDED(hr) && pp->BackBufferWidth > 16) RtScaleInit(pp->BackBufferWidth, pp->BackBufferHeight);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hk_Present(IDirect3DDevice9* self, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    if (InternalCall() || t_inPresent) return o_Present(self, a, b, w, r);
    t_inPresent = true;
    PacingPresentBegin();
    { CtScope ct(CT_VR); BeforePresent(self); }
    HRESULT hr;
    { CtScope ct(CT_PRESENT); hr = o_Present(self, a, b, w, r); }
    PacingPresentEnd();
    AfterPresent(self, hr);
    t_inPresent = false;
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hk_SCPresent(IDirect3DSwapChain9* self, const RECT* a, const RECT* b, HWND w, const RGNDATA* r, DWORD fl) {
    if (InternalCall() || t_inPresent) return o_SCPresent(self, a, b, w, r, fl);
    static bool logged = false;
    if (!logged) { logged = true; LOGI("game presents through IDirect3DSwapChain9::Present"); }
    IDirect3DDevice9* dev = nullptr;
    self->GetDevice(&dev);
    NotePresentSwapChain(self);
    t_inPresent = true;
    PacingPresentBegin();
    if (dev) { CtScope ct(CT_VR); BeforePresent(dev); }
    HRESULT hr;
    { CtScope ct(CT_PRESENT); hr = o_SCPresent(self, a, b, w, r, fl); }
    PacingPresentEnd();
    if (dev) { AfterPresent(dev, hr); dev->Release(); }
    t_inPresent = false;
    return hr;
}

// Shared by Reset and ResetEx (Lightning Returns starts with a 1x1 back buffer and ResetEx's it to the real size).
static void BeforeReset(D3DPRESENT_PARAMETERS* pp) {
    NotePresentSwapChain(nullptr);
    BsOnReset();   // drops a pending batch; the device state goes back to its defaults
    MenuOnLostDevice();
    { RingLock lock; g_ring.Destroy(); g_handovers.clear(); }   // our DEFAULT-pool atlases; recreated at the next Present
    g_ringW = g_ringH = 0;
    StereoOnReset();
    if (pp && !pp->Windowed && G().s.forceWindowed) MakeWindowed(pp, pp->hDeviceWindow ? pp->hDeviceWindow : g_devWnd);
}
static bool g_sizeAfterReset = false;
static void AfterReset(const D3DPRESENT_PARAMETERS* pp, HRESULT hr) {
    // a new back-buffer size: decide the 3D resolution again (the game recreates its targets after a reset)
    if (SUCCEEDED(hr) && pp && pp->BackBufferWidth > 16 && (pp->BackBufferWidth != RtBaseW() || pp->BackBufferHeight != RtBaseH()))
        RtScaleInit(pp->BackBufferWidth, pp->BackBufferHeight);
    // Lightning Returns renders at its own resolution setting (Configuration.ini Graphics_Resolution), which is not
    // the window's size when the window doesn't fit the desktop (dev PC: 3840x2160 targets, 3014x1695 window). The
    // first 16:9 render target it creates after the reset decides the game resolution (MaybeSizeFromTarget).
    if (SUCCEEDED(hr) && G().game == GAME_LR) g_sizeAfterReset = true;
}

static HRESULT STDMETHODCALLTYPE Hk_Reset(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* pp) {
    if (InternalCall()) return o_Reset(self, pp);
    LogPP("Reset", pp);
    BeforeReset(pp);
    HRESULT hr;
    if (pp && !pp->Windowed && g_devEx) {
        D3DDISPLAYMODEEX m = FullscreenMode(pp);
        Internal in;
        hr = g_devEx->ResetEx(pp, &m);
    } else hr = o_Reset(self, pp);
    LOGI("Reset -> %08lX", hr);
    AfterReset(pp, hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hk_ResetEx(IDirect3DDevice9Ex* self, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode) {
    if (InternalCall()) return o_ResetEx(self, pp, mode);
    LogPP("ResetEx", pp);
    BeforeReset(pp);
    if (pp && pp->Windowed) mode = nullptr;
    HRESULT hr;
    { Internal in; hr = o_ResetEx(self, pp, mode); }
    LOGI("ResetEx -> %08lX", hr);
    AfterReset(pp, hr);
    return hr;
}

static HRESULT CreateTextureImpl(IDirect3DDevice9* self, UINT w, UINT h, UINT lv, DWORD usage, D3DFORMAT f, D3DPOOL pool,
                                 IDirect3DTexture9** out, HANDLE* sh) {
    if (pool == D3DPOOL_MANAGED && g_devEx && !InternalCall() && G().s.managedStaging) {
        HRESULT hr = o_CreateTexture(self, w, h, lv, usage, f, D3DPOOL_DEFAULT, out, sh);
        if (SUCCEEDED(hr) && out && *out) { MarkEmulated(*out); st.texRedirect++; }
        else if (st.texFail++ < 20) LOGE("managed texture %ux%u %s levels %u usage %lx -> DEFAULT failed: %08lX", w, h, FormatName(f), lv, usage, hr);
        return hr;
    }
    if (pool == D3DPOOL_MANAGED && g_devEx && !InternalCall()) {
        if (st.texRedirect < 5) LOGD("managed texture %ux%u %s levels %u usage %lx -> DEFAULT+DYNAMIC", w, h, FormatName(f), lv, usage);
        HRESULT hr = o_CreateTexture(self, w, h, lv, usage | D3DUSAGE_DYNAMIC, f, D3DPOOL_DEFAULT, out, sh);
        if (SUCCEEDED(hr)) { st.texRedirect++; return hr; }
        HRESULT hr2 = o_CreateTexture(self, w, h, lv, usage, f, D3DPOOL_DEFAULT, out, sh);
        if (SUCCEEDED(hr2)) {
            if (st.texRedirectStatic++ < 10) LOGW("managed texture %ux%u %s usage %lx: DYNAMIC refused (%08lX), created static (not lockable)", w, h, FormatName(f), usage, hr);
            return hr2;
        }
        if (st.texFail++ < 20) LOGE("managed texture %ux%u %s levels %u usage %lx could not be redirected: %08lX", w, h, FormatName(f), lv, usage, hr2);
        return hr2;
    }
    HRESULT hr = o_CreateTexture(self, w, h, lv, usage, f, pool, out, sh);
    if (FAILED(hr) && st.createFail++ < 20) LOGE("CreateTexture %ux%u %s levels %u usage %lx pool %d failed %08lX", w, h, FormatName(f), lv, usage, (int)pool, hr);
    else if (SUCCEEDED(hr) && (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) && !InternalCall())
        LOGD("CreateTexture %ux%u %s usage %lx (render target) -> %p", w, h, FormatName(f), usage, out ? *out : nullptr);
    return hr;
}

// Diagnostics for "the 3D image is low resolution": render targets of at least 1/4 of the game's width that were not
// made bigger (the first few are logged).
// Lightning Returns creates its screen-size targets while the device and its window are still 1x1, before the Reset to
// the real size: the first big render target gives the game resolution.
static bool g_sizeFromFirstTarget = false;
static void MaybeSizeFromTarget(UINT w, UINT h) {
    if (g_sizeAfterReset && !InternalCall() && w >= 640 && h >= 360 && (UINT)abs((int)(w * 9) - (int)(h * 16)) <= 16) {
        g_sizeAfterReset = false;
        if (w != RtBaseW() || h != RtBaseH()) {
            LOGI("game resolution %ux%u from its first full-size render target after the reset (back buffer %ux%u)", w, h, RtBaseW(), RtBaseH());
            RtScaleInit(w, h);
        }
        return;
    }
    if (!g_sizeFromFirstTarget || InternalCall() || w < 640 || h < 360) return;
    g_sizeFromFirstTarget = false;
    LOGI("game resolution %ux%u from its first full-size render target", w, h);
    RtScaleInit(w, h);
}

static void NoteUnscaledTarget(UINT w, UINT h, D3DFORMAT f) {
    static int n = 0;
    if (w < 320 || n >= 12) return;
    n++;
    LOGI("render target %ux%u %s created at the game's size (scaling %s, game size %ux%u)", w, h, FormatName(f),
         RtScaleActive() ? "on" : "not decided/off", RtBaseW(), RtBaseH());
}

static HRESULT STDMETHODCALLTYPE Hk_CreateTexture(IDirect3DDevice9* self, UINT w, UINT h, UINT lv, DWORD usage, D3DFORMAT f, D3DPOOL pool,
                                                  IDirect3DTexture9** out, HANDLE* sh) {
    UINT ow = w, oh = h;
    if (usage & D3DUSAGE_RENDERTARGET) MaybeSizeFromTarget(w, h);
    bool scaled = !InternalCall() && (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) && RtScaleDims(ow, oh, &w, &h);
    HRESULT hr = CreateTextureImpl(self, w, h, lv, usage, f, pool, out, sh);
    if (!InternalCall()) Trace(scaled ? "CreateTexture(scaled)" : "CreateTexture", WH(ow, oh), (uintptr_t)f | ((uintptr_t)pool << 28), usage, hr);
    if (!InternalCall() && !scaled && (usage & D3DUSAGE_RENDERTARGET)) NoteUnscaledTarget(ow, oh, f);
    if (scaled && SUCCEEDED(hr) && out && *out) {
        RtMarkTexture(*out, ow, oh, w, h);
        LOGD("render target texture %ux%u %s created at %ux%u", ow, oh, FormatName(f), w, h);
    }
    if (SUCCEEDED(hr) && out) {
        HOOK_ONCE(*out, TexLockRect, VT_TEX_LockRect);
        HOOK_ONCE(*out, TexUnlockRect, VT_TEX_UnlockRect);
        HOOK_ONCE(*out, TexAddDirtyRect, VT_TEX_AddDirtyRect);
        HOOK_ONCE(*out, TexGetLevelDesc, VT_TEX_GetLevelDesc);
        static bool surfHooks = false;
        if (!surfHooks) {   // texture level surfaces (what emulated locks go through)
            IDirect3DSurface9* s = nullptr;
            { Internal in; (*out)->GetSurfaceLevel(0, &s); }
            if (s) {
                surfHooks = true;
                HOOK_ONCE(s, SurfGetDesc, VT_SURF_GetDesc);
                HOOK_ONCE(s, SurfLockRect, VT_SURF_LockRect);
                HOOK_ONCE(s, SurfUnlockRect, VT_SURF_UnlockRect);
                s->Release();
            }
        }
        if (!InternalCall()) StereoOnCreateTexture(self, *out, w, h, lv, usage, f, pool);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hk_CreateRenderTarget(IDirect3DDevice9* self, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD q,
                                                       BOOL lockable, IDirect3DSurface9** out, HANDLE* sh) {
    UINT ow = w, oh = h;
    MaybeSizeFromTarget(w, h);
    bool scaled = !InternalCall() && RtScaleDims(ow, oh, &w, &h);
    if (scaled && ms != D3DMULTISAMPLE_NONE && G().s.renderMsaa >= 0) {   // render_msaa for the resized scene targets
        ms = G().s.renderMsaa >= 2 ? (D3DMULTISAMPLE_TYPE)G().s.renderMsaa : D3DMULTISAMPLE_NONE;
        q = 0;
    }
    HRESULT hr = o_CreateRenderTarget(self, w, h, f, ms, q, lockable, out, sh);
    if (!InternalCall()) Trace(scaled ? "CreateRenderTarget(scaled)" : "CreateRenderTarget", WH(ow, oh), f, lockable, hr);
    if (scaled && SUCCEEDED(hr) && out && *out) RtMarkSurface(*out, ow, oh, w, h);
    if (!InternalCall() && !scaled) NoteUnscaledTarget(ow, oh, f);
    if (!InternalCall()) {
        LOGD("CreateRenderTarget %ux%u %s msaa %d/%lu (created %ux%u) -> %08lX", ow, oh, FormatName(f), (int)ms, q, w, h, hr);
        if (SUCCEEDED(hr) && out) StereoOnCreateSurface(self, *out, false, w, h, f, ms, q, lockable);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hk_CreateDepthStencilSurface(IDirect3DDevice9* self, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms,
                                                              DWORD q, BOOL discard, IDirect3DSurface9** out, HANDLE* sh) {
    UINT ow = w, oh = h;
    bool scaled = !InternalCall() && RtScaleDims(ow, oh, &w, &h);
    if (scaled && ms != D3DMULTISAMPLE_NONE && G().s.renderMsaa >= 0) {   // must match the colour target's sample count
        ms = G().s.renderMsaa >= 2 ? (D3DMULTISAMPLE_TYPE)G().s.renderMsaa : D3DMULTISAMPLE_NONE;
        q = 0;
    }
    HRESULT hr = o_CreateDepthStencilSurface(self, w, h, f, ms, q, discard, out, sh);
    if (!InternalCall()) Trace(scaled ? "CreateDepthStencil(scaled)" : "CreateDepthStencil", WH(ow, oh), f, ms, hr);
    if (scaled && SUCCEEDED(hr) && out && *out) RtMarkSurface(*out, ow, oh, w, h);
    if (!InternalCall() && !scaled) NoteUnscaledTarget(ow, oh, f);
    if (!InternalCall()) {
        LOGD("CreateDepthStencilSurface %ux%u %s msaa %d/%lu (created %ux%u) -> %08lX", ow, oh, FormatName(f), (int)ms, q, w, h, hr);
        if (SUCCEEDED(hr) && out) StereoOnCreateSurface(self, *out, true, w, h, f, ms, q, discard);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hk_CreateVolumeTexture(IDirect3DDevice9* self, UINT w, UINT h, UINT d, UINT lv, DWORD usage, D3DFORMAT f,
                                                        D3DPOOL pool, IDirect3DVolumeTexture9** out, HANDLE* sh) {
    if (pool == D3DPOOL_MANAGED && g_devEx && !InternalCall()) {
        HRESULT hr = o_CreateVolumeTexture(self, w, h, d, lv, usage | D3DUSAGE_DYNAMIC, f, D3DPOOL_DEFAULT, out, sh);
        if (FAILED(hr)) hr = o_CreateVolumeTexture(self, w, h, d, lv, usage, f, D3DPOOL_DEFAULT, out, sh);
        if (SUCCEEDED(hr)) { st.volRedirect++; HOOK_ONCE(*out, VolLockBox, VT_VOL_LockBox); }
        else if (st.texFail++ < 20) LOGE("managed volume texture %ux%ux%u %s could not be redirected: %08lX", w, h, d, FormatName(f), hr);
        return hr;
    }
    HRESULT hr = o_CreateVolumeTexture(self, w, h, d, lv, usage, f, pool, out, sh);
    if (SUCCEEDED(hr) && out) HOOK_ONCE(*out, VolLockBox, VT_VOL_LockBox);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hk_CreateCubeTexture(IDirect3DDevice9* self, UINT e, UINT lv, DWORD usage, D3DFORMAT f, D3DPOOL pool,
                                                      IDirect3DCubeTexture9** out, HANDLE* sh) {
    if (pool == D3DPOOL_MANAGED && g_devEx && !InternalCall()) {
        bool staging = G().s.managedStaging;
        HRESULT hr = o_CreateCubeTexture(self, e, lv, staging ? usage : usage | D3DUSAGE_DYNAMIC, f, D3DPOOL_DEFAULT, out, sh);
        if (FAILED(hr) && !staging) hr = o_CreateCubeTexture(self, e, lv, usage, f, D3DPOOL_DEFAULT, out, sh);
        if (SUCCEEDED(hr)) {
            st.cubeRedirect++;
            if (staging) MarkEmulated(*out);
            HOOK_ONCE(*out, CubeLockRect, VT_CUBE_LockRect);
            HOOK_ONCE(*out, CubeUnlockRect, VT_CUBE_UnlockRect);
            HOOK_ONCE(*out, CubeAddDirtyRect, VT_CUBE_AddDirtyRect);
            HOOK_ONCE(*out, CubeGetLevelDesc, VT_CUBE_GetLevelDesc);
        }
        else if (st.texFail++ < 20) LOGE("managed cube texture %u %s could not be redirected: %08lX", e, FormatName(f), hr);
        return hr;
    }
    HRESULT hr = o_CreateCubeTexture(self, e, lv, usage, f, pool, out, sh);
    if (SUCCEEDED(hr) && out) {
        HOOK_ONCE(*out, CubeLockRect, VT_CUBE_LockRect);
        HOOK_ONCE(*out, CubeUnlockRect, VT_CUBE_UnlockRect);
        HOOK_ONCE(*out, CubeAddDirtyRect, VT_CUBE_AddDirtyRect);
        HOOK_ONCE(*out, CubeGetLevelDesc, VT_CUBE_GetLevelDesc);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hk_CreateVertexBuffer(IDirect3DDevice9* self, UINT len, DWORD usage, DWORD fvf, D3DPOOL pool,
                                                       IDirect3DVertexBuffer9** out, HANDLE* sh) {
    if (pool == D3DPOOL_MANAGED && g_devEx && !InternalCall()) {
        pool = D3DPOOL_DEFAULT;
        st.vbRedirect++;
    }
    HRESULT hr = o_CreateVertexBuffer(self, len, usage, fvf, pool, out, sh);
    if (FAILED(hr) && st.createFail++ < 20) LOGE("CreateVertexBuffer %u usage %lx pool %d failed %08lX", len, usage, (int)pool, hr);
    if (SUCCEEDED(hr) && out) HOOK_ONCE(*out, VBLock, VT_VB_Lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hk_CreateIndexBuffer(IDirect3DDevice9* self, UINT len, DWORD usage, D3DFORMAT f, D3DPOOL pool,
                                                      IDirect3DIndexBuffer9** out, HANDLE* sh) {
    if (pool == D3DPOOL_MANAGED && g_devEx && !InternalCall()) {
        pool = D3DPOOL_DEFAULT;
        st.ibRedirect++;
    }
    HRESULT hr = o_CreateIndexBuffer(self, len, usage, f, pool, out, sh);
    if (FAILED(hr) && st.createFail++ < 20) LOGE("CreateIndexBuffer %u usage %lx pool %d failed %08lX", len, usage, (int)pool, hr);
    if (SUCCEEDED(hr) && out) HOOK_ONCE(*out, IBLock, VT_IB_Lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hk_UpdateSurface(IDirect3DDevice9* self, IDirect3DSurface9* s, const RECT* r, IDirect3DSurface9* d, const POINT* p);
static HRESULT STDMETHODCALLTYPE Hk_UpdateTexture(IDirect3DDevice9* self, IDirect3DBaseTexture9* s, IDirect3DBaseTexture9* d) {
    if (!InternalCall()) BatchFlush(self, BF_UPDATE);
    if (!InternalCall() && s && d && IsEmulated(d) && s->GetType() == d->GetType()) {   // per level, like UpdateSurface
        bool cube = d->GetType() == D3DRTYPE_CUBETEXTURE;
        UINT levels = s->GetLevelCount() < d->GetLevelCount() ? s->GetLevelCount() : d->GetLevelCount();
        HRESULT hr = D3D_OK;
        for (int f = 0; f < (cube ? 6 : 1) && SUCCEEDED(hr); ++f)
            for (UINT l = 0; l < levels && SUCCEEDED(hr); ++l) {
                IDirect3DSurface9* ss = LevelSurface(s, cube, f, l);
                IDirect3DSurface9* ds = LevelSurface(d, cube, f, l);
                hr = ss && ds ? Hk_UpdateSurface(self, ss, nullptr, ds, nullptr) : D3DERR_INVALIDCALL;
                if (ss) ss->Release();
                if (ds) ds->Release();
            }
        return hr;
    }
    HRESULT hr = o_UpdateTexture(self, s, d);
    if (FAILED(hr) && st.updateFail++ < 20) LOGE("UpdateTexture %p -> %p failed %08lX", s, d, hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hk_UpdateSurface(IDirect3DDevice9* self, IDirect3DSurface9* s, const RECT* r, IDirect3DSurface9* d, const POINT* p) {
    if (!InternalCall()) BatchFlush(self, BF_UPDATE);
    static int logged = 0;
    if (!InternalCall() && logged < 60 && s && d) {
        logged++;
        D3DSURFACE_DESC a{}, b{};
        { Internal in; s->GetDesc(&a); d->GetDesc(&b); }   // real pools
        LOGI("UpdateSurface %ux%u %s pool %d usage %lx%s -> %ux%u %s pool %d usage %lx%s, rect %s, point %s", a.Width, a.Height, FormatName(a.Format),
             (int)a.Pool, a.Usage, IsEmulated(s) ? " EMULATED" : "", b.Width, b.Height, FormatName(b.Format), (int)b.Pool, b.Usage,
             IsEmulated(d) ? " EMULATED" : "", r ? "yes" : "null", p ? "yes" : "null");
        xrb::LogFlush();
    }
    // D3DX uploads into "managed" textures with UpdateSurface too; with an emulated destination the D3D9Ex runtime
    // crashes inside UpdateSurface. Do it ourselves: copy the source rows into the staging level and upload that.
    EmuInfo e;
    if (!InternalCall() && s && d && GetEmu(d, &e)) {
        D3DSURFACE_DESC sd{};
        { Internal in; s->GetDesc(&sd); }
        bool block = sd.Format == D3DFMT_DXT1 || sd.Format == D3DFMT_DXT2 || sd.Format == D3DFMT_DXT3 || sd.Format == D3DFMT_DXT4 ||
                     sd.Format == D3DFMT_DXT5;
        RECT sr = r ? *r : RECT{0, 0, (LONG)sd.Width, (LONG)sd.Height};
        POINT dp = p ? *p : POINT{0, 0};
        RECT dr{dp.x, dp.y, dp.x + (sr.right - sr.left), dp.y + (sr.bottom - sr.top)};
        UINT w = sr.right - sr.left, h = sr.bottom - sr.top;
        size_t rowBytes = block ? SurfBytes(w, 4, sd.Format) : SurfBytes(w, 1, sd.Format);
        UINT rows = block ? (h + 3) / 4 : h;
        if (!block && sd.Format != D3DFMT_A8R8G8B8 && sd.Format != D3DFMT_X8R8G8B8 && sd.Format != D3DFMT_A8B8G8R8) {
            static int warned = 0;
            if (warned++ < 5) LOGW("UpdateSurface into emulated texture: format %s copied as 4 bytes/pixel", FormatName(sd.Format));
        }
        D3DLOCKED_RECT ls{}, ld{};
        HRESULT hr = s->LockRect(&ls, &sr, D3DLOCK_READONLY);
        if (FAILED(hr)) { if (st.updateFail++ < 20) LOGE("UpdateSurface emulation: source lock failed %08lX", hr); return hr; }
        hr = EmuLock(e, &ld, &dr, 0);
        if (SUCCEEDED(hr)) {
            for (UINT y = 0; y < rows; ++y) memcpy((uint8_t*)ld.pBits + (size_t)y * ld.Pitch, (const uint8_t*)ls.pBits + (size_t)y * ls.Pitch, rowBytes);
            hr = EmuUnlock(e);
        }
        s->UnlockRect();
        static unsigned n = 0;
        if (n++ < 3) LOGI("UpdateSurface into emulated texture (%ux%u %s) done ourselves: %08lX", w, h, FormatName(sd.Format), hr);
        return hr;
    }
    HRESULT hr = o_UpdateSurface(self, s, r, d, p);
    if (FAILED(hr) && st.updateFail++ < 20) LOGE("UpdateSurface %p -> %p failed %08lX", s, d, hr);
    return hr;
}

// Reading back a resized render target into a system-memory surface of the game's (original) size: downscale it
// into a temporary target of that size first.
static HRESULT STDMETHODCALLTYPE Hk_GetRenderTargetData(IDirect3DDevice9* self, IDirect3DSurface9* s, IDirect3DSurface9* d) {
    if (InternalCall() || !s || !d) return o_GetRenderTargetData(self, s, d);
    BatchFlush(self, BF_COPY);
    Trace("GetRenderTargetData", (uintptr_t)s, (uintptr_t)d, RtGetInfo(s, nullptr));
    ScaleInfo si;
    static int logged = 0;
    D3DSURFACE_DESC a{}, b{};
    { Internal in; s->GetDesc(&a); d->GetDesc(&b); }
    if (logged < 10) {
        logged++;
        LOGI("GetRenderTargetData %ux%u %s -> %ux%u %s (source resized: %d)", a.Width, a.Height, FormatName(a.Format), b.Width, b.Height,
             FormatName(b.Format), (int)RtGetInfo(s, nullptr));
    }
    if (RtGetInfo(s, &si) && (a.Width != b.Width || a.Height != b.Height)) {
        Internal in;
        IDirect3DSurface9* tmp = nullptr;
        HRESULT hr = self->CreateRenderTarget(b.Width, b.Height, a.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &tmp, nullptr);
        if (SUCCEEDED(hr)) hr = o_StretchRect(self, s, nullptr, tmp, nullptr, D3DTEXF_LINEAR);
        if (SUCCEEDED(hr)) hr = o_GetRenderTargetData(self, tmp, d);
        if (tmp) tmp->Release();
        if (FAILED(hr)) LOGE("GetRenderTargetData of a resized target (%ux%u -> %ux%u) failed %08lX", a.Width, a.Height, b.Width, b.Height, hr);
        return hr;
    }
    HRESULT hr = o_GetRenderTargetData(self, s, d);
    if (FAILED(hr)) LOGE("GetRenderTargetData %ux%u -> %ux%u failed %08lX", a.Width, a.Height, b.Width, b.Height, hr);
    return hr;
}

// ColorFill rects are in the game's units of that surface.
static HRESULT STDMETHODCALLTYPE Hk_ColorFill(IDirect3DDevice9* self, IDirect3DSurface9* s, const RECT* r, D3DCOLOR c) {
    ScaleInfo si;
    if (!InternalCall()) { BatchFlush(self, BF_COPY); Trace("ColorFill", (uintptr_t)s, r ? 1 : 0, RtGetInfo(s, nullptr)); }
    if (!InternalCall() && r && RtGetInfo(s, &si)) { RECT x = RtScaleRect(*r, si); return o_ColorFill(self, s, &x, c); }
    return o_ColorFill(self, s, r, c);
}

// ---- state tracking (cheap unless a capture is running)
static HRESULT STDMETHODCALLTYPE Hk_SetRenderTarget(IDirect3DDevice9* self, DWORD i, IDirect3DSurface9* s) {
    if (InternalCall()) return o_SetRenderTarget(self, i, s);
    if (Capturing()) { Internal in; CapSetRenderTarget(self, i, s); }
    if (StereoBoundRT(i) != s) BatchFlush(self, BF_TARGET);   // the pass ends: draw the recorded right eye into its targets
    if (i == 0) RtOnSetRT0(s);
    StereoOnSetRT(self, i, s);
    HRESULT hr = o_SetRenderTarget(self, i, s);
    if (i == 0 && s && SUCCEEDED(hr)) {   // the device reset viewport and scissor to the whole target
        D3DSURFACE_DESC d{};
        { Internal in; s->GetDesc(&d); }
        BsOnSetRT0(d.Width, d.Height);
    }
    Trace("SetRenderTarget", i, (uintptr_t)s, RtGetInfo(s, nullptr), hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_SetDepthStencilSurface(IDirect3DDevice9* self, IDirect3DSurface9* s) {
    if (InternalCall()) return o_SetDepthStencilSurface(self, s);
    if (Capturing()) { Internal in; CapSetDepthStencil(s); }
    if (StereoBoundDS() != s) BatchFlush(self, BF_DEPTH);
    StereoOnSetDS(s);
    HRESULT hr = o_SetDepthStencilSurface(self, s);
    Trace("SetDepthStencil", (uintptr_t)s, RtGetInfo(s, nullptr), 0, hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_Clear(IDirect3DDevice9* self, DWORD n, const D3DRECT* r, DWORD fl, D3DCOLOR c, float z, DWORD s) {
    if (InternalCall()) return o_Clear(self, n, r, fl, c, z, s);
    if (Capturing()) { Internal in; CapClear(n, r, fl, c, z, s); }
    ScaleInfo si;
    std::vector<D3DRECT> sr;
    if (n && r && RtRT0Scaled(&si)) {   // rects are in the game's (original) pixel units
        for (DWORD k = 0; k < n; ++k) {
            RECT a = RtScaleRect(RECT{r[k].x1, r[k].y1, r[k].x2, r[k].y2}, si);
            sr.push_back(D3DRECT{a.left, a.top, a.right, a.bottom});
        }
        r = sr.data();
    }
    HRESULT hr;
    { CtScope ct(CT_CLEAR); hr = o_Clear(self, n, r, fl, c, z, s); }
    Trace("Clear", n, fl, RtRT0Scaled(nullptr), hr);
    if (BatchEnabled() && StereoClearWouldMirror()) BatchRecordClear(self, n, r, fl, c, z, s);
    else StereoAfterClear(self, n, r, fl, c, z, s);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_SetViewport(IDirect3DDevice9* self, const D3DVIEWPORT9* vp) {
    if (InternalCall() || !vp) return o_SetViewport(self, vp);
    StereoOnViewport(vp);   // game units
    if (Capturing()) { Internal in; CapViewport(vp); }
    ScaleInfo si;
    Trace("SetViewport", WH(vp->X, vp->Y), WH(vp->Width, vp->Height), RtRT0Scaled(nullptr));
    D3DVIEWPORT9 v = RtRT0Scaled(&si) ? RtScaleViewport(*vp, si) : *vp;
    BsViewport(v);
    return o_SetViewport(self, &v);
}
static HRESULT STDMETHODCALLTYPE Hk_SetScissorRect(IDirect3DDevice9* self, const RECT* r) {
    if (InternalCall() || !r) return o_SetScissorRect(self, r);
    StereoOnScissor(r);     // game units
    if (Capturing()) { Internal in; CapScissor(r); }
    ScaleInfo si;
    RECT s = RtRT0Scaled(&si) ? RtScaleRect(*r, si) : *r;
    BsScissor(s);
    return o_SetScissorRect(self, &s);
}
static HRESULT STDMETHODCALLTYPE Hk_SetTexture(IDirect3DDevice9* self, DWORD stage, IDirect3DBaseTexture9* t) {
    if (InternalCall()) return o_SetTexture(self, stage, t);
    if (Capturing()) { Internal in; CapTexture(stage, t); }
    StereoOnSetTexture(stage, t);
    BsTexture(stage, t);
    return o_SetTexture(self, stage, t);
}
static HRESULT STDMETHODCALLTYPE Hk_SetRenderState(IDirect3DDevice9* self, D3DRENDERSTATETYPE s, DWORD v) {
    if (!InternalCall() && (unsigned)s < 256) {
        Shadow& sh = Sh();
        if (Capturing()) { Internal in; CapRenderState(s, v, sh.rs[s], sh.rsKnown[s]); }
        sh.rs[s] = v; sh.rsKnown[s] = true;
        BsRenderState(s, v);
    }
    return o_SetRenderState(self, s, v);
}
static HRESULT STDMETHODCALLTYPE Hk_SetStreamSource(IDirect3DDevice9* self, UINT n, IDirect3DVertexBuffer9* vb, UINT off, UINT stride) {
    if (!InternalCall()) {
        if (Capturing()) { Internal in; CapStream(n, vb, off, stride); }
        BsStream(n, vb, off, stride);
    }
    return o_SetStreamSource(self, n, vb, off, stride);
}
static HRESULT STDMETHODCALLTYPE Hk_SetVertexDeclaration(IDirect3DDevice9* self, IDirect3DVertexDeclaration9* d) {
    if (!InternalCall()) {
        if (Capturing()) { Internal in; CapDecl(d); }
        BsDecl(d);
    }
    return o_SetVertexDeclaration(self, d);
}
// ---- the rest of the device state, shadowed for the native right-eye batches (stereobatch.cpp)
static HRESULT STDMETHODCALLTYPE Hk_SetSamplerState(IDirect3DDevice9* self, DWORD sm, D3DSAMPLERSTATETYPE t, DWORD v) {
    if (!InternalCall()) BsSamplerState(sm, t, v);
    return o_SetSamplerState(self, sm, t, v);
}
static HRESULT STDMETHODCALLTYPE Hk_SetTextureStageState(IDirect3DDevice9* self, DWORD st, D3DTEXTURESTAGESTATETYPE t, DWORD v) {
    if (!InternalCall()) BsTextureStageState(st, t, v);
    return o_SetTextureStageState(self, st, t, v);
}
static HRESULT STDMETHODCALLTYPE Hk_SetIndices(IDirect3DDevice9* self, IDirect3DIndexBuffer9* ib) {
    if (!InternalCall()) BsIndices(ib);
    return o_SetIndices(self, ib);
}
static HRESULT STDMETHODCALLTYPE Hk_SetFVF(IDirect3DDevice9* self, DWORD fvf) {
    if (!InternalCall()) BsFVF(fvf);
    return o_SetFVF(self, fvf);
}
static HRESULT STDMETHODCALLTYPE Hk_SetStreamSourceFreq(IDirect3DDevice9* self, UINT n, UINT div) {
    if (!InternalCall()) BsStreamFreq(n, div);
    return o_SetStreamSourceFreq(self, n, div);
}
static HRESULT STDMETHODCALLTYPE Hk_SetVertexShaderConstantI(IDirect3DDevice9* self, UINT start, const int* d, UINT n) {
    if (!InternalCall()) BsVSConstI(start, d, n);
    return o_SetVertexShaderConstantI(self, start, d, n);
}
static HRESULT STDMETHODCALLTYPE Hk_SetVertexShaderConstantB(IDirect3DDevice9* self, UINT start, const BOOL* d, UINT n) {
    if (!InternalCall()) BsVSConstB(start, d, n);
    return o_SetVertexShaderConstantB(self, start, d, n);
}
static HRESULT STDMETHODCALLTYPE Hk_SetPixelShaderConstantI(IDirect3DDevice9* self, UINT start, const int* d, UINT n) {
    if (!InternalCall()) BsPSConstI(start, d, n);
    return o_SetPixelShaderConstantI(self, start, d, n);
}
static HRESULT STDMETHODCALLTYPE Hk_SetPixelShaderConstantB(IDirect3DDevice9* self, UINT start, const BOOL* d, UINT n) {
    if (!InternalCall()) BsPSConstB(start, d, n);
    return o_SetPixelShaderConstantB(self, start, d, n);
}
static HRESULT STDMETHODCALLTYPE Hk_SetClipPlane(IDirect3DDevice9* self, DWORD i, const float* p) {
    if (!InternalCall()) BsClipPlane(i, p);
    return o_SetClipPlane(self, i, p);
}
static HRESULT STDMETHODCALLTYPE Hk_BeginScene(IDirect3DDevice9* self) {
    HRESULT hr = o_BeginScene(self);
    if (!InternalCall() && SUCCEEDED(hr)) BsInScene(true);
    return hr;
}
// The batch stays open across the game's scene ends (FF XIII ends a scene per pass); it is drawn at the end of its pass.
static HRESULT STDMETHODCALLTYPE Hk_EndScene(IDirect3DDevice9* self) {
    if (!InternalCall()) BsInScene(false);
    return o_EndScene(self);
}
static HRESULT STDMETHODCALLTYPE Hk_StretchRect(IDirect3DDevice9* self, IDirect3DSurface9* s, const RECT* sr, IDirect3DSurface9* d, const RECT* dr,
                                                D3DTEXTUREFILTERTYPE f) {
    if (InternalCall()) return o_StretchRect(self, s, sr, d, dr, f);
    if (Capturing()) { Internal in; CapStretch(s, sr, d, dr, f); }
    BatchFlush(self, BF_COPY);
    StereoOnStretchRect(self, s, sr, d, dr);
    // rects are in the game's (original) pixel units of each surface
    ScaleInfo si;
    RECT a, b;
    const RECT* osr = sr;
    const RECT* odr = dr;
    if (sr && RtGetInfo(s, &si)) { a = RtScaleRect(*sr, si); sr = &a; }
    if (dr && RtGetInfo(d, &si)) { b = RtScaleRect(*dr, si); dr = &b; }
    // a scaled scene copied to the (unscaled) back buffer is a downscale: filter it
    if (f == D3DTEXF_POINT && RtScaleActive() && RtGetInfo(s, nullptr) && !RtGetInfo(d, nullptr)) f = D3DTEXF_LINEAR;
    HRESULT hr;
    { CtScope ct(CT_STRETCH); hr = o_StretchRect(self, s, sr, d, dr, f); }
    Trace("StretchRect", (uintptr_t)s, (uintptr_t)d, (sr ? 1 : 0) | (dr ? 2 : 0) | (RtGetInfo(s, nullptr) ? 4 : 0) | (RtGetInfo(d, nullptr) ? 8 : 0), hr);
    static int failLog = 0;
    if (FAILED(hr) && failLog++ < 10) {
        D3DSURFACE_DESC x{}, y{};
        { Internal in; s->GetDesc(&x); d->GetDesc(&y); }
        auto R = [](const RECT* r, char* buf) { if (r) sprintf_s(buf, 64, "%ld,%ld-%ld,%ld", r->left, r->top, r->right, r->bottom); else strcpy_s(buf, 64, "full"); };
        char r1[64], r2[64], r3[64], r4[64];
        R(osr, r1); R(sr, r2); R(odr, r3); R(dr, r4);
        LOGE("StretchRect failed %08lX: %ux%u %s usage %lx pool %d [%s -> %s] -> %ux%u %s usage %lx pool %d [%s -> %s], filter %d", hr, x.Width,
             x.Height, FormatName(x.Format), x.Usage, (int)x.Pool, r1, r2, y.Width, y.Height, FormatName(y.Format), y.Usage, (int)y.Pool, r3, r4, (int)f);
    }
    StereoAfterStretchRect(self, s, sr, d, dr, f);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hk_CreateVertexShader(IDirect3DDevice9* self, const DWORD* fn, IDirect3DVertexShader9** out) {
    std::vector<DWORD> patched;
    bool ray = !InternalCall() && fn && StereoPatchVSCode(fn, &patched);
    HRESULT hr = o_CreateVertexShader(self, ray ? patched.data() : fn, out);
    if (ray && FAILED(hr)) {
        LOGW("stereo: patched vertex shader rejected (0x%08lX) - created unchanged", (unsigned long)hr);
        ray = false;
        hr = o_CreateVertexShader(self, fn, out);
    }
    if (SUCCEEDED(hr) && out && *out && !InternalCall()) { RegisterShader(*out, false, fn); StereoRegisterVS(*out, fn, ray); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_CreatePixelShader(IDirect3DDevice9* self, const DWORD* fn, IDirect3DPixelShader9** out) {
    HRESULT hr = o_CreatePixelShader(self, fn, out);
    if (SUCCEEDED(hr) && out && *out && !InternalCall()) { RegisterShader(*out, true, fn); StereoRegisterPS(*out, fn); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_SetVertexShader(IDirect3DDevice9* self, IDirect3DVertexShader9* s) {
    if (!InternalCall()) {
        StereoOnSetVS(s);
        BsVertexShader(s);
        if (Capturing()) { Internal in; CapShader(false, s); }
    }
    return o_SetVertexShader(self, s);
}
static HRESULT STDMETHODCALLTYPE Hk_SetPixelShader(IDirect3DDevice9* self, IDirect3DPixelShader9* s) {
    if (!InternalCall()) {
        StereoOnSetPS(s);
        BsPixelShader(s);
        if (Capturing()) { Internal in; CapShader(true, s); }
    }
    return o_SetPixelShader(self, s);
}
static HRESULT STDMETHODCALLTYPE Hk_SetVertexShaderConstantF(IDirect3DDevice9* self, UINT start, const float* d, UINT n) {
    if (!InternalCall() && d && start < 256) {
        UINT m = start + n > 256 ? 256 - start : n;
        memcpy(Sh().vsc[start], d, m * 16);
        BsVSConstF(start, d, m);
        if (Capturing()) { Internal in; CapConstF(false, start, d, m); }
    }
    return o_SetVertexShaderConstantF(self, start, d, n);
}
static HRESULT STDMETHODCALLTYPE Hk_SetPixelShaderConstantF(IDirect3DDevice9* self, UINT start, const float* d, UINT n) {
    if (!InternalCall() && d && start < 224) {
        UINT m = start + n > 224 ? 224 - start : n;
        memcpy(Sh().psc[start], d, m * 16);
        BsPSConstF(start, d, m);
        if (Capturing()) { Internal in; CapConstF(true, start, d, m); }
    }
    return o_SetPixelShaderConstantF(self, start, d, n);
}

// Every game draw goes through StereoPreDraw (eye matrices) and, for UI draws, is repeated into the HUD texture.
// Native: the right eye is recorded for the pass's batch (native_batch = 1) or drawn right away.
#define STEREO_DRAW(call, post, ...)                                                                  \
    PerfFrameStart(self);                                                                       \
    int sm = StereoPreDraw(self);                                                               \
    HRESULT hr;                                                                                 \
    { CtScope ct(CT_DRAW); hr = call; }                                                         \
    if (sm == SD_NATIVE) {                                                                      \
        CtScope ct(CT_RIGHT);                                                                   \
        if (BatchEnabled()) { BatchDraw bd = __VA_ARGS__; BatchRecordDraw(self, bd); }          \
        else if (StereoRightBegin(self)) { { Internal in; call; } StereoRightEnd(self); }      \
    }                                                                                           \
    if (sm == SD_HUD) { CtScope ct(CT_HUD); if (StereoHudBegin(self)) { { Internal in; call; } StereoHudEnd(self); } } \
    StereoPostDraw(self, sm);                                                                   \
    post;                                                                                       \
    return hr;

static HRESULT STDMETHODCALLTYPE Hk_DrawPrimitive(IDirect3DDevice9* self, D3DPRIMITIVETYPE t, UINT sv, UINT pc) {
    if (InternalCall()) return o_DrawPrimitive(self, t, sv, pc);
    st.draws++;
    if (Capturing()) { Internal in; CapDraw("DP", t, sv, 0, 0, 0, pc); }
    STEREO_DRAW(o_DrawPrimitive(self, t, sv, pc), (void)0, {0, t, sv, pc, 0, 0, 0, nullptr, D3DFMT_UNKNOWN, nullptr})
}
static HRESULT STDMETHODCALLTYPE Hk_DrawIndexedPrimitive(IDirect3DDevice9* self, D3DPRIMITIVETYPE t, INT bv, UINT mi, UINT nv, UINT si, UINT pc) {
    if (InternalCall()) return o_DrawIndexedPrimitive(self, t, bv, mi, nv, si, pc);
    st.draws++;
    if (Capturing()) { Internal in; CapDraw("DIP", t, (UINT)bv, mi, nv, si, pc); }
    STEREO_DRAW(o_DrawIndexedPrimitive(self, t, bv, mi, nv, si, pc), (void)0, {1, t, mi, nv, si, pc, bv, nullptr, D3DFMT_UNKNOWN, nullptr})
}
static HRESULT STDMETHODCALLTYPE Hk_DrawPrimitiveUP(IDirect3DDevice9* self, D3DPRIMITIVETYPE t, UINT pc, const void* v, UINT stride) {
    if (InternalCall()) return o_DrawPrimitiveUP(self, t, pc, v, stride);
    st.draws++;
    if (Capturing()) { Internal in; CapDraw("DPUP", t, stride, 0, 0, 0, pc); }
    STEREO_DRAW(o_DrawPrimitiveUP(self, t, pc, v, stride), BsStream(0, nullptr, 0, 0), {2, t, pc, stride, 0, 0, 0, nullptr, D3DFMT_UNKNOWN, v})
}
static HRESULT STDMETHODCALLTYPE Hk_DrawIndexedPrimitiveUP(IDirect3DDevice9* self, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT pc, const void* id,
                                                           D3DFORMAT f, const void* v, UINT stride) {
    if (InternalCall()) return o_DrawIndexedPrimitiveUP(self, t, mi, nv, pc, id, f, v, stride);
    st.draws++;
    if (Capturing()) { Internal in; CapDraw("DIPUP", t, mi, nv, stride, 0, pc); }
    STEREO_DRAW(o_DrawIndexedPrimitiveUP(self, t, mi, nv, pc, id, f, v, stride), (BsStream(0, nullptr, 0, 0), BsIndices(nullptr)), {3, t, mi, nv, pc, stride, 0, id, f, v})
}
#undef STEREO_DRAW

static void InstallDeviceHooks(IDirect3DDevice9* dev) {
    static bool done = false;
    if (done) return;
    done = true;
    int ok = 0, fail = 0;
#define HOOK(name)                                                                                   \
    if ((o_##name = (name##_t)HookVtable(dev, VT_DEV_##name, (void*)&Hk_##name)) != nullptr) ok++; \
    else { fail++; LOGE("device hook " #name " failed"); }
    HOOK(Reset) HOOK(Present) HOOK(CreateTexture) HOOK(CreateVolumeTexture) HOOK(CreateCubeTexture) HOOK(CreateVertexBuffer)
    HOOK(CreateIndexBuffer) HOOK(SetRenderTarget) HOOK(SetDepthStencilSurface) HOOK(Clear) HOOK(SetViewport) HOOK(SetTexture)
    HOOK(DrawPrimitive) HOOK(DrawIndexedPrimitive) HOOK(DrawPrimitiveUP) HOOK(DrawIndexedPrimitiveUP) HOOK(CreateVertexShader)
    HOOK(SetVertexShader) HOOK(SetVertexShaderConstantF) HOOK(CreatePixelShader) HOOK(SetPixelShader) HOOK(SetPixelShaderConstantF)
    HOOK(StretchRect) HOOK(SetRenderState) HOOK(SetScissorRect) HOOK(SetStreamSource) HOOK(SetVertexDeclaration)
    HOOK(CreateRenderTarget) HOOK(CreateDepthStencilSurface) HOOK(UpdateSurface) HOOK(UpdateTexture) HOOK(GetRenderTargetData)
    HOOK(ColorFill) HOOK(CreateQuery) HOOK(SetSamplerState) HOOK(SetTextureStageState) HOOK(SetIndices) HOOK(SetFVF)
    HOOK(SetStreamSourceFreq) HOOK(SetVertexShaderConstantI) HOOK(SetVertexShaderConstantB) HOOK(SetPixelShaderConstantI)
    HOOK(SetPixelShaderConstantB) HOOK(SetClipPlane) HOOK(BeginScene) HOOK(EndScene) HOOK(CreateAdditionalSwapChain)
#undef HOOK
    if (g_isEx) {
        o_ResetEx = (ResetEx_t)HookVtable(dev, VT_DEVEX_ResetEx, (void*)&Hk_ResetEx);
        if (o_ResetEx) ok++; else { fail++; LOGE("device ResetEx hook failed"); }
        o_DevQI = (DevQI_t)HookVtable(dev, 0, (void*)&Hk_DevQI);
        if (o_DevQI) ok++; else { fail++; LOGE("device QueryInterface hook failed"); }
    }
    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
        o_SCPresent = (SCPresent_t)HookVtable(sc, VT_SC_Present, (void*)&Hk_SCPresent);
        if (o_SCPresent) ok++; else { fail++; LOGE("swap chain Present hook failed"); }
        sc->Release();
    }
    LOGI("device hooks: %d installed, %d failed", ok, fail);
}

static HRESULT STDMETHODCALLTYPE Hk_CreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                                 D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    if (InternalCall() || !pp || !out) return o_CreateDevice(self, adapter, type, focus, flags, pp, out);
    LOGI("CreateDevice(adapter %u, type %d, focus %p, flags %lx)", adapter, (int)type, focus, flags);
    LogPP("  requested", pp);
    HWND wnd = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
    g_devWnd = wnd;
    G().gameWnd = focus ? focus : wnd;
    if (!pp->Windowed && G().s.forceWindowed) MakeWindowed(pp, wnd);
    VrStart();   // launch the XR host early: SteamVR may take a while to come up
    HRESULT hr;
    IDirect3D9Ex* ex = nullptr;
    if (g_isEx && SUCCEEDED(self->QueryInterface(IID_IDirect3D9Ex, (void**)&ex)) && ex) {
        D3DDISPLAYMODEEX m = FullscreenMode(pp);
        IDirect3DDevice9Ex* dex = nullptr;
        {
            Internal in;
            hr = ex->CreateDeviceEx(adapter, type, focus, flags, pp, pp->Windowed ? nullptr : &m, &dex);
        }
        ex->Release();
        if (SUCCEEDED(hr)) { *out = dex; g_devEx = dex; }
        LOGI("CreateDeviceEx -> %08lX, device %p", hr, dex);
    } else {
        hr = o_CreateDevice(self, adapter, type, focus, flags, pp, out);
        LOGI("CreateDevice (plain D3D9) -> %08lX, device %p", hr, *out);
    }
    if (SUCCEEDED(hr)) {
        LogPP("  created", pp);
        D3DADAPTER_IDENTIFIER9 id{};
        if (SUCCEEDED(self->GetAdapterIdentifier(adapter, 0, &id))) LOGI("  adapter: %s (%s)", id.Description, id.Driver);
        // the game's real device (not the 1x1 probe): decide the 3D resolution before it creates its targets
        if (pp->BackBufferWidth > 16) RtScaleInit(pp->BackBufferWidth, pp->BackBufferHeight);
        InstallDeviceHooks(*out);
    } else LOGE("device creation failed %08lX", hr);
    return hr;
}

// Games that create the device with CreateDeviceEx themselves (Lightning Returns): the same setup as Hk_CreateDevice.
static HRESULT STDMETHODCALLTYPE Hk_CreateDeviceEx(IDirect3D9Ex* self, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                                   D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode, IDirect3DDevice9Ex** out) {
    if (InternalCall() || !pp || !out) return o_CreateDeviceEx(self, adapter, type, focus, flags, pp, mode, out);
    LOGI("CreateDeviceEx by the game (adapter %u, type %d, focus %p, flags %lx)", adapter, (int)type, focus, flags);
    LogPP("  requested", pp);
    HWND wnd = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
    g_devWnd = wnd;
    G().gameWnd = focus ? focus : wnd;
    if (!pp->Windowed && G().s.forceWindowed) { MakeWindowed(pp, wnd); mode = nullptr; }   // windowed: no display mode
    VrStart();
    g_isEx = true;
    HRESULT hr;
    { Internal in; hr = o_CreateDeviceEx(self, adapter, type, focus, flags, pp, mode, out); }
    LOGI("CreateDeviceEx -> %08lX, device %p", hr, SUCCEEDED(hr) ? *out : nullptr);
    if (SUCCEEDED(hr) && *out) {
        g_devEx = *out;
        LogPP("  created", pp);
        D3DADAPTER_IDENTIFIER9 id{};
        if (SUCCEEDED(self->GetAdapterIdentifier(adapter, 0, &id))) LOGI("  adapter: %s (%s)", id.Description, id.Driver);
        if (pp->BackBufferWidth > 16) RtScaleInit(pp->BackBufferWidth, pp->BackBufferHeight);
        else {
            // Lightning Returns: a 1x1 device, Reset to the real size later - but it creates its render targets before
            // that reset. The game's resolution is already its window's client size: scale from that.
            RECT rc{};
            HWND w = wnd ? wnd : G().gameWnd;
            if (w && GetClientRect(w, &rc) && rc.right > 16 && rc.bottom > 16) {
                LOGI("tiny back buffer: using the window's client size %ldx%ld as the game resolution", rc.right, rc.bottom);
                RtScaleInit(rc.right, rc.bottom);
            } else {
                LOGI("tiny back buffer: window %p client size %ldx%ld - the game resolution is taken from its first full-size render target", w, rc.right, rc.bottom);
                g_sizeFromFirstTarget = true;
            }
        }
        InstallDeviceHooks(*out);
    } else LOGE("device creation failed %08lX", hr);
    return hr;
}

// ------------------------------------------------------------------ entry points from dllmain
void OnGameStartingGraphics() {
    static bool once = false;
    if (once) return;
    once = true;
    InstallInputHooks();
    InstallPacingHooks();
}

void OnDirect3DCreated(IDirect3D9* d3d, bool isEx) {
    g_isEx = g_isEx || isEx;
    if (!o_CreateDevice) {
        o_CreateDevice = (CreateDevice_t)HookVtable(d3d, VT_D3D9_CreateDevice, (void*)&Hk_CreateDevice);
        LOGI("IDirect3D9::CreateDevice hook %s (%s)", o_CreateDevice ? "installed" : "FAILED", isEx ? "Ex object" : "plain object");
    }
    if (isEx && !o_CreateDeviceEx) {
        o_CreateDeviceEx = (CreateDeviceEx_t)HookVtable(d3d, VT_D3D9EX_CreateDeviceEx, (void*)&Hk_CreateDeviceEx);
        LOGI("IDirect3D9Ex::CreateDeviceEx hook %s", o_CreateDeviceEx ? "installed" : "FAILED");
    }
    if (isEx && !o_D3DQI) {
        o_D3DQI = (D3DQI_t)HookVtable(d3d, 0, (void*)&Hk_D3DQI);
        if (!o_D3DQI) LOGE("IDirect3D9 QueryInterface hook failed");
    }
}

} // namespace fvr
