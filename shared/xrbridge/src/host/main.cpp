// XrBridgeHost.exe - shared 64-bit OpenXR compositor bridge for VR mods.
//
// A game-side mod DLL (usually in a 32-bit game) renders both eyes + HUD into shared textures and talks to this
// process through shared memory (include/xrbridge/ipc.h). This process owns the OpenXR session, publishes predicted
// eye poses, and submits the game's frames to the runtime as a projection layer plus a HUD/menu quad layer.
//
// Command line:  XrBridgeHost.exe --name <instance> [--pid <game pid>] [--log <log file path>]
// Normally started by the mod (xrb::ClientInit), never by the user.
//
// Derived from AmalurVR_XrHost (Amalur VR mod). Additions in v4: per-instance names, D3D9Ex producers
// (slot-state sync, no keyed mutex), BGRA/RGBA format handling with a shader-blit fallback, separate HUD size.

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include "xrbridge/ipc.h"
#include "xrbridge/names.h"
#include "../common/log.h"
#include "../common/bmp.h"

using namespace xrb;

#define XRC(x) do { XrResult _r = (x); if (XR_FAILED(_r)) { LOGE("%s failed: %d (line %d)", #x, (int)_r, __LINE__); return false; } } while (0)

static IpcShared* g_shm = nullptr;
static HANDLE g_game = nullptr;
static HANDLE g_evtHostFrame = nullptr;
static std::string g_name;
static std::wstring g_dumpDir;            // folder of the host log
static uint32_t g_lastDump = 0;
static uint32_t g_dumpNow = 0;            // != 0 while capturing the current fetch

// ------------------------------------------------------------------ pose math
struct Quat { float x, y, z, w; };
struct Vec3 { float x, y, z; };
static Quat QMul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
static Quat QConj(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
static Vec3 QRot(Quat q, Vec3 v) {
    Quat p{v.x, v.y, v.z, 0};
    Quat r = QMul(QMul(q, p), QConj(q));
    return {r.x, r.y, r.z};
}
static XrPosef PoseMul(const XrPosef& a, const XrPosef& b) {   // a * b : b expressed in a's frame
    Quat qa{a.orientation.x, a.orientation.y, a.orientation.z, a.orientation.w};
    Quat qb{b.orientation.x, b.orientation.y, b.orientation.z, b.orientation.w};
    Vec3 pb = QRot(qa, {b.position.x, b.position.y, b.position.z});
    Quat q = QMul(qa, qb);
    XrPosef r;
    r.orientation = {q.x, q.y, q.z, q.w};
    r.position = {a.position.x + pb.x, a.position.y + pb.y, a.position.z + pb.z};
    return r;
}
static XrPosef PoseInv(const XrPosef& a) {
    Quat qi = QConj({a.orientation.x, a.orientation.y, a.orientation.z, a.orientation.w});
    Vec3 p = QRot(qi, {-a.position.x, -a.position.y, -a.position.z});
    XrPosef r;
    r.orientation = {qi.x, qi.y, qi.z, qi.w};
    r.position = {p.x, p.y, p.z};
    return r;
}
static XrPosef Identity() { XrPosef p{}; p.orientation.w = 1; return p; }

// ------------------------------------------------------------------ formats
enum Family { FAM_NONE, FAM_RGBA, FAM_BGRA };
static Family FormatFamily(int64_t f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return FAM_RGBA;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return FAM_BGRA;
    default: return FAM_NONE;
    }
}
static DXGI_FORMAT Unorm(Family f) { return f == FAM_BGRA ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM; }
static DXGI_FORMAT Srgb(Family f) { return f == FAM_BGRA ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; }
static bool IsSrgb(int64_t f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; }

// ------------------------------------------------------------------ state
struct Swap {
    XrSwapchain sc = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<ID3D11RenderTargetView*> rtv;   // blit mode only, created lazily
    bool rtvSrgb = false;
    uint32_t w = 0, h = 0;
};

static XrInstance g_inst = XR_NULL_HANDLE;
static XrSystemId g_sys = XR_NULL_SYSTEM_ID;
static XrSession g_sess = XR_NULL_HANDLE;
static XrSpace g_local = XR_NULL_HANDLE, g_view = XR_NULL_HANDLE;
static XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
static bool g_running = false;
static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static ID3D11Query* g_copyDone = nullptr;
static XrViewConfigurationView g_vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
static std::vector<int64_t> g_runtimeFormats;
static Swap g_eye[2], g_hud;
static int64_t g_swapFormat = 0;
static bool g_blit = false;

struct SharedTex { ID3D11Texture2D* tex = nullptr; IDXGIKeyedMutex* mutex = nullptr; ID3D11ShaderResourceView* srv = nullptr; };
static SharedTex g_tex[kSlots];
static uint32_t g_texGen = 0, g_syncMode = SYNC_KEYED_MUTEX;
static IpcRect g_eyeRect[2], g_hudRect;

// shader blit (only when the runtime offers no swapchain format in the game's format family)
static ID3D11VertexShader* g_vs = nullptr;
static ID3D11PixelShader* g_ps = nullptr;
static ID3D11Buffer* g_cb = nullptr;
static ID3D11RasterizerState* g_rs = nullptr;

static XrPosef g_base = Identity();       // recenter reference (in LOCAL space)
static uint32_t g_lastRecenter = 0;
static bool g_needRecenter = true;

static bool g_haveFrame = false;          // swapchains hold a valid image
static bool g_forceResync = false;        // a swapchain went bad: recreate everything on the next frame
static int g_releasePending = -1;         // slot-state mode: slot still READING until our copy fence completes
static XrTime g_spaceChangeTime = 0;      // pending LOCAL space change (recenter once it takes effect)
static bool g_spaceChangePending = false;
static IpcPose g_shownPose[2];
static IpcFov g_shownFov[2];
static uint32_t g_shownFlags = 0;
static IpcMotion g_shownMotion{};
static bool g_shownNew = false;           // the shown frame was fetched this headset frame (first display)
static uint32_t g_lastSubmitSeq = 0;

