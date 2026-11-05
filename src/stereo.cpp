// Stereo rendering for FF XIII (native twin rendering or alternate-eye frames) - see stereo.h.
//
// Matrix conventions (verified on a capture): constants are rows of row-vector matrices (v' = v * M).
//   WV  = worldViewMatrix,      WVP = worldViewProjMatrix = WV * P,
//   VIT = viewITMatrix = transpose(inverse(V)), so inverse(V) = transpose(VIT).
// The game's view space is right-handed, +Y up, -Z forward (same as OpenXR), 1 unit ~ 1 m.
// With E = the eye transform (view space -> eye space):
//   WV' = WV * E,   WVP' = WV' * P',   VIT' = transpose(inverse(E) * transpose(VIT)).
#include "stereo.h"
#include "d3d9hooks.h"
#include "capture.h"
#include "globals.h"
#include "imageio.h"
#include "xrbridge/client.h"
#include "rtscale.h"
#include <unordered_map>
#include <cmath>
#include <cstring>

namespace fvr {

static bool Native() { return G().s.vrMethod == VR_NATIVE; }

// ------------------------------------------------------------------ 4x4 math (row-vector)
struct M4 { float m[4][4]; };
static M4 Mul(const M4& a, const M4& b) {
    M4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}
static M4 Transpose(const M4& a) {
    M4 r;
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) r.m[i][j] = a.m[j][i];
    return r;
}
static bool Inverse(const M4& a, M4* out) {
    double m[4][8];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { m[i][j] = a.m[i][j]; m[i][j + 4] = i == j; }
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r) if (fabs(m[r][c]) > fabs(m[p][c])) p = r;
        if (fabs(m[p][c]) < 1e-12) return false;
        for (int j = 0; j < 8; ++j) { double t = m[c][j]; m[c][j] = m[p][j]; m[p][j] = t; }
        double d = m[c][c];
        for (int j = 0; j < 8; ++j) m[c][j] /= d;
        for (int r = 0; r < 4; ++r)
            if (r != c) { double f = m[r][c]; for (int j = 0; j < 8; ++j) m[r][j] -= f * m[c][j]; }
    }
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) out->m[i][j] = (float)m[i][j + 4];
    return true;
}
static M4 Identity() { M4 r{}; r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1; return r; }
static M4 Load(int reg) { M4 r; memcpy(r.m, Sh().vsc[reg], sizeof(r.m)); return r; }

// ------------------------------------------------------------------ vertex shader constant layouts
// FF XIII / XIII-2: worldViewProjMatrix + worldViewMatrix + viewITMatrix. Lightning Returns has no worldViewMatrix but
// worldMatrix (w) and a 3-row viewITMatrix: there WV = W * V with V = inverse(transpose(VIT)). Its scene depth pass (the
// R32F target its lighting and fog read) uses localProjectionMatrix (= WVP) + localViewMatrix (= WV) and no
// viewITMatrix (vit < 0); its shadow maps use localProjectionMatrix alone (the light's camera: not patched).
struct Layout { int wvp = -1, wv = -1, vit = -1, w = -1, vitN = 4, so = -1; bool local = false; };   // local: localProjection/ViewMatrix   // so: g_vScreenOffset (post-processing half-pixel offset)
static std::unordered_map<void*, Layout> g_layouts;
static SRWLOCK g_layoutLock = SRWLOCK_INIT;
static Layout g_cur;             // layout of the current vertex shader (wvp < 0: not patchable)

// viewITMatrix of the current draw (a 3-row one is completed: its 4th row is (0, 0, 0, 1) for a rigid camera)
static M4 LoadVIT() {
    M4 r = Load(g_cur.vit);
    if (g_cur.vitN < 4) { r.m[3][0] = r.m[3][1] = r.m[3][2] = 0; r.m[3][3] = 1; }
    return r;
}
// worldViewMatrix of the current draw: the constant, or worldMatrix * view (view from viewITMatrix)
static M4 LoadWV() {
    if (g_cur.wv >= 0) return Load(g_cur.wv);
    M4 V;
    if (!Inverse(Transpose(LoadVIT()), &V)) V = Identity();
    return Mul(Load(g_cur.w), V);
}

void StereoRegisterVS(void* shader, const DWORD* code) {
    if (!shader || !code) return;
    Layout L;
    int lp = -1, lv = -1;   // localProjectionMatrix / localViewMatrix
    // walk the token stream to the CTAB comment block
    for (size_t i = 1; i < 65536; ) {
        DWORD t = code[i];
        if (t == 0x0000FFFF) break;
        if ((t & 0xFFFF) == 0xFFFE) {
            DWORD len = (t >> 16) & 0x7FFF;
            if (len >= 8 && code[i + 1] == 0x42415443 /*'CTAB'*/) {
                const uint8_t* base = (const uint8_t*)&code[i + 2];
                size_t size = (len - 1) * 4;
                struct Hdr { DWORD Size, Creator, Version, Constants, ConstantInfo, Flags, Target; };
                struct Info { DWORD Name; WORD RegisterSet, RegisterIndex, RegisterCount, Reserved; DWORD TypeInfo, DefaultValue; };
                const Hdr* h = (const Hdr*)base;
                if (h->ConstantInfo + h->Constants * sizeof(Info) <= size) {
                    const Info* ci = (const Info*)(base + h->ConstantInfo);
                    for (DWORD k = 0; k < h->Constants; ++k) {
                        if (ci[k].Name >= size || ci[k].RegisterSet != 2 /*D3DXRS_FLOAT4*/) continue;
                        const char* n = (const char*)base + ci[k].Name;
                        if (!strcmp(n, "g_vScreenOffset") && ci[k].RegisterCount == 2 && ci[k].RegisterIndex + 2 <= 256) { L.so = ci[k].RegisterIndex; continue; }
                        if (!strcmp(n, "viewITMatrix") && ci[k].RegisterCount >= 3) { L.vit = ci[k].RegisterIndex; L.vitN = ci[k].RegisterCount >= 4 ? 4 : 3; continue; }
                        if (ci[k].RegisterCount < 4) continue;
                        if (!strcmp(n, "worldViewProjMatrix")) L.wvp = ci[k].RegisterIndex;
                        else if (!strcmp(n, "worldViewMatrix")) L.wv = ci[k].RegisterIndex;
                        else if (!strcmp(n, "worldMatrix")) L.w = ci[k].RegisterIndex;
                        else if (!strcmp(n, "localProjectionMatrix")) lp = ci[k].RegisterIndex;
                        else if (!strcmp(n, "localViewMatrix")) lv = ci[k].RegisterIndex;
                    }
                }
                break;
            }
            i += 1 + len;
        } else i += 1 + ((t >> 24) & 0xF);
    }
    // Lightning Returns only: FF XIII / XIII-2 have shaders with this pair too, but look right without it (user's test)
    if (G().game == GAME_LR && L.wvp < 0 && lp >= 0 && lv >= 0) { L.wvp = lp; L.wv = lv; L.vit = -1; L.local = true; }
    bool ok = L.wvp >= 0 && (L.wv >= 0 || (L.w >= 0 && L.vit >= 0)) && L.wvp + 4 <= 256 && (L.vit < 0 || L.vit + L.vitN <= 256) &&
              (L.wv < 0 || L.wv + 4 <= 256) && (L.w < 0 || L.w + 4 <= 256);
    if (!ok) { int so = L.so; L = Layout(); L.so = so; }
    AcquireSRWLockExclusive(&g_layoutLock);
    if (ok || L.so >= 0) g_layouts[shader] = L; else g_layouts.erase(shader);
    ReleaseSRWLockExclusive(&g_layoutLock);
}

