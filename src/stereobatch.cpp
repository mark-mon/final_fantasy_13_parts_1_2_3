// Native stereo, right eye in batches - see stereobatch.h.
#include "stereobatch.h"
#include "stereo.h"
#include "d3d9hooks.h"
#include "globals.h"
#include <vector>
#include <cstring>

namespace fvr {

bool BatchEnabled() {
    const Settings& s = G().s;
    return s.vrEnabled && s.stereo && s.vrMethod == VR_NATIVE && s.nativeBatch;
}

// ------------------------------------------------------------------ device-state shadow
struct BState {
    DWORD rs[256]; bool rsK[256];
    DWORD ss[20][14]; bool ssK[20][14];    // samplers 0..15 (pixel), 16..19 (vertex)
    DWORD tss[8][33]; bool tssK[8][33];
    IDirect3DBaseTexture9* tex[20];
    IDirect3DVertexShader9* vs;
    IDirect3DPixelShader9* ps;
    IDirect3DVertexDeclaration9* decl;
    DWORD fvf; bool useFvf;               // the later of SetFVF / SetVertexDeclaration wins
    IDirect3DVertexBuffer9* vb[16]; UINT vbOff[16], vbStride[16], freq[16]; bool freqK[16];
    IDirect3DIndexBuffer9* ib;
    float vsf[256][4]; int vsi[16][4]; BOOL vsb[16];
    float psf[224][4]; int psi[16][4]; BOOL psb[16];
    D3DVIEWPORT9 vp; bool vpK;
    RECT sc; bool scK;
    float clip[6][4]; bool clipK[6];
};
static BState g_cur{}, g_snap{};
static bool g_inScene = false;
void BsInScene(bool in) { g_inScene = in; }

static int SamplerIndex(DWORD s) {
    if (s < 16) return (int)s;
    if (s >= D3DVERTEXTEXTURESAMPLER0 && s <= D3DVERTEXTEXTURESAMPLER3) return 16 + (int)(s - D3DVERTEXTEXTURESAMPLER0);
    return -1;
}
static DWORD SamplerId(int i) { return i < 16 ? (DWORD)i : D3DVERTEXTEXTURESAMPLER0 + (i - 16); }

// ------------------------------------------------------------------ recording
enum OpType : uint8_t {
    OP_RS, OP_SS, OP_TSS, OP_TEX, OP_VS, OP_PS, OP_DECL, OP_FVF, OP_STREAM, OP_FREQ, OP_IDX,
    OP_VSF, OP_VSI, OP_VSB, OP_PSF, OP_PSI, OP_PSB, OP_VP, OP_SC, OP_CLIP, OP_CLEAR, OP_DRAW
};
struct Op {
    OpType t;
    DWORD a, b, c, d, e, f;
    void* p;
    uint32_t off, len;   // payload in g_data
};
static std::vector<Op> g_ops;
static std::vector<uint8_t> g_data;
static std::vector<IUnknown*> g_refs;   // resources the batch uses (kept alive until it is drawn)
static bool g_open = false;
static unsigned g_batchDraws = 0, g_batchClears = 0;   // a batch with only clears must be replayed too

// statistics (per 10 s)
static unsigned g_stFlushes = 0, g_stDraws = 0, g_stOps = 0, g_stUnknown = 0;
static LONGLONG g_stTicks = 0;

static void Ref(IUnknown* u) { if (u) { u->AddRef(); g_refs.push_back(u); } }
static uint32_t Data(const void* p, size_t n) {
    uint32_t off = (uint32_t)g_data.size();
    g_data.resize(g_data.size() + n);
    if (n) memcpy(&g_data[off], p, n);
    return off;
}
static void Push(OpType t, DWORD a = 0, DWORD b = 0, DWORD c = 0, void* p = nullptr, const void* data = nullptr, size_t len = 0) {
    Op o{t, a, b, c, 0, 0, 0, p, 0, (uint32_t)len};
    if (len) o.off = Data(data, len);
    g_ops.push_back(o);
}

bool BatchOpen() { return g_open; }

static void Open() {
    g_snap = g_cur;
    for (auto* t : g_snap.tex) Ref(t);
    for (auto* v : g_snap.vb) Ref(v);
    Ref(g_snap.ib); Ref(g_snap.vs); Ref(g_snap.ps); Ref(g_snap.decl);
    g_open = true;
}

// ------------------------------------------------------------------ shadow updates (+ recording while open)
void BsRenderState(D3DRENDERSTATETYPE s, DWORD v) {
    if ((unsigned)s >= 256 || (g_cur.rsK[s] && g_cur.rs[s] == v)) return;   // the game repeats states a lot
    g_cur.rs[s] = v; g_cur.rsK[s] = true;
    if (g_open) Push(OP_RS, s, v);
}
void BsSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE t, DWORD v) {
    int i = SamplerIndex(sampler);
    if (i < 0 || (unsigned)t >= 14 || (g_cur.ssK[i][t] && g_cur.ss[i][t] == v)) return;
    g_cur.ss[i][t] = v; g_cur.ssK[i][t] = true;
    if (g_open) Push(OP_SS, sampler, t, v);
}
void BsTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v) {
    if (stage >= 8 || (unsigned)t >= 33 || (g_cur.tssK[stage][t] && g_cur.tss[stage][t] == v)) return;
    g_cur.tss[stage][t] = v; g_cur.tssK[stage][t] = true;
    if (g_open) Push(OP_TSS, stage, t, v);
}
void BsTexture(DWORD stage, IDirect3DBaseTexture9* t) {
    int i = SamplerIndex(stage);
    if (i < 0 || g_cur.tex[i] == t) return;
    g_cur.tex[i] = t;
    if (g_open) { Ref(t); Push(OP_TEX, stage, 0, 0, t); }
}
void BsVertexShader(IDirect3DVertexShader9* s) { if (g_cur.vs == s) return; g_cur.vs = s; if (g_open) { Ref(s); Push(OP_VS, 0, 0, 0, s); } }
void BsPixelShader(IDirect3DPixelShader9* s) { if (g_cur.ps == s) return; g_cur.ps = s; if (g_open) { Ref(s); Push(OP_PS, 0, 0, 0, s); } }
void BsDecl(IDirect3DVertexDeclaration9* d) {
    if (g_cur.decl == d && !g_cur.useFvf) return;
    g_cur.decl = d; g_cur.useFvf = false;
    if (g_open) { Ref(d); Push(OP_DECL, 0, 0, 0, d); }
}
void BsFVF(DWORD fvf) { if (g_cur.useFvf && g_cur.fvf == fvf) return; g_cur.fvf = fvf; g_cur.useFvf = true; if (g_open) Push(OP_FVF, fvf); }
void BsStream(UINT n, IDirect3DVertexBuffer9* vb, UINT off, UINT stride) {
    if (n >= 16 || (g_cur.vb[n] == vb && g_cur.vbOff[n] == off && g_cur.vbStride[n] == stride)) return;
    g_cur.vb[n] = vb; g_cur.vbOff[n] = off; g_cur.vbStride[n] = stride;
    if (g_open) { Ref(vb); Push(OP_STREAM, n, off, stride, vb); }
}
void BsStreamFreq(UINT n, UINT div) {
    if (n >= 16 || (g_cur.freqK[n] && g_cur.freq[n] == div)) return;
    g_cur.freq[n] = div; g_cur.freqK[n] = true;
    if (g_open) Push(OP_FREQ, n, div);
}
void BsIndices(IDirect3DIndexBuffer9* ib) { if (g_cur.ib == ib) return; g_cur.ib = ib; if (g_open) { Ref(ib); Push(OP_IDX, 0, 0, 0, ib); } }