// ------------------------------------------------------------------ camera-aware reprojection (SLOT_REPROJECT)
// A game running below the headset rate shows each frame for one or more headset frames; while its camera pans the
// image stands still and then jumps (60 fps on 90 Hz: 2,1,2,1). Here each eye layer is rotated by the camera's
// rotation since the frame's content time, so the scene keeps moving at every headset frame. Only rotation can be
// corrected; near objects that move with the camera (the character it orbits) wobble slightly instead.
// The game's content times are on its own clock: the offset to XrTime is learned as the minimum of (display time -
// content time) over recent frames at their first display, i.e. the shortest path from game frame to the display.
static int64_t g_rpOffsets[90];
static int g_rpCount = 0, g_rpNext = 0;
static struct { uint32_t frames; double dtMs, dtMaxMs, speedDeg; } g_rpStats{};
static const double kRpMaxDt = 0.05;      // s: never extrapolate further (a stalled game keeps its last image)

static XrQuaternionf QuatFromRotVec(const float v[3], double dt) {
    double x = v[0] * dt, y = v[1] * dt, z = v[2] * dt;
    double a = sqrt(x * x + y * y + z * z);
    if (a < 1e-9) return {0, 0, 0, 1};
    double s = sin(a / 2) / a;
    return {(float)(x * s), (float)(y * s), (float)(z * s), (float)cos(a / 2)};
}

// Rotation for eye e at display time `display`, or false (no reprojection).
static bool ReprojectRotation(XrTime display, XrQuaternionf q[2]) {
    if (!(g_shownFlags & SLOT_REPROJECT) || (g_shownFlags & SLOT_MENU_MODE)) return false;
    const IpcMotion& m = g_shownMotion;
    int64_t newest = m.contentTime[0] > m.contentTime[1] ? m.contentTime[0] : m.contentTime[1];
    if (!newest) return false;
    if (g_shownNew) {
        g_rpOffsets[g_rpNext] = display - newest;
        g_rpNext = (g_rpNext + 1) % 90;
        if (g_rpCount < 90) g_rpCount++;
    }
    if (!g_rpCount) return false;
    int64_t off = g_rpOffsets[0];
    for (int i = 1; i < g_rpCount; ++i) if (g_rpOffsets[i] < off) off = g_rpOffsets[i];
    for (int e = 0; e < 2; ++e) {
        double dt = (double)(display - off - (m.contentTime[e] ? m.contentTime[e] : newest)) / 1e9;
        if (dt < 0) dt = 0;
        if (dt > kRpMaxDt) dt = kRpMaxDt;
        q[e] = QuatFromRotVec(m.angVel, dt);
        if (e == 0) {
            g_rpStats.frames++;
            g_rpStats.dtMs += dt * 1000;
            if (dt * 1000 > g_rpStats.dtMaxMs) g_rpStats.dtMaxMs = dt * 1000;
            g_rpStats.speedDeg += sqrt(m.angVel[0] * m.angVel[0] + m.angVel[1] * m.angVel[1] + m.angVel[2] * m.angVel[2]) * 57.2958;
        }
    }
    return true;
}
static struct {
    uint32_t frames, shouldRender, fetched, busy, layersProj, layersQuad; DWORD t0;
    double waitMs, fetchMs, fetchMaxMs, endMs, intervalMaxMs;   // frame timing (QPC)
    uint32_t late;                                              // frames that came > 1.5 display periods after the previous one
} g_stats{};
static LARGE_INTEGER g_qpf{}, g_lastWaitDone{};
static double QpcMs(LONGLONG a, LONGLONG b) { return (double)(b - a) * 1000.0 / (double)g_qpf.QuadPart; }
static LONGLONG QpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

// ------------------------------------------------------------------ D3D11 device on the runtime's adapter
static bool CreateDevice(LUID luid) {
    IDXGIFactory1* fac = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac))) return false;
    IDXGIAdapter1* chosen = nullptr;
    IDXGIAdapter1* ad = nullptr;
    for (UINT i = 0; fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d; ad->GetDesc1(&d);
        if (d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart) { chosen = ad; LOGI("Using adapter: %S", d.Description); break; }
        ad->Release();
    }
    fac->Release();
    D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(chosen, chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 2, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
    if (chosen) chosen->Release();
    if (FAILED(hr)) { LOGE("D3D11CreateDevice failed %08X", hr); return false; }
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    g_dev->CreateQuery(&qd, &g_copyDone);
    return true;
}

// Slot-state mode: the game may overwrite a slot as soon as we mark it FREE and there is no keyed mutex to order
// the two devices, so a slot stays READING until our copies out of it have executed on the GPU.
// Waits up to `ms`; returns true when the fence passed. On timeout the slot stays READING (see TryReleasePending).
static bool CopiesDone(unsigned ms) {
    if (!g_copyDone) { g_ctx->Flush(); return true; }
    DWORD t0 = GetTickCount();
    for (;;) {
        HRESULT r = g_ctx->GetData(g_copyDone, nullptr, 0, 0);
        if (r != S_FALSE) return true;   // S_OK, or device removed (nothing will ever complete then)
        if (GetTickCount() - t0 >= ms) return false;
        YieldProcessor();
    }
}

static void ReleaseSlot(int slot) {
    MemoryBarrier();
    InterlockedCompareExchange((volatile LONG*)&g_shm->slotState[slot], SLOT_FREE, SLOT_READING);
}