void StereoOnSetVS(void* shader) {
    AcquireSRWLockShared(&g_layoutLock);
    auto it = g_layouts.find(shader);
    g_cur = it != g_layouts.end() ? it->second : Layout();
    ReleaseSRWLockShared(&g_layoutLock);
}

// ------------------------------------------------------------------ pixel shaders: screen-size reciprocal
// Lightning Returns' fog pass and many materials read screen-space buffers at vPos * screenSizeReciprocal
// ("$s_screenReciprocal" / "screenSizeReciprocal" = 1 / game resolution). On a resized target vPos is in headset
// pixels, so with the game's value only the top-left game-sized corner was read and the rest clamped to streaks (t11:
// the user's save area, where the height-fog pass copies the whole scene this way). The constant is divided by the
// target's scale for each draw, like g_vScreenOffset.
static std::unordered_map<void*, int> g_psRecip;   // pixel shader -> register of its screen reciprocal
static int g_curPsRecip = -1;
void StereoRegisterPS(void* shader, const DWORD* code) {
    if (!shader || !code) return;
    int reg = -1;
    bool vpos = false;
    for (size_t i = 1; i < 65536; ) {
        DWORD t = code[i];
        if (t == 0x0000FFFF) break;
        if ((t & 0xFFFF) == 0xFFFE) {
            DWORD len = (t >> 16) & 0x7FFF;
            if (len >= 8 && code[i + 1] == 0x42415443 /*'CTAB'*/) {
                const uint8_t* base = (const uint8_t*)&code[i + 2];
                size_t size = (len - 1) * 4;
                struct Hdr { DWORD Size, Creator, Version, Constants, ConstantInfo, Flags, Target; };
                struct Info { DWORD Name; WORD RegisterSet, RegisterIndex, RegisterCount, Reserved; DWORD TypeInfo, DefaultValue; };
                const Hdr* h = (const Hdr*)base;
                if (h->ConstantInfo + h->Constants * sizeof(Info) <= size) {
                    const Info* ci = (const Info*)(base + h->ConstantInfo);
                    for (DWORD k = 0; k < h->Constants; ++k) {
                        if (ci[k].Name >= size || ci[k].RegisterSet != 2 /*D3DXRS_FLOAT4*/ || ci[k].RegisterIndex >= 224) continue;
                        const char* n = (const char*)base + ci[k].Name;
                        if (!strcmp(n, "$s_screenReciprocal") || !strcmp(n, "screenSizeReciprocal") || !strcmp(n, "$screenSizeReciprocal"))
                            reg = ci[k].RegisterIndex;
                    }
                }
            }
            i += 1 + len;
            continue;
        }
        DWORD op = t & 0xFFFF, n = (t >> 24) & 0xF;
        if (op == 0x1F && n >= 2) {   // dcl: is the destination vPos (misc type 17, index 0)?
            DWORD d = code[i + 2];
            DWORD type = ((d >> 28) & 7) | (((d >> 11) & 3) << 3);
            if (type == 17 && (d & 0x7FF) == 0) vpos = true;
        }
        i += 1 + n;
    }
    AcquireSRWLockExclusive(&g_layoutLock);
    if (reg >= 0 && vpos) g_psRecip[shader] = reg; else g_psRecip.erase(shader);
    ReleaseSRWLockExclusive(&g_layoutLock);
    static int logged = 0;
    if (reg >= 0 && vpos && logged++ < 3) LOGI("stereo: pixel shader %p reads the screen at vPos * c%d (scaled per target)", shader, reg);
}
void StereoOnSetPS(void* shader) {
    AcquireSRWLockShared(&g_layoutLock);
    auto it = g_psRecip.find(shader);
    g_curPsRecip = it != g_psRecip.end() ? it->second : -1;
    ReleaseSRWLockShared(&g_layoutLock);
}

// ------------------------------------------------------------------ right-eye twins (native)
// The twin is attached to the game's resource as IUnknown private data, so it lives exactly as long as the original.
static const GUID kTwinGuid = {0x6a3c1f2e, 0x51b7, 0x4f0e, {0x9d, 0x2a, 0x13, 0x77, 0xc4, 0x08, 0xe1, 0x5b}};
static int g_privAddRefs = -1;   // does GetPrivateData AddRef the IUnknown it returns? (probed once)
static unsigned g_twins = 0;
static bool g_twinCreateFailed = false;

template <class T> static T* Twin(T* r) {
    if (!r) return nullptr;
    IUnknown* p = nullptr;
    DWORD sz = sizeof(p);
    if (FAILED(r->GetPrivateData(kTwinGuid, &p, &sz)) || !p || sz != sizeof(p)) return nullptr;
    if (g_privAddRefs == 1) p->Release();
    return (T*)p;
}

template <class T> static void Attach(T* orig, IUnknown* twin) {
    // with D3DSPD_IUNKNOWN, pData is the interface itself (D3D AddRefs it and releases it with the resource)
    orig->SetPrivateData(kTwinGuid, twin, sizeof(IUnknown*), D3DSPD_IUNKNOWN);
    if (g_privAddRefs < 0) {
        twin->AddRef();
        ULONG before = twin->Release();
        IUnknown* p = nullptr;
        DWORD sz = sizeof(p);
        orig->GetPrivateData(kTwinGuid, &p, &sz);
        twin->AddRef();
        ULONG after = twin->Release();
        g_privAddRefs = after > before ? 1 : 0;
        if (g_privAddRefs == 1) twin->Release();
        LOGI("stereo: GetPrivateData %s the twin", g_privAddRefs ? "AddRefs" : "does not AddRef");
    }
}

// Stereo targets: everything that is not square. Square targets (shadow maps 1024^2, reflections 256^2, ...) are
// eye-independent or not worth doubling, and stay shared.
static bool WantTwin(UINT w, UINT h) {
    const Settings& s = G().s;
    return s.vrEnabled && s.stereo && s.vrMethod == VR_NATIVE && w != h && w >= 8 && h >= 4;
}