template <class T, int W>
static void ConstSet(T (*dst)[W], UINT max, UINT start, const T* d, UINT n, OpType op) {
    if (!d || start >= max) return;
    UINT m = start + n > max ? max - start : n;
    if (!memcmp(dst[start], d, m * sizeof(T) * W)) return;
    memcpy(dst[start], d, m * sizeof(T) * W);
    if (g_open) Push(op, start, m, 0, nullptr, d, m * sizeof(T) * W);
}
void BsVSConstF(UINT start, const float* d, UINT n) { ConstSet<float, 4>(g_cur.vsf, 256, start, d, n, OP_VSF); }
void BsVSConstI(UINT start, const int* d, UINT n) { ConstSet<int, 4>(g_cur.vsi, 16, start, d, n, OP_VSI); }
void BsPSConstF(UINT start, const float* d, UINT n) { ConstSet<float, 4>(g_cur.psf, 224, start, d, n, OP_PSF); }
void BsPSConstI(UINT start, const int* d, UINT n) { ConstSet<int, 4>(g_cur.psi, 16, start, d, n, OP_PSI); }
static void BoolSet(BOOL* dst, UINT start, const BOOL* d, UINT n, OpType op) {
    if (!d || start >= 16) return;
    UINT m = start + n > 16 ? 16 - start : n;
    if (!memcmp(dst + start, d, m * sizeof(BOOL))) return;
    memcpy(dst + start, d, m * sizeof(BOOL));
    if (g_open) Push(op, start, m, 0, nullptr, d, m * sizeof(BOOL));
}
void BsVSConstB(UINT start, const BOOL* d, UINT n) { BoolSet(g_cur.vsb, start, d, n, OP_VSB); }
void BsPSConstB(UINT start, const BOOL* d, UINT n) { BoolSet(g_cur.psb, start, d, n, OP_PSB); }
void BsViewport(const D3DVIEWPORT9& v) { if (g_cur.vpK && !memcmp(&g_cur.vp, &v, sizeof(v))) return; g_cur.vp = v; g_cur.vpK = true; if (g_open) Push(OP_VP, 0, 0, 0, nullptr, &v, sizeof(v)); }
void BsScissor(const RECT& r) { if (g_cur.scK && !memcmp(&g_cur.sc, &r, sizeof(r))) return; g_cur.sc = r; g_cur.scK = true; if (g_open) Push(OP_SC, 0, 0, 0, nullptr, &r, sizeof(r)); }
void BsClipPlane(DWORD i, const float* p) {
    if (i >= 6 || !p) return;
    memcpy(g_cur.clip[i], p, 16); g_cur.clipK[i] = true;
    if (g_open) Push(OP_CLIP, i, 0, 0, nullptr, p, 16);
}
void BsOnSetRT0(UINT w, UINT h) {   // recorded when the batch stays open (the same target set again)
    BsViewport({0, 0, w, h, 0.0f, 1.0f});
    BsScissor({0, 0, (LONG)w, (LONG)h});
}
void BsOnReset() {
    g_ops.clear(); g_data.clear();
    for (auto* u : g_refs) u->Release();
    g_refs.clear();
    g_open = false;
    g_batchDraws = g_batchClears = 0;
    memset(&g_cur, 0, sizeof(g_cur));
}