// Finish a release that timed out on an earlier frame. Returns false while it is still pending.
static bool TryReleasePending() {
    if (g_releasePending < 0) return true;
    if (!CopiesDone(0)) return false;
    ReleaseSlot(g_releasePending);
    g_releasePending = -1;
    return true;
}

// ------------------------------------------------------------------ shader blit
static const char kBlitHlsl[] =
    "cbuffer C : register(b0) { int2 offset; int linearize; int pad; };\n"
    "Texture2D<float4> src : register(t0);\n"
    "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 uv = float2((id << 1) & 2, id & 2);\n"
    "  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n"
    "float3 toLinear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }\n"
    "float4 ps(float4 pos : SV_Position) : SV_Target {\n"
    "  float4 c = src.Load(int3(int2(pos.xy) + offset, 0));\n"
    "  if (linearize) c.rgb = toLinear(c.rgb);\n"
    "  return c;\n"
    "}\n";

static bool InitBlit() {
    if (g_vs) return true;
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dc ? (pD3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!compile) { LOGE("d3dcompiler_47.dll not available - cannot convert texture formats"); return false; }
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(compile(kBlitHlsl, sizeof(kBlitHlsl) - 1, "blit", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(compile(kBlitHlsl, sizeof(kBlitHlsl) - 1, "blit", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err))) {
        LOGE("blit shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        if (err) err->Release();
        if (vsb) vsb->Release();
        if (psb) psb->Release();
        return false;
    }
    if (err) err->Release();
    g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
    vsb->Release(); psb->Release();
    D3D11_BUFFER_DESC bd{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    g_dev->CreateBuffer(&bd, nullptr, &g_cb);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    g_dev->CreateRasterizerState(&rd, &g_rs);
    LOGI("shader blit ready");
    return g_vs && g_ps && g_cb;
}

// ------------------------------------------------------------------ OpenXR setup
static bool InitXr() {
    std::vector<const char*> exts = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    char app[64] = {};
    memcpy(app, g_shm->appName, sizeof(app) - 1);
    strcpy_s(ci.applicationInfo.applicationName, app[0] ? app : g_name.c_str());
    ci.applicationInfo.applicationVersion = 1;
    strcpy_s(ci.applicationInfo.engineName, "XrBridge");
    ci.applicationInfo.engineVersion = kIpcVersion;
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.enabledExtensionNames = exts.data();
    XRC(xrCreateInstance(&ci, &g_inst));
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_inst, &ip);
    LOGI("OpenXR runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
    strncpy_s(g_shm->runtimeName, ip.runtimeName, _TRUNCATE);

    XrSystemGetInfo sg{XR_TYPE_SYSTEM_GET_INFO};
    sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    for (int tries = 0;; ++tries) {
        XrResult r = xrGetSystem(g_inst, &sg, &g_sys);
        if (XR_SUCCEEDED(r)) break;
        if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE && tries < 60) { if (tries == 0) LOGW("Headset not available yet, waiting..."); Sleep(1000); continue; }
        LOGE("xrGetSystem failed: %d", (int)r);
        return false;
    }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(g_inst, g_sys, &sp);
    LOGI("System: %s (max swapchain %ux%u)", sp.systemName, sp.graphicsProperties.maxSwapchainImageWidth, sp.graphicsProperties.maxSwapchainImageHeight);
    strncpy_s(g_shm->systemName, sp.systemName, _TRUNCATE);

    uint32_t n = 0;
    XRC(xrEnumerateViewConfigurationViews(g_inst, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, g_vcv));
    LOGI("Recommended per-eye %ux%u (max %ux%u)", g_vcv[0].recommendedImageRectWidth, g_vcv[0].recommendedImageRectHeight,
         g_vcv[0].maxImageRectWidth, g_vcv[0].maxImageRectHeight);

    PFN_xrGetD3D11GraphicsRequirementsKHR getReq = nullptr;
    XRC(xrGetInstanceProcAddr(g_inst, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReq));
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XRC(getReq(g_inst, g_sys, &req));
    g_shm->adapterLuid = ((uint64_t)(uint32_t)req.adapterLuid.HighPart << 32) | req.adapterLuid.LowPart;
    if (!CreateDevice(req.adapterLuid)) return false;

    XrGraphicsBindingD3D11KHR gb{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    gb.device = g_dev;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = g_sys;
    XRC(xrCreateSession(g_inst, &sci, &g_sess));

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace = Identity();
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XRC(xrCreateReferenceSpace(g_sess, &rs, &g_local));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XRC(xrCreateReferenceSpace(g_sess, &rs, &g_view));

    uint32_t fc = 0;
    xrEnumerateSwapchainFormats(g_sess, 0, &fc, nullptr);
    g_runtimeFormats.resize(fc);
    xrEnumerateSwapchainFormats(g_sess, fc, &fc, g_runtimeFormats.data());
    std::string list;
    for (auto f : g_runtimeFormats) list += std::to_string(f) + " ";
    LOGI("Runtime swapchain formats: %s", list.c_str());

    g_shm->recWidth = g_vcv[0].recommendedImageRectWidth;
    g_shm->recHeight = g_vcv[0].recommendedImageRectHeight;
    MemoryBarrier();
    g_shm->hostState = HOST_READY;
    return true;
}

static bool RuntimeHasFormat(int64_t f) {
    for (auto x : g_runtimeFormats) if (x == f) return true;
    return false;
}

// Pick the swapchain format for the game's texture format. Same family -> plain GPU copy; otherwise shader blit.
static bool ChooseSwapFormat(uint32_t texFormat) {
    Family fam = FormatFamily(texFormat);
    if (fam == FAM_NONE) { LOGE("Unsupported shared texture format %u (need an 8-bit RGBA or BGRA format)", texFormat); return false; }
    Family other = fam == FAM_RGBA ? FAM_BGRA : FAM_RGBA;
    const int64_t prefs[4] = {Srgb(fam), Unorm(fam), Srgb(other), Unorm(other)};
    g_swapFormat = 0;
    for (int64_t p : prefs) if (RuntimeHasFormat(p)) { g_swapFormat = p; break; }
    if (!g_swapFormat) { LOGE("Runtime offers no 8-bit RGBA/BGRA swapchain format"); return false; }
    // Game pixels are gamma-encoded. An sRGB swapchain takes them as-is; a UNORM one is treated as linear by the
    // runtime, so we must decode in the blit shader (CopySubresourceRegion can't convert).
    g_blit = FormatFamily(g_swapFormat) != fam || !IsSrgb(g_swapFormat);
    if (g_blit && !InitBlit()) return false;
    g_shm->swapFormat = (uint32_t)g_swapFormat;
    g_shm->hostCopyMode = g_blit ? 1 : 0;
    LOGI("Game texture format %u -> swapchain format %lld (%s)", texFormat, (long long)g_swapFormat, g_blit ? "shader blit" : "direct copy");
    return true;
}

static bool CreateSwap(Swap& s, uint32_t w, uint32_t h) {
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = g_swapFormat;
    ci.sampleCount = 1; ci.width = w; ci.height = h; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    XRC(xrCreateSwapchain(g_sess, &ci, &s.sc));
    uint32_t n = 0;
    xrEnumerateSwapchainImages(s.sc, 0, &n, nullptr);
    s.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    XRC(xrEnumerateSwapchainImages(s.sc, n, &n, (XrSwapchainImageBaseHeader*)s.images.data()));
    s.rtv.assign(n, nullptr);
    s.w = w; s.h = h;
    return true;
}

static void DestroySwap(Swap& s) {
    for (auto* r : s.rtv) if (r) r->Release();
    if (s.sc) xrDestroySwapchain(s.sc);
    s = Swap();
}

static void ReleaseShared() {
    for (auto& t : g_tex) {
        if (t.srv) t.srv->Release();
        if (t.mutex) t.mutex->Release();
        if (t.tex) t.tex->Release();
        t = SharedTex();
    }
}

static bool RectOk(const IpcRect& r, uint32_t aw, uint32_t ah) {
    return r.w > 0 && r.h > 0 && r.x >= 0 && r.y >= 0 && (uint32_t)(r.x + r.w) <= aw && (uint32_t)(r.y + r.h) <= ah;
}

// (Re)open the game's shared atlases when the game publishes a new generation.
static bool SyncSharedTextures() {
    uint32_t gen = g_shm->gameState;
    if (gen == g_texGen && !g_forceResync) return g_tex[0].tex != nullptr;
    if (g_releasePending >= 0) { CopiesDone(100); ReleaseSlot(g_releasePending); g_releasePending = -1; }
    g_forceResync = false;
    ReleaseShared();
    DestroySwap(g_eye[0]); DestroySwap(g_eye[1]); DestroySwap(g_hud);
    g_haveFrame = false;
    g_texGen = gen;
    if (!gen) return false;
    MemoryBarrier();
    uint32_t aw = g_shm->atlasWidth, ah = g_shm->atlasHeight;
    if (!aw || !ah) { LOGI("Game withdrew its textures (gen %u)", gen); return false; }
    g_syncMode = g_shm->syncMode;
    g_eyeRect[0] = g_shm->eyeRect[0]; g_eyeRect[1] = g_shm->eyeRect[1]; g_hudRect = g_shm->hudRect;
    if (!RectOk(g_eyeRect[0], aw, ah) || !RectOk(g_eyeRect[1], aw, ah) || !RectOk(g_hudRect, aw, ah) ||
        g_eyeRect[0].w != g_eyeRect[1].w || g_eyeRect[0].h != g_eyeRect[1].h) {
        LOGE("Invalid atlas layout from game (atlas %ux%u)", aw, ah);
        return false;
    }
    if (!ChooseSwapFormat(g_shm->texFormat)) return false;
    for (int i = 0; i < kSlots; ++i) {
        // 32-bit handle values are sign-extended when moved to a 64-bit process (Windows interop rule)
        HANDLE h = (HANDLE)(LONG_PTR)(LONG)(uint32_t)g_shm->sharedHandle[i];
        if (FAILED(g_dev->OpenSharedResource(h, __uuidof(ID3D11Texture2D), (void**)&g_tex[i].tex))) {
            LOGE("OpenSharedResource(slot %d, handle %p) failed - is the game on the same GPU as the headset?", i, h);
            ReleaseShared();
            return false;
        }
        if (g_syncMode == SYNC_KEYED_MUTEX && FAILED(g_tex[i].tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&g_tex[i].mutex))) {
            LOGE("Slot %d has no keyed mutex but the game asked for SYNC_KEYED_MUTEX", i);
            ReleaseShared();
            return false;
        }
        if (g_blit) {
            D3D11_TEXTURE2D_DESC td; g_tex[i].tex->GetDesc(&td);
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = Unorm(FormatFamily(td.Format));
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            if (FAILED(g_dev->CreateShaderResourceView(g_tex[i].tex, &sv, &g_tex[i].srv))) {
                LOGE("Cannot create SRV on shared slot %d (format %u, bind %u)", i, td.Format, td.BindFlags);
                ReleaseShared();
                return false;
            }
        }
    }
    bool ok = CreateSwap(g_eye[0], g_eyeRect[0].w, g_eyeRect[0].h) && CreateSwap(g_eye[1], g_eyeRect[1].w, g_eyeRect[1].h) &&
              CreateSwap(g_hud, g_hudRect.w, g_hudRect.h);
    LOGI("Opened game atlases %ux%u (gen %u, %s): eyes %dx%d, HUD %dx%d, swapchains %s", aw, ah, gen,
         g_syncMode == SYNC_KEYED_MUTEX ? "keyed mutex" : "slot state", g_eyeRect[0].w, g_eyeRect[0].h, g_hudRect.w, g_hudRect.h,
         ok ? "ok" : "FAILED");
    if (!ok) { ReleaseShared(); DestroySwap(g_eye[0]); DestroySwap(g_eye[1]); DestroySwap(g_hud); }
    return ok;
}

// ------------------------------------------------------------------ frame
static bool Recenter(XrTime t) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(g_view, g_local, t, &loc)) || !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) return false;
    // keep only yaw (rotation about +Y) and position
    Quat q{loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z, loc.pose.orientation.w};
    Vec3 fwd = QRot(q, {0, 0, -1});
    float yaw = atan2f(-fwd.x, -fwd.z);
    g_base.orientation = {0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)};
    g_base.position = loc.pose.position;
    LOGI("Recentered: yaw %.1f deg, pos (%.2f %.2f %.2f)", yaw * 57.2958f, g_base.position.x, g_base.position.y, g_base.position.z);
    return true;
}