void StereoOnCreateTexture(IDirect3DDevice9* dev, IDirect3DTexture9* tex, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool) {
    if (!tex || !(usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) || !WantTwin(w, h)) return;
    InternalScope in;
    IDirect3DTexture9* twin = nullptr;
    if (FAILED(dev->CreateTexture(w, h, levels, usage, f, pool, &twin, nullptr)) || !twin) {
        if (!g_twinCreateFailed) LOGE("stereo: twin texture %ux%u %s failed", w, h, FormatName(f));
        g_twinCreateFailed = true;
        return;
    }
    for (DWORD l = 0; l < tex->GetLevelCount() && l < twin->GetLevelCount(); ++l) {
        IDirect3DSurface9 *a = nullptr, *b = nullptr;
        if (SUCCEEDED(tex->GetSurfaceLevel(l, &a)) && SUCCEEDED(twin->GetSurfaceLevel(l, &b)) && a && b) Attach(a, b);
        if (a) a->Release();
        if (b) b->Release();
    }
    Attach(tex, twin);
    twin->Release();   // now owned by the original's private data
    g_twins++;
    LOGD("stereo: twin for texture %ux%u %s usage %lx", w, h, FormatName(f), usage);
}

void StereoOnCreateSurface(IDirect3DDevice9* dev, IDirect3DSurface9* s, bool depth, UINT w, UINT h, D3DFORMAT f,
                           D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL flag) {
    if (!s || !WantTwin(w, h)) return;
    InternalScope in;
    IDirect3DSurface9* twin = nullptr;
    HRESULT hr = depth ? dev->CreateDepthStencilSurface(w, h, f, ms, q, flag, &twin, nullptr)
                       : dev->CreateRenderTarget(w, h, f, ms, q, flag, &twin, nullptr);
    if (FAILED(hr) || !twin) {
        if (!g_twinCreateFailed) LOGE("stereo: twin surface %ux%u %s failed %08lX", w, h, FormatName(f), hr);
        g_twinCreateFailed = true;
        return;
    }
    Attach(s, twin);
    twin->Release();
    g_twins++;
    LOGD("stereo: twin for %s surface %ux%u %s msaa %d", depth ? "depth" : "colour", w, h, FormatName(f), (int)ms);
}

// ------------------------------------------------------------------ bound state (shadows; the device is pure)
static IDirect3DSurface9* g_rt[4] = {};
static IDirect3DSurface9* g_rtTwin[4] = {};
static IDirect3DSurface9* g_ds = nullptr;
static IDirect3DSurface9* g_dsTwin = nullptr;
static IDirect3DBaseTexture9* g_tex[20] = {};      // 0..15 pixel samplers, 16..19 vertex samplers
static IDirect3DBaseTexture9* g_texTwin[20] = {};
static UINT g_rt0W = 0, g_rt0H = 0;
static D3DVIEWPORT9 g_vp{};
static RECT g_scissor{};

static DWORD StageId(int i) { return i < 16 ? (DWORD)i : D3DVERTEXTEXTURESAMPLER0 + (i - 16); }

void StereoOnSetRT(IDirect3DDevice9*, DWORD i, IDirect3DSurface9* s) {
    if (i >= 4) return;
    g_rt[i] = s;
    g_rtTwin[i] = Native() ? Twin(s) : nullptr;
    if (i == 0) {
        g_rt0W = g_rt0H = 0;
        D3DSURFACE_DESC d;
        { InternalScope in; if (s && SUCCEEDED(s->GetDesc(&d))) { g_rt0W = d.Width; g_rt0H = d.Height; } }   // real size
        // SetRenderTarget(0) resets the viewport and scissor rect to the whole target (kept in the game's units)
        ScaleInfo si;
        UINT gw = g_rt0W, gh = g_rt0H;
        if (RtGetInfo(s, &si)) { gw = si.origW; gh = si.origH; }
        g_vp = {0, 0, gw, gh, 0.0f, 1.0f};
        g_scissor = {0, 0, (LONG)gw, (LONG)gh};
    }
}

// Our own viewport/scissor restores bypass the hooks, so they convert the game-unit shadows themselves.
static void ApplyViewport(IDirect3DDevice9* dev, IDirect3DSurface9* gameRt0) {
    ScaleInfo si;
    if (RtGetInfo(gameRt0, &si)) {
        D3DVIEWPORT9 v = RtScaleViewport(g_vp, si);
        RECT r = RtScaleRect(g_scissor, si);
        dev->SetViewport(&v);
        dev->SetScissorRect(&r);
    } else {
        dev->SetViewport(&g_vp);
        dev->SetScissorRect(&g_scissor);
    }
}
void StereoOnSetDS(IDirect3DSurface9* s) { g_ds = s; g_dsTwin = Native() ? Twin(s) : nullptr; }
void StereoOnSetTexture(DWORD stage, IDirect3DBaseTexture9* t) {
    int i = stage < 16 ? (int)stage : (stage >= D3DVERTEXTEXTURESAMPLER0 && stage <= D3DVERTEXTEXTURESAMPLER3) ? 16 + (int)(stage - D3DVERTEXTEXTURESAMPLER0) : -1;
    if (i < 0) return;
    g_tex[i] = t;
    g_texTwin[i] = Native() ? Twin(t) : nullptr;
}
void StereoOnViewport(const D3DVIEWPORT9* vp) { if (vp) g_vp = *vp; }
void StereoOnScissor(const RECT* r) { if (r) g_scissor = *r; }

// ------------------------------------------------------------------ eye / HUD textures
static UINT g_bbW = 0, g_bbH = 0;
static IDirect3DSurface9* g_uiTarget = nullptr;   // the game's UI/compose target (copied to the back buffer)
static bool g_uiPhase = false;                      // between the scene copy into g_uiTarget and its copy to the back buffer

static IDirect3DTexture9* g_eyeTex[2] = {};
static IDirect3DSurface9* g_eyeSurf[2] = {};
static IDirect3DTexture9* g_hudTex = nullptr;
static IDirect3DSurface9* g_hudSurf = nullptr;
static UINT g_texW = 0, g_texH = 0;

static void ReleaseTextures() {
    for (int e = 0; e < 2; ++e) {
        if (g_eyeSurf[e]) g_eyeSurf[e]->Release();
        if (g_eyeTex[e]) g_eyeTex[e]->Release();
        g_eyeSurf[e] = nullptr; g_eyeTex[e] = nullptr;
    }
    if (g_hudSurf) g_hudSurf->Release();
    if (g_hudTex) g_hudTex->Release();
    g_hudSurf = nullptr; g_hudTex = nullptr;
    g_texW = g_texH = 0;
}

// Eye and HUD textures at the size of the game's full-screen targets (the headset resolution when they are scaled;
// the HUD must match the UI target's pixel grid because UI draws use its stencil).
static bool EnsureTextures(IDirect3DDevice9* dev) {
    UINT ew = RtScaleActive() ? RtSceneW() : g_bbW, eh = RtScaleActive() ? RtSceneH() : g_bbH;
    if (g_texW == ew && g_texH == eh && g_hudSurf) return true;
    ReleaseTextures();
    InternalScope in;
    bool ok = true;
    for (int e = 0; e < 2 && ok; ++e)
        ok = SUCCEEDED(dev->CreateTexture(ew, eh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_eyeTex[e], nullptr)) &&
             SUCCEEDED(g_eyeTex[e]->GetSurfaceLevel(0, &g_eyeSurf[e]));
    ok = ok && SUCCEEDED(dev->CreateTexture(ew, eh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_hudTex, nullptr)) &&
         SUCCEEDED(g_hudTex->GetSurfaceLevel(0, &g_hudSurf));
    if (!ok) { LOGE("stereo: could not create eye/HUD textures %ux%u", ew, eh); ReleaseTextures(); return false; }
    g_texW = ew; g_texH = eh;
    LOGI("stereo: eye and HUD textures %ux%u created (%s)", ew, eh, Native() ? "native" : "alternate-eye frames");
    return true;
}