// ------------------------------------------------------------------ draws and clears
static UINT VertexCount(D3DPRIMITIVETYPE t, UINT pc) {
    switch (t) {
    case D3DPT_POINTLIST: return pc;
    case D3DPT_LINELIST: return pc * 2;
    case D3DPT_LINESTRIP: return pc + 1;
    case D3DPT_TRIANGLELIST: return pc * 3;
    default: return pc + 2;   // strips and fans
    }
}

void BatchRecordDraw(IDirect3DDevice9* dev, const BatchDraw& d) {
    if (!g_open) Open();
    StereoPatchReg patch[kStereoMaxPatches];
    int np = StereoRightPatches(patch);
    Op o{OP_DRAW, (DWORD)d.kind, (DWORD)d.type, d.a, d.b, d.c, d.d, nullptr, 0, 0};
    o.off = (uint32_t)g_data.size();
    uint32_t n32 = (uint32_t)np;
    Data(&n32, 4);
    for (int i = 0; i < np; ++i) Data(&patch[i], sizeof(StereoPatchReg));
    // fields: c..f = d.a..d.d; DIP: bv in p; DIPUP: index format in p
    if (d.kind == 1) o.p = (void*)(intptr_t)d.bv;
    if (d.kind == 2) {   // DPUP: a = pc, b = stride
        uint32_t len = VertexCount(d.type, d.a) * d.b;
        Data(&len, 4);
        Data(d.verts, len);
    }
    if (d.kind == 3) {   // DIPUP: a = mi, b = nv, c = pc, d = stride
        uint32_t ilen = VertexCount(d.type, d.c) * (d.idxFmt == D3DFMT_INDEX32 ? 4 : 2);
        uint32_t vlen = (d.a + d.b) * d.d;
        o.p = (void*)(intptr_t)d.idxFmt;
        Data(&ilen, 4); Data(d.idx, ilen);
        Data(&vlen, 4); Data(d.verts, vlen);
    }
    o.len = (uint32_t)g_data.size() - o.off;
    g_ops.push_back(o);
    g_batchDraws++;
    if (g_ops.size() > 60000 || g_data.size() > (48u << 20)) BatchFlush(dev, BF_FULL);   // keep memory bounded
}