static void PublishPoses(const XrView views[2], XrTime t, XrDuration period) {
    XrPosef inv = PoseInv(g_base);
    g_shm->poseSeq = g_shm->poseSeq + 1;   // odd: writing
    MemoryBarrier();
    for (int e = 0; e < 2; ++e) {
        XrPosef p = PoseMul(inv, views[e].pose);
        g_shm->eyePose[e] = {p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
        g_shm->eyeFov[e] = {views[e].fov.angleLeft, views[e].fov.angleRight, views[e].fov.angleUp, views[e].fov.angleDown};
    }
    g_shm->predictedTime = t;
    g_shm->displayPeriodNs = period;
    MemoryBarrier();
    g_shm->poseSeq = g_shm->poseSeq + 1;
}

static ID3D11RenderTargetView* GetRtv(Swap& s, uint32_t idx) {
    if (s.rtv[idx]) return s.rtv[idx];
    // Prefer a UNORM view so the game's (already gamma-encoded) bytes pass through unchanged. This needs a typeless
    // swapchain texture; if the runtime's texture is typed sRGB we use an sRGB view and linearise in the shader.
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rd.Format = Unorm(FormatFamily(g_swapFormat));
    s.rtvSrgb = false;
    if (FAILED(g_dev->CreateRenderTargetView(s.images[idx].texture, &rd, &s.rtv[idx]))) {
        rd.Format = (DXGI_FORMAT)g_swapFormat;
        s.rtvSrgb = IsSrgb(g_swapFormat);
        if (FAILED(g_dev->CreateRenderTargetView(s.images[idx].texture, &rd, &s.rtv[idx]))) { LOGE("swapchain RTV failed"); return nullptr; }
    }
    return s.rtv[idx];
}

static void BlitRegion(Swap& s, uint32_t idx, ID3D11ShaderResourceView* srv, const IpcRect& r) {
    ID3D11RenderTargetView* rtv = GetRtv(s, idx);
    if (!rtv) return;
    // decode gamma if the hardware re-encodes (sRGB view) or the runtime treats the swapchain as linear (UNORM)
    int cb[4] = {r.x, r.y, (s.rtvSrgb || !IsSrgb(g_swapFormat)) ? 1 : 0, 0};
    g_ctx->UpdateSubresource(g_cb, 0, nullptr, cb, 0, 0);
    D3D11_VIEWPORT vp{0, 0, (float)r.w, (float)r.h, 0, 1};
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(g_rs);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->PSSetShader(g_ps, nullptr, 0);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
    g_ctx->PSSetShaderResources(0, 1, &srv);
    g_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nul = nullptr;
    g_ctx->PSSetShaderResources(0, 1, &nul);
    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

static void DumpImage(ID3D11Texture2D* img, const wchar_t* what);
static void DumpImage(ID3D11Texture2D* img, const wchar_t* what) {
    D3D11_TEXTURE2D_DESC d; img->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = d.Width; sd.Height = d.Height; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = d.Format;
    sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &st))) { LOGE("dump: staging texture failed"); return; }
    g_ctx->CopyResource(st, img);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%shost_%u_%s.bmp", g_dumpDir.c_str(), g_dumpNow, what);
        bool rgba = FormatFamily(d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM : d.Format) != FAM_BGRA;
        bool ok = WriteBmp32(path, (const uint8_t*)m.pData, d.Width, d.Height, m.RowPitch, rgba);
        LOGI("dump: %S %ux%u format %u %s", path, d.Width, d.Height, d.Format, ok ? "saved" : "FAILED");
        g_ctx->Unmap(st, 0);
    }
    st->Release();
}