// ------------------------------------------------------------------ per-frame state
static bool g_active = false;      // stereo enabled for the frame being rendered
static int g_eye = 0;              // AER: eye of the frame being rendered
static M4 g_E[2], g_Einv[2];       // view space -> eye space and back, per eye
static bool g_haveP = false;       // projection derived this frame
static M4 g_P[2];                  // projection used for each eye
static M4 g_Pg;                    // latest main-scene projection, full matrix (Lightning Returns' clip-space correction)
static bool g_havePg = false;
static M4 g_C[2];                  // Lightning Returns: clip-space correction per eye (see EnsureClipCorrection)
static bool g_haveC = false;       // C computed for this frame
static bool g_frameStereo = false; // this frame's scene was captured
static bool g_eyeValid[2] = {};
static unsigned g_patched = 0, g_hudDraws = 0, g_rightDraws = 0, g_mirrored = 0;
static bool g_didPatch = false;    // constants of the current draw were changed
// Set once this frame's full copy into the UI target happened (every frame, even movies). In gameplay every 3D draw
// comes before it; what comes after is the 2D layer (UI, and the title screen's 3D backdrop).
static bool g_sceneCopyThisFrame = false, g_sceneCopyLastFrame = false;

// Lightning Returns' start-up: after the opening movie it plays a real-time 3D logo animation drawn straight into the UI
// target (no scene pass, no HUD: it went to the headset in stereo and looked broken), and its title screen draws its crest
// with a 3D camera into the UI target (shown on the flat screen, but head-tracked: it moved against the head). So in LR
// nothing is head-tracked or put in stereo until the first frame that looks like gameplay: a scene copy after at least
// kLrMinScene camera draws, followed by at least kLrMinHud draws of the HUD layer (gameplay: ~100 and ~54; the logo
// animation and the title fail both). From then on the normal rules apply (real-time cutscenes stay in stereo).
// Known limit: after quitting to the title the animation and title are treated normally again (gate stays open).
static const unsigned kLrMinScene = 20, kLrMinHud = 10;
static bool g_lrOpen = false;                  // gameplay seen
static unsigned g_lrWould = 0;                 // camera draws this frame that would have been patched
static bool g_lrProbe = false;                 // this frame had a scene copy after enough camera draws
static unsigned g_lrProbeUi = 0, g_lrProbeScene = 0;   // HUD-layer draws after that copy / camera draws before it
// Two separate switches (t10): lr_flat_until_play = no stereo frames before gameplay (the logo animation stays on the
// flat screen); lr_still_until_play = no head tracking before gameplay (the title screen's crest). The right eye is always
// drawn (t9 skipped it before gameplay: a target the game draws once at start-up then stayed black in the right eye,
// "a transparent black block over the right eye" in the game).
static bool LrGateActive() { return G().game == GAME_LR && !g_lrOpen && (G().s.lrFlatUntilPlay || G().s.lrStillUntilPlay); }
static bool LrFlat() { return LrGateActive() && G().s.lrFlatUntilPlay; }
static bool LrStill() { return LrGateActive() && G().s.lrStillUntilPlay; }

static xrb::IpcPose g_framePose[2];
static xrb::IpcFov g_frameFov[2];
static int64_t g_frameTime = 0;
static xrb::IpcPose g_eyePose[2];   // pose each eye texture was rendered with
static xrb::IpcFov g_eyeFov[2];
static int64_t g_eyeTime[2] = {};
static xrb::IpcFov g_gameFov{-0.55f, 0.55f, 0.33f, -0.33f};

static unsigned g_statFrames = 0, g_statStereo = 0, g_statPatched = 0, g_statHud = 0, g_statRight = 0, g_statMirror = 0;

void StereoOnReset() {
    ReleaseTextures();
    for (int e = 0; e < 2; ++e) g_eyeValid[e] = false;
    g_uiTarget = nullptr;
    g_uiPhase = false;
    memset(g_rt, 0, sizeof(g_rt)); memset(g_rtTwin, 0, sizeof(g_rtTwin));
    g_ds = g_dsTwin = nullptr;
    memset(g_tex, 0, sizeof(g_tex)); memset(g_texTwin, 0, sizeof(g_texTwin));
}