bool BatchRecordClear(IDirect3DDevice9*, DWORD n, const D3DRECT* r, DWORD fl, D3DCOLOR c, float z, DWORD s) {
    if (!g_open) Open();
    Op o{OP_CLEAR, n, fl, c, 0, s, 0, nullptr, 0, 0};
    memcpy(&o.d, &z, 4);
    g_batchClears++;
    if (n && r) { o.off = Data(r, n * sizeof(D3DRECT)); o.len = n * sizeof(D3DRECT); }
    else o.a = 0;
    g_ops.push_back(o);
    return true;
}

// ------------------------------------------------------------------ replay
static IDirect3DBaseTexture9* RightTex(IDirect3DBaseTexture9* t) {
    IDirect3DBaseTexture9* w = StereoTwinTexture(t);
    return w ? w : t;
}

// Puts the device into state `s` (the pass start), assuming it is in state `c` (the pass end).
static void ApplyState(IDirect3DDevice9* dev, const BState& s, const BState& c) {
    for (int i = 0; i < 256; ++i)
        if (s.rsK[i] && s.rs[i] != c.rs[i]) dev->SetRenderState((D3DRENDERSTATETYPE)i, s.rs[i]);
        else if (!s.rsK[i] && c.rsK[i]) g_stUnknown++;
    for (int i = 0; i < 20; ++i)
        for (int t = 1; t < 14; ++t)
            if (s.ssK[i][t] && s.ss[i][t] != c.ss[i][t]) dev->SetSamplerState(SamplerId(i), (D3DSAMPLERSTATETYPE)t, s.ss[i][t]);
            else if (!s.ssK[i][t] && c.ssK[i][t]) g_stUnknown++;
    for (int i = 0; i < 8; ++i)
        for (int t = 1; t < 33; ++t)
            if (s.tssK[i][t] && s.tss[i][t] != c.tss[i][t]) dev->SetTextureStageState(i, (D3DTEXTURESTAGESTATETYPE)t, s.tss[i][t]);
            else if (!s.tssK[i][t] && c.tssK[i][t]) g_stUnknown++;
    for (int i = 0; i < 20; ++i) {
        IDirect3DBaseTexture9* w = StereoTwinTexture(s.tex[i]);
        if (s.tex[i] != c.tex[i] || w) dev->SetTexture(SamplerId(i), w ? w : s.tex[i]);
    }
    if (s.vs != c.vs) dev->SetVertexShader(s.vs);
    if (s.ps != c.ps) dev->SetPixelShader(s.ps);
    if (s.useFvf != c.useFvf || s.decl != c.decl || s.fvf != c.fvf) {
        if (s.useFvf) dev->SetFVF(s.fvf); else dev->SetVertexDeclaration(s.decl);
    }
    for (int i = 0; i < 16; ++i) {
        if (s.vb[i] != c.vb[i] || s.vbOff[i] != c.vbOff[i] || s.vbStride[i] != c.vbStride[i]) dev->SetStreamSource(i, s.vb[i], s.vbOff[i], s.vbStride[i]);
        if (s.freq[i] != c.freq[i]) dev->SetStreamSourceFreq(i, s.freqK[i] ? s.freq[i] : 1);
    }
    if (s.ib != c.ib) dev->SetIndices(s.ib);
    for (UINT i = 0; i < 256; ++i) if (memcmp(s.vsf[i], c.vsf[i], 16)) dev->SetVertexShaderConstantF(i, s.vsf[i], 1);
    for (UINT i = 0; i < 224; ++i) if (memcmp(s.psf[i], c.psf[i], 16)) dev->SetPixelShaderConstantF(i, s.psf[i], 1);
    for (UINT i = 0; i < 16; ++i) {
        if (memcmp(s.vsi[i], c.vsi[i], 16)) dev->SetVertexShaderConstantI(i, s.vsi[i], 1);
        if (memcmp(s.psi[i], c.psi[i], 16)) dev->SetPixelShaderConstantI(i, s.psi[i], 1);
        if (s.vsb[i] != c.vsb[i]) dev->SetVertexShaderConstantB(i, &s.vsb[i], 1);
        if (s.psb[i] != c.psb[i]) dev->SetPixelShaderConstantB(i, &s.psb[i], 1);
    }
    for (DWORD i = 0; i < 6; ++i) if (s.clipK[i] && memcmp(s.clip[i], c.clip[i], 16)) dev->SetClipPlane(i, s.clip[i]);
    if (s.vpK) dev->SetViewport(&s.vp);   // always: binding the targets reset them
    if (s.scK) dev->SetScissorRect(&s.sc);
}