static void CopyRegion(Swap& s, const SharedTex& src, const IpcRect& r, const wchar_t* what) {
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(s.sc, &ai, &idx))) return;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 100000000;   // 100 ms; XR_TIMEOUT_EXPIRED is a *success* code, so keep waiting on it
    XrResult wr;
    int tries = 0;
    do { wr = xrWaitSwapchainImage(s.sc, &wi); } while (wr == XR_TIMEOUT_EXPIRED && ++tries < 20);
    if (wr != XR_SUCCESS) {
        // Not waited -> must not write or release it. The swapchain's acquire/wait order is now unknown: rebuild.
        LOGE("xrWaitSwapchainImage failed (%d) - recreating swapchains", (int)wr);
        g_forceResync = true;
        return;
    }
    if (g_blit) {
        BlitRegion(s, idx, src.srv, r);
    } else {
        D3D11_BOX box{(UINT)r.x, (UINT)r.y, 0, (UINT)(r.x + r.w), (UINT)(r.y + r.h), 1};
        g_ctx->CopySubresourceRegion(s.images[idx].texture, 0, 0, 0, 0, src.tex, 0, &box);
    }
    if (g_dumpNow) DumpImage(s.images[idx].texture, what);   // read back exactly what the runtime will show
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(s.sc, &ri);
}