// Eye transform from an OpenXR eye pose (relative to the recentered reference = the game camera).
static void BuildEye(int e, const xrb::IpcPose& pin) {
    const Settings& s = G().s;
    xrb::IpcPose p = pin;
    if (s.debugHeadYaw != 0) {   // test aid without a headset: extra yaw (deg) applied to the head
        float a = s.debugHeadYaw * 3.14159265f / 180 / 2;
        float yy = sinf(a), yw = cosf(a);                          // q = q_yaw(0, yy, 0, yw) * q
        float qx = yw * p.qx + yy * p.qz, qy = yw * p.qy + yy * p.qw, qz = yw * p.qz - yy * p.qx, qw = yw * p.qw - yy * p.qy;
        p.qx = qx; p.qy = qy; p.qz = qz; p.qw = qw;
        float c = cosf(2 * a), sn = sinf(2 * a);                   // rotate the position too
        float px = c * p.px + sn * p.pz, pz = -sn * p.px + c * p.pz;
        p.px = px; p.pz = pz;
    }
    if (s.debugHeadPitch != 0) {   // test aid: extra pitch (deg, + = look up) applied to the head
        float a = s.debugHeadPitch * 3.14159265f / 180 / 2;
        float xx = sinf(a), xw = cosf(a);                          // q = q_pitch(xx, 0, 0, xw) * q
        float qx = xw * p.qx + xx * p.qw, qy = xw * p.qy - xx * p.qz, qz = xw * p.qz + xx * p.qy, qw = xw * p.qw - xx * p.qx;
        p.qx = qx; p.qy = qy; p.qz = qz; p.qw = qw;
        float c = cosf(2 * a), sn = sinf(2 * a);
        float py = c * p.py - sn * p.pz, pz = sn * p.py + c * p.pz;
        p.py = py; p.pz = pz;
    }
    float sc = s.worldScale;
    float tx = p.px * sc, ty = p.py * sc, tz = p.pz * sc;
    float x = p.qx, y = p.qy, z = p.qz, w = p.qw;
    float R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                     {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                     {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    // E = T(-t) * R4 ;  Einv = R4^T * T(t)
    M4 R4 = Identity(), T = Identity(), Ti = Identity();
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) R4.m[i][j] = R[i][j];
    T.m[3][0] = -tx; T.m[3][1] = -ty; T.m[3][2] = -tz;
    Ti.m[3][0] = tx; Ti.m[3][1] = ty; Ti.m[3][2] = tz;
    g_E[e] = Mul(T, R4);
    g_Einv[e] = Mul(Transpose(R4), Ti);
}

void StereoBeginFrame(IDirect3DDevice9* dev) {
    g_statFrames++;
    if (g_frameStereo) g_statStereo++;
    g_statPatched += g_patched; g_statHud += g_hudDraws; g_statRight += g_rightDraws; g_statMirror += g_mirrored;
    if (g_lrProbe && !g_lrOpen && g_lrProbeUi >= kLrMinHud) {
        g_lrOpen = true;
        LOGI("stereo: Lightning Returns gameplay detected at frame %ld (%u camera draws, %u HUD-layer draws) - stereo on", G().frame,
             g_lrProbeScene, g_lrProbeUi);
    }
    g_lrProbe = false;
    g_lrProbeUi = g_lrProbeScene = g_lrWould = 0;
    g_frameStereo = false;
    g_sceneCopyLastFrame = g_sceneCopyThisFrame;
    g_sceneCopyThisFrame = false;
    g_patched = g_hudDraws = g_rightDraws = g_mirrored = 0;
    g_haveP = false;
    g_haveC = false;
    g_uiPhase = false;
    g_active = false;
    if (!G().s.stereo || !xrb::ClientHostConnected()) return;
    IDirect3DSurface9* bb = nullptr;
    if (!(bb = GameBackBuffer(dev))) return;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    bb->Release();
    g_bbW = d.Width; g_bbH = d.Height;
    if (!EnsureTextures(dev)) return;
    xrb::IpcPose pose[2];
    xrb::IpcFov fov[2];
    int64_t t = 0;
    if (!xrb::ClientGetLatestPoses(pose, fov, &t)) return;
    g_eye = (int)(G().frame & 1);
    memcpy(g_framePose, pose, sizeof(pose));
    memcpy(g_frameFov, fov, sizeof(fov));
    g_frameTime = t;
    BuildEye(0, pose[0]);
    BuildEye(1, pose[1]);
    g_active = true;
}

// The game's own projection of the latest frame (main scene camera), for the culling FOV search (camfov.cpp).
static float g_gameP00 = 0, g_gameP11 = 0;
static long g_gamePFrame = -1;
bool StereoGameProjection(float* p00, float* p11, long* frame) {
    if (g_gamePFrame < 0) return false;
    *p00 = g_gameP00; *p11 = g_gameP11; *frame = g_gamePFrame;
    return true;
}

// fov_mode 1: replaces the field of view of projection P by the headset's for eye e (its depth mapping stays).
static void EyeFov(M4* P, int e) {
    if (G().s.fovMode != 1) return;
    const xrb::IpcFov& f = g_frameFov[e];
    float L = tanf(f.left), R = tanf(f.right), U = tanf(f.up), D = tanf(f.down);
    P->m[0][0] = 2 / (R - L);
    P->m[1][1] = 2 / (U - D);
    P->m[2][0] = (R + L) / (R - L);
    P->m[2][1] = (U + D) / (U - D);
}

// Projection per eye. fov_mode 0: the game's own projection (the image is shown with the game's FOV).
// fov_mode 1: the headset's FOV for that eye (wider; the game still culls with its own frustum).
static void DeriveProjection(const M4& WV, const M4& WVP) {
    M4 iwv, Pg;
    if (!Inverse(WV, &iwv)) return;
    Pg = Mul(iwv, WVP);
    // RH, row-vector: ndc.x = P00 * (x / -z) - P20  ->  edges at tan = (+-1 + P20) / P00
    float l = (-1 + Pg.m[2][0]) / Pg.m[0][0], r = (1 + Pg.m[2][0]) / Pg.m[0][0];
    float d = (-1 + Pg.m[2][1]) / Pg.m[1][1], u = (1 + Pg.m[2][1]) / Pg.m[1][1];
    g_gameFov = {atanf(l), atanf(r), atanf(u), atanf(d)};
    g_gameP00 = Pg.m[0][0]; g_gameP11 = Pg.m[1][1]; g_gamePFrame = G().frame;
    g_Pg = Pg; g_havePg = true;
    for (int e = 0; e < 2; ++e) {
        g_P[e] = Pg;
        EyeFov(&g_P[e], e);
    }
    g_haveP = true;
    static bool logged = false;
    if (!logged) {
        logged = true;
        LOGI("stereo: game projection P00 %.4f P11 %.4f P20 %.4f P21 %.4f P22 %.6f P23 %.3f P32 %.5f -> FOV L%.1f R%.1f U%.1f D%.1f deg",
             Pg.m[0][0], Pg.m[1][1], Pg.m[2][0], Pg.m[2][1], Pg.m[2][2], Pg.m[2][3], Pg.m[3][2], g_gameFov.left * 57.2958f,
             g_gameFov.right * 57.2958f, g_gameFov.up * 57.2958f, g_gameFov.down * 57.2958f);
    }
}

// Lightning Returns' depth pass (local layout) comes before the main scene and may use its own depth range: it gets
// its own projection (the draw's, with the eye's FOV) instead of the frame's, and is patched only when its FOV is the
// game camera's (latest main-scene projection), so a pass with another camera (a light) is left alone. t4 derived the
// frame's projection from this pass (the first patchable draw), which flashed the whole image.
static unsigned g_localSkipped = 0;

// Lightning Returns draws its scene depth pass (local layout, also fills the depth buffer S7) and then the colour pass
// with depth writes off on top of it: the two must produce bit-identical depth for the same object, or the colour
// pixels fail the depth test (t4/t5: the whole image black on the dev PC, flashing on the user's). The two passes used
// to get differently computed matrices (localProjectionMatrix with a per-draw projection vs W*V rebuilt from viewITMatrix
// with the frame's projection), so float rounding differed. Now every patched LR draw gets WVP' = WVP_game * C[eye], with
// one clip-space correction per frame C = inverse(P) * E * P_eye: the game's own WVPs already agree between the passes,
// so the patched ones do too. P is the latest main-scene projection (the previous frame's until this frame's is known).
static bool ClipFix() { return G().game == GAME_LR && G().s.lrClipFix; }
static bool EnsureClipCorrection() {
    if (g_haveC) return true;
    M4 ip;
    if (!g_havePg || !Inverse(g_Pg, &ip)) return false;
    for (int e = 0; e < 2; ++e) {
        M4 P = g_Pg;
        EyeFov(&P, e);
        g_C[e] = Mul(Mul(ip, g_E[e]), P);
    }
    g_haveC = true;
    return true;
}

// The eye's matrices for the current draw. false: leave the draw as the game set it.
static bool EyeMatrices(int eye, M4* WVP2, M4* WV2, M4* VIT2) {
    if (g_cur.wvp < 0 || LrStill()) return false;
    M4 WV, WVP = Load(g_cur.wvp), P;
    bool ownP = false;
    if (g_cur.local) {
        if (!G().s.lrDepthStereo) return false;   // A/B: depth pass left mono (as before t4)
        M4 iwv;
        WV = Load(g_cur.wv);
        if (g_gamePFrame < 0 || !Inverse(WV, &iwv)) return false;
        P = Mul(iwv, WVP);   // a pass with another camera (a light) is left alone
        if (fabsf(P.m[0][0] - g_gameP00) > 0.03f * fabsf(g_gameP00) || fabsf(P.m[1][1] - g_gameP11) > 0.03f * fabsf(g_gameP11)) {
            g_localSkipped++;
            return false;
        }
        ownP = true;
    } else {
        WV = LoadWV();
        if (!g_haveP) DeriveProjection(WV, WVP);
        if (!g_haveP) return false;
    }
    *WV2 = Mul(WV, g_E[eye]);
    if (ClipFix() && EnsureClipCorrection()) *WVP2 = Mul(WVP, g_C[eye]);
    else if (ownP) { EyeFov(&P, eye); *WVP2 = Mul(*WV2, P); }
    else *WVP2 = Mul(*WV2, g_P[eye]);
    if (g_cur.vit >= 0) *VIT2 = Transpose(Mul(g_Einv[eye], Transpose(LoadVIT())));
    return true;
}

// Uploads the eye's matrices for the current draw (if its vertex shader has the camera layout).
static void Patch(IDirect3DDevice9* dev, int eye) {
    M4 WVP2, WV2, VIT2;
    if (!EyeMatrices(eye, &WVP2, &WV2, &VIT2)) return;
    InternalScope in;
    dev->SetVertexShaderConstantF(g_cur.wvp, &WVP2.m[0][0], 4);
    if (g_cur.wv >= 0) dev->SetVertexShaderConstantF(g_cur.wv, &WV2.m[0][0], 4);
    if (g_cur.vit >= 0) dev->SetVertexShaderConstantF(g_cur.vit, &VIT2.m[0][0], g_cur.vitN);
    g_didPatch = true;
}

// Camera world position of the last main-scene 3D draw (inverse(V) = transpose(VIT): translation = VIT column 3).
static float g_camPos[3] = {};
void StereoCameraPos(float out[3]) { memcpy(out, g_camPos, sizeof(g_camPos)); }
// Its rotation: rows of inverse(V) = the view axes in world space (row-vector, x_world = x_view * rot).
static float g_camRot[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
// Camera of the newest captured scene, and the game frame each eye image comes from (for reprojection).
static float g_sceneRot[3][3];
static long g_sceneFrame = -1, g_eyeFrame[2] = {-1, -1};
bool StereoSceneCamera(float rot[3][3], long* frame, long eyeFrame[2]) {
    if (g_sceneFrame < 0) return false;
    memcpy(rot, g_sceneRot, sizeof(g_sceneRot));
    *frame = g_sceneFrame;
    eyeFrame[0] = g_eyeFrame[0]; eyeFrame[1] = g_eyeFrame[1];
    return true;
}

// Post-processing passes add a half-pixel offset (g_vScreenOffset, in clip units = 1/width) computed for the game's
// resolution; on a scaled target it is divided by the scale so it stays half a pixel.
static bool g_soPatched = false;
static int g_psrPatched = -1;   // pixel shader register whose screen reciprocal is patched for this draw
static bool PsRecipScaled(float out[4]) {
    ScaleInfo si;
    if (g_curPsRecip < 0 || !G().s.screenRecipFix || !RtRT0Scaled(&si)) return false;
    memcpy(out, Sh().psc[g_curPsRecip], 16);
    out[0] /= si.sx;
    out[1] /= si.sy;
    return true;
}
static void PatchScreenOffset(IDirect3DDevice9* dev) {
    g_psrPatched = -1;
    float r[4];
    if (PsRecipScaled(r)) {
        InternalScope in;
        dev->SetPixelShaderConstantF(g_curPsRecip, r, 1);
        g_psrPatched = g_curPsRecip;
    }
    g_soPatched = false;
    ScaleInfo si;
    if (g_cur.so < 0 || !RtRT0Scaled(&si)) return;
    float a[4], b[4];
    memcpy(a, Sh().vsc[g_cur.so], sizeof(a));
    memcpy(b, Sh().vsc[g_cur.so + 1], sizeof(b));
    a[0] /= si.sx;
    b[0] /= si.sy;
    InternalScope in;
    dev->SetVertexShaderConstantF(g_cur.so, a, 1);
    dev->SetVertexShaderConstantF(g_cur.so + 1, b, 1);
    g_soPatched = true;
}

// Lightning Returns has no StretchRect into the UI target: its post-processing result is drawn into the UI target (the
// scene target S6, which is then copied to the back buffer) by one full-screen draw, the first one there without a depth
// buffer, sampling that result (a render target of the same size). That draw is the scene copy.
static void CaptureScene(IDirect3DDevice9* dev, IDirect3DSurface9* src);
static bool SceneCopyDraw(IDirect3DDevice9* dev) {
    if (G().game != GAME_LR || !g_uiTarget || g_rt[0] != g_uiTarget || g_ds || !g_tex[0]) return false;
    if (g_patched == 0 && g_rightDraws == 0 && g_lrWould == 0) return false;   // no 3D scene this frame
    if (g_tex[0]->GetType() != D3DRTYPE_TEXTURE) return false;
    IDirect3DTexture9* t = (IDirect3DTexture9*)g_tex[0];
    D3DSURFACE_DESC d{};
    IDirect3DSurface9* s = nullptr;
    {
        InternalScope in;
        if (FAILED(t->GetLevelDesc(0, &d)) || !(d.Usage & D3DUSAGE_RENDERTARGET) || d.Width != g_rt0W || d.Height != g_rt0H) return false;
        if (FAILED(t->GetSurfaceLevel(0, &s)) || !s) return false;
    }
    static int logged = 0;
    if (logged++ < 1) LOGI("stereo: scene copy by a draw into the UI target from %p %ux%u %s (twin %p), frame %ld", s, d.Width, d.Height,
                            FormatName(d.Format), Twin(s), G().frame);
    CaptureScene(dev, s);
    s->Release();
    return true;
}

int StereoPreDraw(IDirect3DDevice9* dev) {
    g_didPatch = false;
    PatchScreenOffset(dev);
    if (g_cur.vit >= 0 && g_rt0W != g_rt0H && g_rt0W >= 640) {
        const float(*c)[4] = Sh().vsc + g_cur.vit;
        g_camPos[0] = c[0][3]; g_camPos[1] = c[1][3]; g_camPos[2] = c[2][3];
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) g_camRot[i][j] = c[j][i];
    }
    if (!g_active) return SD_NONE;
    if (!g_sceneCopyThisFrame && SceneCopyDraw(dev)) return SD_NONE;   // Lightning Returns: the copy is a draw
    if (g_uiPhase) return (g_rt[0] == g_uiTarget && g_hudSurf) ? SD_HUD : SD_NONE;
    // Draws after this frame's copy into the UI target belong to the 2D layer. The title screen draws its 3D backdrop
    // there (into the UI target): it must not be head-tracked, it is shown on the flat screen.
    if (g_sceneCopyThisFrame) {
        if (g_lrProbe && g_rt[0] == g_uiTarget) g_lrProbeUi++;   // Lightning Returns before gameplay: count the HUD layer
        return SD_NONE;
    }
    // Lightning Returns before gameplay: count the camera draws (gameplay detection); the draw goes on as usual (right eye
    // included), only unpatched while lr_still_until_play holds (EyeMatrices)
    if (LrGateActive() && g_cur.wvp >= 0 && (!Native() || g_rtTwin[0])) g_lrWould++;
    if (Native()) {
        if (!g_rtTwin[0]) return SD_NONE;            // shared (eye-independent) target, e.g. a shadow map
        Patch(dev, 0);
        if (g_didPatch) g_patched++;
        return SD_NATIVE;
    }
    UINT sw = RtScaleActive() ? RtSceneW() : g_bbW, sh = RtScaleActive() ? RtSceneH() : g_bbH;
    if (g_cur.wvp < 0 || g_rt0W != sw || g_rt0H != sh) return SD_NONE;
    Patch(dev, g_eye);
    if (!g_didPatch) return SD_NONE;
    g_patched++;
    return SD_AER;
}

void StereoPostDraw(IDirect3DDevice9* dev, int) {
    if (g_psrPatched >= 0) {
        InternalScope in;
        dev->SetPixelShaderConstantF(g_psrPatched, Sh().psc[g_psrPatched], 1);
        g_psrPatched = -1;
    }
    if (g_soPatched) {
        g_soPatched = false;
        InternalScope in;
        dev->SetVertexShaderConstantF(g_cur.so, Sh().vsc[g_cur.so], 2);
    }
    if (!g_didPatch) return;
    g_didPatch = false;
    InternalScope in;   // put the game's values back (it may not set them again for the next draw)
    dev->SetVertexShaderConstantF(g_cur.wvp, Sh().vsc[g_cur.wvp], 4);
    if (g_cur.wv >= 0) dev->SetVertexShaderConstantF(g_cur.wv, Sh().vsc[g_cur.wv], 4);
    if (g_cur.vit >= 0) dev->SetVertexShaderConstantF(g_cur.vit, Sh().vsc[g_cur.vit], g_cur.vitN);
}

// ---- native: bind the right-eye twins of everything bound (targets, depth, sampled textures)
static void BindTargets(IDirect3DDevice9* dev, bool right) {
    for (int i = 0; i < 4; ++i) {
        if (!g_rt[i] || (i > 0 && !g_rtTwin[i])) continue;   // MRT slots without a twin stay as they are
        IDirect3DSurface9* s = right && g_rtTwin[i] ? g_rtTwin[i] : g_rt[i];
        dev->SetRenderTarget(i, s);
    }
    if (g_dsTwin) dev->SetDepthStencilSurface(right ? g_dsTwin : g_ds);
    ApplyViewport(dev, g_rt[0]);   // twins have the same size as the game's target
}

static void BindTextures(IDirect3DDevice9* dev, bool right) {
    for (int i = 0; i < 20; ++i)
        if (g_texTwin[i]) dev->SetTexture(StageId(i), right ? g_texTwin[i] : g_tex[i]);
}

// ---- native, batched (stereobatch.cpp): what the per-draw path does, as data
void StereoBindPassTargets(IDirect3DDevice9* dev, bool right) { BindTargets(dev, right); }
IDirect3DBaseTexture9* StereoTwinTexture(IDirect3DBaseTexture9* t) { return Native() ? Twin(t) : nullptr; }
void StereoNoteRightDraws(unsigned n) { g_rightDraws += n; }
IDirect3DSurface9* StereoBoundRT(DWORD i) { return i < 4 ? g_rt[i] : nullptr; }
IDirect3DSurface9* StereoBoundDS() { return g_ds; }
bool StereoClearWouldMirror() { return g_active && Native() && !g_uiPhase && g_sceneCopyLastFrame && (g_rtTwin[0] || g_dsTwin); }

// The right eye's constants for the current draw (call between the game's draw and StereoPostDraw).
int StereoRightPatches(StereoPatchReg out[kStereoMaxPatches]) {
    int n = 0;
    auto add = [&](int reg, int count, const float* right) {
        StereoPatchReg& p = out[n++];
        p.reg = reg; p.count = count; p.ps = 0;
        memcpy(p.right, right, count * 16);
        memcpy(p.game, Sh().vsc[reg], count * 16);
    };
    if (g_psrPatched >= 0) {   // the screen reciprocal, as PatchScreenOffset set it
        StereoPatchReg& p = out[n++];
        p.reg = g_psrPatched; p.count = 1; p.ps = 1;
        PsRecipScaled(p.right[0]);
        memcpy(p.game, Sh().psc[g_psrPatched], 16);
    }
    M4 WVP2, WV2, VIT2;
    if (g_cur.wvp >= 0 && (g_haveP || g_cur.local) && EyeMatrices(1, &WVP2, &WV2, &VIT2)) {
        add(g_cur.wvp, 4, &WVP2.m[0][0]);
        if (g_cur.wv >= 0) add(g_cur.wv, 4, &WV2.m[0][0]);
        if (g_cur.vit >= 0) add(g_cur.vit, g_cur.vitN, &VIT2.m[0][0]);
    }
    ScaleInfo si;
    if (g_soPatched && g_cur.so >= 0 && RtRT0Scaled(&si)) {   // the half-pixel offset, as PatchScreenOffset set it
        float ab[2][4];
        memcpy(ab, Sh().vsc[g_cur.so], sizeof(ab));
        ab[0][0] /= si.sx;
        ab[1][0] /= si.sy;
        add(g_cur.so, 2, &ab[0][0]);
    }
    return n;
}

bool StereoRightBegin(IDirect3DDevice9* dev) {
    static bool warned = false;
    if (!warned && ((g_ds && !g_dsTwin) || (g_rt[1] && !g_rtTwin[1]))) {
        warned = true;
        LOGW("stereo: a stereo draw has a shared depth buffer or MRT target - the right eye may overwrite the left");
    }
    InternalScope in;
    if (!G().s.debugNativeNoSwitch) {
        BindTargets(dev, true);
        BindTextures(dev, true);
    }
    if (g_cur.wvp >= 0 && (g_haveP || g_cur.local)) {
        bool keep = g_didPatch;
        Patch(dev, 1);
        g_didPatch = keep || g_didPatch;
    }
    g_rightDraws++;
    return true;
}

void StereoRightEnd(IDirect3DDevice9* dev) {
    if (G().s.debugNativeNoSwitch) return;
    InternalScope in;
    BindTargets(dev, false);
    BindTextures(dev, false);
}

void StereoAfterClear(IDirect3DDevice9* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR c, float z, DWORD s) {
    if (!g_active || !Native() || g_uiPhase || !g_sceneCopyLastFrame || (!g_rtTwin[0] && !g_dsTwin)) return;
    InternalScope in;
    BindTargets(dev, true);
    dev->Clear(count, rects, flags, c, z, s);
    BindTargets(dev, false);
    g_mirrored++;
}

void StereoAfterStretchRect(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const RECT* dr,
                            D3DTEXTUREFILTERTYPE f) {
    if (!g_active || !Native() || !src || !dst || dst == g_uiTarget || g_uiPhase || !g_sceneCopyLastFrame) return;
    IDirect3DSurface9* dt = Twin(dst);
    if (!dt) return;
    IDirect3DSurface9* st = Twin(src);
    InternalScope in;
    dev->StretchRect(st ? st : src, sr, dt, dr, f);
    g_mirrored++;
}

// ---- HUD: UI draws are repeated into g_hudSurf (cleared transparent) with alpha accumulated as
// a_dst = a_src + a_dst * (1 - a_src), so the result is premultiplied over transparent black.
static DWORD RsOr(D3DRENDERSTATETYPE s, DWORD def) { return Sh().rsKnown[s] ? Sh().rs[s] : def; }

bool StereoHudBegin(IDirect3DDevice9* dev) {
    if (!g_hudSurf) return false;
    InternalScope in;
    dev->SetRenderTarget(0, g_hudSurf);
    ApplyViewport(dev, g_rt[0]);   // the HUD texture has the UI target's (possibly scaled) size: same pixel grid as its stencil
    if (RsOr(D3DRS_ALPHABLENDENABLE, FALSE)) {
        dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
        dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
        dev->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    }
    g_hudDraws++;
    return true;
}

void StereoHudEnd(IDirect3DDevice9* dev) {
    InternalScope in;
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, RsOr(D3DRS_SEPARATEALPHABLENDENABLE, FALSE));
    dev->SetRenderState(D3DRS_SRCBLENDALPHA, RsOr(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE));
    dev->SetRenderState(D3DRS_DESTBLENDALPHA, RsOr(D3DRS_DESTBLENDALPHA, D3DBLEND_ZERO));
    dev->SetRenderState(D3DRS_BLENDOPALPHA, RsOr(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD));
    dev->SetRenderTarget(0, g_rt[0]);
    ApplyViewport(dev, g_rt[0]);
}