static void ReplayDraw(IDirect3DDevice9* dev, const Op& o) {
    const uint8_t* p = &g_data[o.off];
    uint32_t np;
    memcpy(&np, p, 4); p += 4;
    const StereoPatchReg* patch = (const StereoPatchReg*)p;
    p += np * sizeof(StereoPatchReg);
    for (uint32_t i = 0; i < np; ++i)
        if (patch[i].ps) dev->SetPixelShaderConstantF(patch[i].reg, &patch[i].right[0][0], patch[i].count);
        else dev->SetVertexShaderConstantF(patch[i].reg, &patch[i].right[0][0], patch[i].count);
    D3DPRIMITIVETYPE t = (D3DPRIMITIVETYPE)o.b;
    switch (o.a) {
    case 0: dev->DrawPrimitive(t, o.c, o.d); break;
    case 1: dev->DrawIndexedPrimitive(t, (INT)(intptr_t)o.p, o.c, o.d, o.e, o.f); break;   // bv, mi, nv, si, pc
    case 2: {
        p += 4;   // vertex data length
        dev->DrawPrimitiveUP(t, o.c, p, o.d);
        break;
    }
    case 3: {
        uint32_t il; memcpy(&il, p, 4); p += 4;
        const uint8_t* idx = p; p += il;
        uint32_t vl; memcpy(&vl, p, 4); p += 4;
        // o.c = mi, o.d = nv, o.e = pc, o.f = stride, p = index format
        dev->DrawIndexedPrimitiveUP(t, o.c, o.d, o.e, idx, (D3DFORMAT)(intptr_t)o.p, p, o.f);
        break;
    }
    }
    for (uint32_t i = 0; i < np; ++i)
        if (patch[i].ps) dev->SetPixelShaderConstantF(patch[i].reg, &patch[i].game[0][0], patch[i].count);
        else dev->SetVertexShaderConstantF(patch[i].reg, &patch[i].game[0][0], patch[i].count);
}