// Pull the newest finished game frame into the swapchains (if there is one).
static void FetchGameFrame() {
    if (!SyncSharedTextures()) return;
    if (!TryReleasePending()) { g_stats.busy++; return; }   // previous copy still in flight on the GPU
    uint32_t seq = g_shm->submitSeq;
    if (seq == g_lastSubmitSeq && g_haveFrame) return;
    MemoryBarrier();
    int slot = g_shm->lastSlot;
    if (slot < 0 || slot >= kSlots || !g_tex[slot].tex) return;

    if (g_syncMode == SYNC_KEYED_MUTEX) {
        if (g_tex[slot].mutex->AcquireSync(1, 4) != S_OK) { g_stats.busy++; return; }   // game still owns it (or it was recycled)
    } else {
        LONG prev = InterlockedCompareExchange((volatile LONG*)&g_shm->slotState[slot], SLOT_READING, SLOT_READY);
        if (prev != SLOT_READY) {
            // FREE: we already consumed it (a newer submit raced with our read of submitSeq) -> nothing new.
            // RENDERING: the game recycled it because a newer slot was submitted -> pick that up next frame.
            if (prev == SLOT_FREE) g_lastSubmitSeq = seq;
            else g_stats.busy++;
            return;
        }
        if (g_shm->gameState != g_texGen) {   // the game re-published between our generation check and the CAS
            InterlockedCompareExchange((volatile LONG*)&g_shm->slotState[slot], SLOT_READY, SLOT_READING);
            return;
        }
    }
    MemoryBarrier();
    g_stats.fetched++;
    IpcPose pose[2]; IpcFov fov[2];
    memcpy(pose, g_shm->slotPose[slot], sizeof(pose));
    memcpy(fov, g_shm->slotFov[slot], sizeof(fov));
    uint32_t flags = g_shm->slotFlags[slot];
    IpcMotion motion = g_shm->slotMotion[slot];
    uint32_t dumpReq = g_shm->dumpRequest;
    if (dumpReq != g_lastDump) { g_lastDump = dumpReq; g_dumpNow = dumpReq; }
    if (!(flags & SLOT_MENU_MODE)) { CopyRegion(g_eye[0], g_tex[slot], g_eyeRect[0], L"left"); CopyRegion(g_eye[1], g_tex[slot], g_eyeRect[1], L"right"); }
    CopyRegion(g_hud, g_tex[slot], g_hudRect, flags & SLOT_MENU_MODE ? L"menu" : L"hud");
    if (g_dumpNow) {
        LOGI("dump %u: flags %u, left eye pose (%.3f %.3f %.3f) fov L%.3f R%.3f U%.3f D%.3f, right eye pose (%.3f %.3f %.3f)", g_dumpNow, flags,
             pose[0].px, pose[0].py, pose[0].pz, fov[0].left, fov[0].right, fov[0].up, fov[0].down, pose[1].px, pose[1].py, pose[1].pz);
        g_dumpNow = 0;
    }

    if (g_syncMode == SYNC_KEYED_MUTEX) {
        g_tex[slot].mutex->ReleaseSync(0);
    } else if (g_copyDone) {
        g_ctx->End(g_copyDone);
        g_ctx->Flush();
        // Never wait here: our copy queues behind the game's GPU work, and a spin of a few ms inside the frame made
        // the host miss 90 Hz deadlines. The slot is released on a later frame once the fence passed (the game
        // has the other slots meanwhile); never hand the game a slot mid-copy.
        if (CopiesDone(0)) ReleaseSlot(slot);
        else g_releasePending = slot;
    } else {
        ReleaseSlot(slot);
    }
    memcpy(g_shownPose, pose, sizeof(pose));
    memcpy(g_shownFov, fov, sizeof(fov));
    g_shownFlags = flags;
    g_shownMotion = motion;
    g_shownNew = true;
    g_lastSubmitSeq = seq;
    g_haveFrame = true;
}