static void CaptureScene(IDirect3DDevice9* dev, IDirect3DSurface9* src);

void StereoOnStretchRect(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const RECT* dr) {
    if (!src || !dst) return;
    // learn the UI target: the surface the game copies to the back buffer
    IDirect3DSurface9* bb = nullptr;
    bool toBB = false;
    if ((bb = GameBackBuffer(dev)) != nullptr) { toBB = dst == bb; bb->Release(); }
    if (toBB) {
        if (g_uiTarget != src) {
            D3DSURFACE_DESC d;
            src->GetDesc(&d);
            LOGI("stereo: UI target is %p (%ux%u %s)", src, d.Width, d.Height, FormatName(d.Format));
            g_uiTarget = src;
        }
        g_uiPhase = false;
        return;
    }
    if (dst != g_uiTarget) return;
    {
        static int logged = 0;
        D3DSURFACE_DESC d{};
        src->GetDesc(&d);
        if (logged < 1) {
            logged++;
            LOGI("stereo: copy into UI target from %p %ux%u %s (twin %p) rects %d/%d, frame %ld, patched so far %u", src, d.Width, d.Height,
                 FormatName(d.Format), Twin(src), sr != nullptr, dr != nullptr, G().frame, g_patched);
        }
    }
    if (sr || dr) return;
    CaptureScene(dev, src);
}