static unsigned g_stReason[BF_COUNT] = {};
void BatchFlush(IDirect3DDevice9* dev, int reason) {
    if (!g_open) return;
    if (reason >= 0 && reason < BF_COUNT) g_stReason[reason]++;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    if (g_batchDraws || g_batchClears) {
        InternalScope in;
        bool ownScene = !g_inScene;   // the game ends a scene per pass; the batch outlives that
        if (ownScene) dev->BeginScene();
        StereoBindPassTargets(dev, true);
        ApplyState(dev, g_snap, g_cur);
        for (const Op& o : g_ops) {
            const void* d = o.len ? &g_data[o.off] : nullptr;
            switch (o.t) {
            case OP_RS: dev->SetRenderState((D3DRENDERSTATETYPE)o.a, o.b); break;
            case OP_SS: dev->SetSamplerState(o.a, (D3DSAMPLERSTATETYPE)o.b, o.c); break;
            case OP_TSS: dev->SetTextureStageState(o.a, (D3DTEXTURESTAGESTATETYPE)o.b, o.c); break;
            case OP_TEX: dev->SetTexture(o.a, RightTex((IDirect3DBaseTexture9*)o.p)); break;
            case OP_VS: dev->SetVertexShader((IDirect3DVertexShader9*)o.p); break;
            case OP_PS: dev->SetPixelShader((IDirect3DPixelShader9*)o.p); break;
            case OP_DECL: dev->SetVertexDeclaration((IDirect3DVertexDeclaration9*)o.p); break;
            case OP_FVF: dev->SetFVF(o.a); break;
            case OP_STREAM: dev->SetStreamSource(o.a, (IDirect3DVertexBuffer9*)o.p, o.b, o.c); break;
            case OP_FREQ: dev->SetStreamSourceFreq(o.a, o.b); break;
            case OP_IDX: dev->SetIndices((IDirect3DIndexBuffer9*)o.p); break;
            case OP_VSF: dev->SetVertexShaderConstantF(o.a, (const float*)d, o.b); break;
            case OP_VSI: dev->SetVertexShaderConstantI(o.a, (const int*)d, o.b); break;
            case OP_VSB: dev->SetVertexShaderConstantB(o.a, (const BOOL*)d, o.b); break;
            case OP_PSF: dev->SetPixelShaderConstantF(o.a, (const float*)d, o.b); break;
            case OP_PSI: dev->SetPixelShaderConstantI(o.a, (const int*)d, o.b); break;
            case OP_PSB: dev->SetPixelShaderConstantB(o.a, (const BOOL*)d, o.b); break;
            case OP_VP: dev->SetViewport((const D3DVIEWPORT9*)d); break;
            case OP_SC: dev->SetScissorRect((const RECT*)d); break;
            case OP_CLIP: dev->SetClipPlane(o.a, (const float*)d); break;
            case OP_CLEAR: { float z; memcpy(&z, &o.d, 4); dev->Clear(o.a, (const D3DRECT*)d, o.b, o.c, z, o.e); break; }
            case OP_DRAW: ReplayDraw(dev, o); break;
            }
        }
        for (int i = 0; i < 20; ++i)   // the game's (left) textures back where a twin was used
            if (StereoTwinTexture(g_cur.tex[i])) dev->SetTexture(SamplerId(i), g_cur.tex[i]);
        StereoBindPassTargets(dev, false);
        if (g_cur.vpK) dev->SetViewport(&g_cur.vp);
        if (g_cur.scK) dev->SetScissorRect(&g_cur.sc);
        StereoNoteRightDraws(g_batchDraws);
        if (ownScene) dev->EndScene();
    }
    g_stFlushes++;
    g_stDraws += g_batchDraws;
    g_stOps += (unsigned)g_ops.size();
    g_ops.clear();
    g_data.clear();
    for (auto* u : g_refs) u->Release();
    g_refs.clear();
    g_open = false;
    g_batchDraws = g_batchClears = 0;
    QueryPerformanceCounter(&t1);
    g_stTicks += t1.QuadPart - t0.QuadPart;
}