static bool RunFrame() {
    XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    LONGLONG tw0 = QpcNow();
    XRC(xrWaitFrame(g_sess, &fwi, &fs));
    LONGLONG tw1 = QpcNow();
    g_stats.waitMs += QpcMs(tw0, tw1);
    if (g_lastWaitDone.QuadPart) {
        double iv = QpcMs(g_lastWaitDone.QuadPart, tw1);
        if (iv > g_stats.intervalMaxMs) g_stats.intervalMaxMs = iv;
        if (fs.predictedDisplayPeriod > 0 && iv > 1.5 * fs.predictedDisplayPeriod / 1e6) g_stats.late++;
    }
    g_lastWaitDone.QuadPart = tw1;
    XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
    XRC(xrBeginFrame(g_sess, &fbi));

    if (g_spaceChangePending && fs.predictedDisplayTime >= g_spaceChangeTime) { g_spaceChangePending = false; g_needRecenter = true; }
    if (g_shm->recenterRequest != g_lastRecenter) { g_lastRecenter = g_shm->recenterRequest; g_needRecenter = true; }
    if (g_needRecenter && Recenter(fs.predictedDisplayTime)) g_needRecenter = false;   // retried until tracking is valid
    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = fs.predictedDisplayTime;
    vli.space = g_local;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t vc = 0;
    if (XR_SUCCEEDED(xrLocateViews(g_sess, &vli, &vs, 2, &vc, views)) && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
        PublishPoses(views, fs.predictedDisplayTime, fs.predictedDisplayPeriod);
    g_shm->hostHeartbeat = g_shm->hostHeartbeat + 1;
    if (g_evtHostFrame) SetEvent(g_evtHostFrame);   // pace the game to the headset

    LONGLONG tf0 = QpcNow();
    FetchGameFrame();
    double fm = QpcMs(tf0, QpcNow());
    g_stats.fetchMs += fm;
    if (fm > g_stats.fetchMaxMs) g_stats.fetchMaxMs = fm;

    XrQuaternionf rp[2];
    bool reproject = g_haveFrame && ReprojectRotation(fs.predictedDisplayTime, rp);
    g_shownNew = false;

    std::vector<XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    if (fs.shouldRender && g_haveFrame) {
        bool menu = (g_shownFlags & SLOT_MENU_MODE) != 0;
        if (!menu) {
            for (int e = 0; e < 2; ++e) {
                const IpcPose& p = g_shownPose[e];
                XrPosef rel{{p.qx, p.qy, p.qz, p.qw}, {p.px, p.py, p.pz}};
                if (reproject) { XrPosef r{rp[e], {0, 0, 0}}; rel = PoseMul(r, rel); }   // scene rotated with the game camera
                pv[e].pose = PoseMul(g_base, rel);   // back to LOCAL space
                pv[e].fov = {g_shownFov[e].left, g_shownFov[e].right, g_shownFov[e].up, g_shownFov[e].down};
                pv[e].subImage.swapchain = g_eye[e].sc;
                pv[e].subImage.imageRect = {{0, 0}, {(int32_t)g_eye[e].w, (int32_t)g_eye[e].h}};
            }
            proj.space = g_local;
            proj.viewCount = 2;
            proj.views = pv;
            layers.push_back((XrCompositionLayerBaseHeader*)&proj);
        }
        if (g_shownFlags & SLOT_HUD_VISIBLE) {
            float dist = menu ? g_shm->menuDistance : g_shm->hudDistance;
            float width = menu ? g_shm->menuWidth : g_shm->hudWidth;
            quad.layerFlags = 0;
            if (!menu) {
                quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                if (!g_shm->hudPremultiplied) quad.layerFlags |= XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
            }
            quad.subImage.swapchain = g_hud.sc;
            quad.subImage.imageRect = {{0, 0}, {(int32_t)g_hud.w, (int32_t)g_hud.h}};
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.size = {width, width * (float)g_hud.h / (float)g_hud.w};
            if (!menu && g_shm->hudFollowHead) {
                quad.space = g_view;
                quad.pose = Identity();
                quad.pose.position = {0, g_shm->hudOffsetY, -dist};
            } else {
                quad.space = g_local;
                XrPosef off = Identity();
                off.position = {0, menu ? 0.0f : g_shm->hudOffsetY, -dist};
                quad.pose = PoseMul(g_base, off);
            }
            layers.push_back((XrCompositionLayerBaseHeader*)&quad);
        }
    }
    g_stats.frames++;
    if (fs.shouldRender) g_stats.shouldRender++;
    for (auto* l : layers) { if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) g_stats.layersProj++; else g_stats.layersQuad++; }
    DWORD now = GetTickCount();
    if (!g_stats.t0) g_stats.t0 = now;
    if (now - g_stats.t0 >= 10000) {
        float sec = (now - g_stats.t0) / 1000.0f;
        double n = g_stats.frames ? g_stats.frames : 1;
        LOGI("stats: %.1f host fps, shouldRender %u, game frames %.1f/s, slot busy %u, proj layers %u, quad layers %u, state %d",
             g_stats.frames / sec, g_stats.shouldRender, g_stats.fetched / sec, g_stats.busy, g_stats.layersProj, g_stats.layersQuad, (int)g_state);
        LOGI("timing: per frame xrWaitFrame %.2f ms, fetch+copy %.2f ms (max %.2f), xrEndFrame %.2f ms; longest frame interval %.2f ms, "
             "late frames %u (display period %.2f ms)",
             g_stats.waitMs / n, g_stats.fetchMs / n, g_stats.fetchMaxMs, g_stats.endMs / n, g_stats.intervalMaxMs, g_stats.late,
             fs.predictedDisplayPeriod / 1e6);
        if (g_rpStats.frames)
            LOGI("reproject: %u headset frames, extrapolated avg %.2f ms (max %.2f), avg camera turn speed %.1f deg/s", g_rpStats.frames,
                 g_rpStats.dtMs / g_rpStats.frames, g_rpStats.dtMaxMs, g_rpStats.speedDeg / g_rpStats.frames);
        g_rpStats = {};
        g_stats = {}; g_stats.t0 = now;
    }
    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = (uint32_t)layers.size();
    fei.layers = layers.data();
    LONGLONG te0 = QpcNow();
    XRC(xrEndFrame(g_sess, &fei));
    g_stats.endMs += QpcMs(te0, QpcNow());
    TryReleasePending();   // hand the slot back as soon as our copy finished, not only at the next headset frame
    return true;
}