// The finished scene (no UI yet) is being copied into the UI target: keep it as the eye image(s). From here on the
// frame's draws into the UI target are the 2D layer.
static void CaptureScene(IDirect3DDevice9* dev, IDirect3DSurface9* src) {
    g_sceneCopyThisFrame = true;
    if (!g_active) return;
    if (LrGateActive()) {   // before gameplay: check for gameplay; with lr_flat_until_play not shown in stereo
        if (g_lrWould >= kLrMinScene) { g_lrProbe = true; g_lrProbeScene = g_lrWould; }
        if (LrFlat()) return;
    }
    IDirect3DSurface9* right = Native() ? Twin(src) : nullptr;
    // no 3D scene this frame: no draw had the game camera (right-eye draws alone don't count: Lightning Returns' menus
    // draw into twinned targets too, and were shown in stereo)
    if (g_patched == 0 || (Native() && !right)) return;
    InternalScope in;
    bool ok = true;
    if (Native()) {
        ok = SUCCEEDED(dev->StretchRect(src, nullptr, g_eyeSurf[0], nullptr, D3DTEXF_NONE)) &&
             SUCCEEDED(dev->StretchRect(right, nullptr, g_eyeSurf[1], nullptr, D3DTEXF_NONE));
        for (int e = 0; e < 2 && ok; ++e) {
            g_eyeValid[e] = true;
            g_eyePose[e] = g_framePose[e];
            g_eyeFov[e] = G().s.fovMode == 1 ? g_frameFov[e] : g_gameFov;
            g_eyeTime[e] = g_frameTime;
        }
    } else {
        ok = SUCCEEDED(dev->StretchRect(src, nullptr, g_eyeSurf[g_eye], nullptr, D3DTEXF_NONE));
        if (ok) {
            g_eyeValid[g_eye] = true;
            g_eyePose[g_eye] = g_framePose[g_eye];
            g_eyeFov[g_eye] = G().s.fovMode == 1 ? g_frameFov[g_eye] : g_gameFov;
            g_eyeTime[g_eye] = g_frameTime;
        }
    }
    if (ok) {
        memcpy(g_sceneRot, g_camRot, sizeof(g_camRot));
        g_sceneFrame = G().frame;
        if (Native()) g_eyeFrame[0] = g_eyeFrame[1] = G().frame;
        else g_eyeFrame[g_eye] = G().frame;
        g_frameStereo = true;
        dev->ColorFill(g_hudSurf, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
        g_uiPhase = true;
    }
}

bool StereoFrameReady() { return g_frameStereo && g_eyeValid[0] && g_eyeValid[1]; }
IDirect3DSurface9* StereoEyeSurface(int eye) { return g_eyeSurf[eye & 1]; }
IDirect3DSurface9* StereoHudSurface() { return g_hudSurf; }
void StereoPoses(xrb::IpcPose pose[2], xrb::IpcFov fov[2], int64_t* time) {
    for (int e = 0; e < 2; ++e) { pose[e] = g_eyePose[e]; fov[e] = g_eyeFov[e]; }
    *time = g_eyeTime[0] > g_eyeTime[1] ? g_eyeTime[0] : g_eyeTime[1];
}

void StereoLogStatus() {
    if (!G().s.stereo) return;
    double n = g_statFrames ? g_statFrames : 1;
    LOGI("stereo (%s): %u of %u frames in stereo, per frame %.0f patched, %.0f right-eye draws, %.0f mirrored clears/copies, "
         "%.0f HUD draws; %u twins",
         Native() ? "native" : "aer", g_statStereo, g_statFrames, g_statPatched / n, g_statRight / n, g_statMirror / n, g_statHud / n, g_twins);
    g_statFrames = g_statStereo = g_statPatched = g_statHud = g_statRight = g_statMirror = 0;
    if (g_localSkipped) LOGI("stereo: %u depth-pass draws left unpatched (their FOV isn't the game camera's)", g_localSkipped);
    g_localSkipped = 0;
}

} // namespace fvr