// ------------------------------------------------------------------ restore after an overlay (in-game menu)
// Puts the game's state back after our own drawing: the shadow for everything the game ever set; D3D defaults for
// the few fixed-function states the menu uses that the game may never have touched.
void BsRestoreDevice(IDirect3DDevice9* dev) {
    if (IDirect3DSurface9* rt = StereoBoundRT(0)) dev->SetRenderTarget(0, rt);
    dev->SetDepthStencilSurface(StereoBoundDS());
    const BState& c = g_cur;
    static const struct { D3DRENDERSTATETYPE s; DWORD def; } kRs[] = {
        {D3DRS_FILLMODE, D3DFILL_SOLID}, {D3DRS_SHADEMODE, D3DSHADE_GOURAUD}, {D3DRS_CULLMODE, D3DCULL_CCW}, {D3DRS_LIGHTING, TRUE},
        {D3DRS_ZENABLE, D3DZB_TRUE}, {D3DRS_ZWRITEENABLE, TRUE}, {D3DRS_STENCILENABLE, FALSE}, {D3DRS_ALPHATESTENABLE, FALSE},
        {D3DRS_FOGENABLE, FALSE}, {D3DRS_CLIPPLANEENABLE, 0}, {D3DRS_COLORWRITEENABLE, 0xF}, {D3DRS_SRGBWRITEENABLE, FALSE},
        {D3DRS_ALPHABLENDENABLE, FALSE}, {D3DRS_BLENDOP, D3DBLENDOP_ADD}, {D3DRS_SRCBLEND, D3DBLEND_ONE}, {D3DRS_DESTBLEND, D3DBLEND_ZERO},
        {D3DRS_SEPARATEALPHABLENDENABLE, FALSE}, {D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD}, {D3DRS_SRCBLENDALPHA, D3DBLEND_ONE},
        {D3DRS_DESTBLENDALPHA, D3DBLEND_ZERO}, {D3DRS_SCISSORTESTENABLE, FALSE}};
    for (auto& r : kRs) dev->SetRenderState(r.s, c.rsK[r.s] ? c.rs[r.s] : r.def);
    static const struct { DWORD st; D3DTEXTURESTAGESTATETYPE t; DWORD def; } kTss[] = {
        {0, D3DTSS_COLOROP, D3DTOP_MODULATE}, {0, D3DTSS_COLORARG1, D3DTA_TEXTURE}, {0, D3DTSS_COLORARG2, D3DTA_CURRENT},
        {0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1}, {0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE}, {0, D3DTSS_ALPHAARG2, D3DTA_CURRENT},
        {0, D3DTSS_TEXCOORDINDEX, 0}, {0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE}, {1, D3DTSS_COLOROP, D3DTOP_DISABLE},
        {1, D3DTSS_ALPHAOP, D3DTOP_DISABLE}};
    for (auto& t : kTss) dev->SetTextureStageState(t.st, t.t, c.tssK[t.st][t.t] ? c.tss[t.st][t.t] : t.def);
    static const struct { D3DSAMPLERSTATETYPE t; DWORD def; } kSs[] = {
        {D3DSAMP_MINFILTER, D3DTEXF_POINT}, {D3DSAMP_MAGFILTER, D3DTEXF_POINT}, {D3DSAMP_MIPFILTER, D3DTEXF_NONE},
        {D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP}, {D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP}, {D3DSAMP_SRGBTEXTURE, FALSE}};
    for (auto& s : kSs) dev->SetSamplerState(0, s.t, c.ssK[0][s.t] ? c.ss[0][s.t] : s.def);
    dev->SetTexture(0, c.tex[0]);
    dev->SetVertexShader(c.vs);
    dev->SetPixelShader(c.ps);
    if (c.useFvf) dev->SetFVF(c.fvf); else dev->SetVertexDeclaration(c.decl);
    dev->SetStreamSource(0, c.vb[0], c.vbOff[0], c.vbStride[0]);   // the UP draws unbound stream 0 and the indices
    dev->SetIndices(c.ib);
    if (c.vpK) dev->SetViewport(&c.vp);
    if (c.scK) dev->SetScissorRect(&c.sc);
}

void BatchLogStatus() {
    if (!g_stFlushes) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    LOGI("native batch: %u batches, %u right-eye draws, %u recorded calls, replay %.1f ms total per 10 s%s; ended by: target %u, depth %u, "
         "copy %u, lock %u, end of scene %u, present %u, update %u, full %u", g_stFlushes, g_stDraws, g_stOps, g_stTicks * 1000.0 / f.QuadPart,
         g_stUnknown ? " (some pass-start states were never set by the game)" : "", g_stReason[BF_TARGET], g_stReason[BF_DEPTH],
         g_stReason[BF_COPY], g_stReason[BF_LOCK], g_stReason[BF_ENDSCENE], g_stReason[BF_PRESENT], g_stReason[BF_UPDATE], g_stReason[BF_FULL]);
    memset(g_stReason, 0, sizeof(g_stReason));
    g_stFlushes = g_stDraws = g_stOps = g_stUnknown = 0;
    g_stTicks = 0;
}

} // namespace fvr