static bool OwnShm();
static bool PollEvents(bool* quit) {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_inst, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            auto* e = (XrEventDataSessionStateChanged*)&ev;
            g_state = e->state;
            LOGI("Session state -> %d", (int)g_state);
            if (!OwnShm()) { *quit = true; break; }
            if (g_state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XRC(xrBeginSession(g_sess, &bi));
                g_running = true;
                g_shm->hostState = HOST_READY;
            } else if (g_state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g_sess);
                g_running = false;
                g_shm->hostState = HOST_READY;
            } else if (g_state == XR_SESSION_STATE_FOCUSED) {
                g_shm->hostState = HOST_FOCUSED;
            } else if (g_state == XR_SESSION_STATE_VISIBLE) {
                g_shm->hostState = HOST_RUNNING;
            } else if (g_state == XR_SESSION_STATE_SYNCHRONIZED) {
                g_shm->hostState = HOST_READY;   // not shown yet (runtime may throttle): game must not pace to us
            } else if (g_state == XR_SESSION_STATE_EXITING || g_state == XR_SESSION_STATE_LOSS_PENDING) {
                *quit = true;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: *quit = true; break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
            auto* e = (XrEventDataReferenceSpaceChangePending*)&ev;
            if (e->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) { g_spaceChangePending = true; g_spaceChangeTime = e->changeTime; }
            break;
        }
        default: break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return true;
}

// A restarted game re-initialises the shared memory and launches a new host; an old host must then stop writing.
static bool OwnShm() { return g_shm && g_shm->hostPid == GetCurrentProcessId() && g_shm->magic == kIpcMagic; }

static void Fail(const char* msg) {
    LOGE("%s", msg);
    if (g_shm) {
        strncpy_s(g_shm->hostError, msg, _TRUNCATE);
        MemoryBarrier();
        g_shm->hostState = HOST_ERROR;
    }
    LogFlush();
}

// ------------------------------------------------------------------ main
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    DWORD pid = 0;
    std::wstring logPath;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--pid") && i + 1 < argc) pid = wcstoul(argv[++i], nullptr, 10);
        else if (!wcscmp(argv[i], L"--log") && i + 1 < argc) logPath = argv[++i];
        else if (!wcscmp(argv[i], L"--name") && i + 1 < argc) { for (const wchar_t* p = argv[++i]; *p; ++p) g_name += (char)*p; }
    }
    if (logPath.empty()) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        logPath = exe;
        logPath = logPath.substr(0, logPath.find_last_of(L"\\/") + 1) + L"XrBridgeHost_";
        for (char c : g_name) logPath += (wchar_t)c;
        logPath += L".log";
    }
    LogInit(logPath.c_str(), LOG_INFO);
    g_dumpDir = logPath.substr(0, logPath.find_last_of(L"\\/") + 1);
    LOGI("XrBridge host v%u " __DATE__ " " __TIME__ " (instance '%s', game pid %lu)", kIpcVersion, g_name.c_str(), pid);
    if (!ValidInstanceName(g_name.c_str())) { LOGE("Missing or invalid --name - this exe is started by the mod, not by hand"); return 1; }
    // The host must hit every display frame even while the game keeps all cores busy (FF XIII spins dozens of
    // threads on Sleep(0)), so it runs at high priority. Its work per frame is tiny.
    BOOL pc = SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    BOOL tp = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    LOGI("Priority: process HIGH %s, frame thread HIGHEST %s", pc ? "ok" : "failed", tp ? "ok" : "failed");

    HANDLE map = nullptr;
    std::wstring shmName = ShmName(g_name.c_str());
    for (int i = 0; i < 100 && !map; ++i) { map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shmName.c_str()); if (!map) Sleep(50); }
    if (!map) { LOGE("Shared memory %S not found - start the game, not this exe", shmName.c_str()); return 1; }
    g_shm = (IpcShared*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(IpcShared));
    if (!g_shm || g_shm->magic != kIpcMagic || g_shm->version != kIpcVersion) {
        LOGE("Shared memory version mismatch (got magic %08X version %u, want %08X v%u) - mod and host are from different builds",
             g_shm ? g_shm->magic : 0, g_shm ? g_shm->version : 0, kIpcMagic, kIpcVersion);
        return 1;
    }
    LogSetLevel((int)g_shm->hostLogLevel);
    g_shm->hostPid = GetCurrentProcessId();
    g_shm->hostState = HOST_STARTING;
    g_evtHostFrame = CreateEventW(nullptr, FALSE, FALSE, HostEventName(g_name.c_str()).c_str());
    g_game = OpenProcess(SYNCHRONIZE, FALSE, pid ? pid : g_shm->gamePid);

    if (!InitXr()) {
        Fail("OpenXR initialisation failed - is SteamVR (or another OpenXR runtime) installed and set as active? See the host log.");
        return 2;
    }

    bool quit = false;
    while (!quit) {
        if (g_game && WaitForSingleObject(g_game, 0) == WAIT_OBJECT_0) { LOGI("Game exited"); break; }
        if (g_shm->quitRequest) { LOGI("Quit requested by game"); break; }
        if (!OwnShm()) { LOGW("Shared memory taken over by a newer game/host - exiting"); break; }
        PollEvents(&quit);
        if (g_running) {
            if (!RunFrame()) { Sleep(10); }
        } else {
            Sleep(20);
        }
    }
    LOGI("Shutting down");
    if (g_running) {   // clean exit: request, then wait for STOPPING (PollEvents ends the session there)
        xrRequestExitSession(g_sess);
        bool q = false;
        for (int i = 0; i < 100 && g_running; ++i) { PollEvents(&q); if (g_running) Sleep(10); }
        if (g_running) xrEndSession(g_sess);
    }
    if (g_releasePending >= 0 && OwnShm()) { CopiesDone(100); ReleaseSlot(g_releasePending); }
    DestroySwap(g_eye[0]); DestroySwap(g_eye[1]); DestroySwap(g_hud);
    ReleaseShared();
    if (g_sess) xrDestroySession(g_sess);
    if (g_inst) xrDestroyInstance(g_inst);
    if (OwnShm()) g_shm->hostState = HOST_NONE;
    LogFlush();
    return 0;
}
