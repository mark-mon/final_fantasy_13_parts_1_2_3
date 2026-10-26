// One-frame draw capture: writes FF13VR\capture_<id>\frame.txt (every state change and draw of one frame, with the
// shader constants that changed before each draw), an image of each render target whenever the game switches away
// from it ("pass_NNN_<name>.png"), the final backbuffer, and every shader used (bytecode + disassembly).
#include "capture.h"
#include "d3d9hooks.h"
#include "imageio.h"
#include "globals.h"
#include <d3dcommon.h>
#include <unordered_map>
#include <map>
#include <set>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdarg>

namespace fvr {

Shadow& Sh() { static Shadow s; return s; }

// ------------------------------------------------------------------ shader registry
struct ShaderInfo { bool ps = false; uint32_t hash = 0; std::vector<DWORD> code; };
static std::unordered_map<void*, ShaderInfo> g_shaders;
static SRWLOCK g_shLock = SRWLOCK_INIT;

static uint32_t Fnv(const void* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) { h ^= ((const uint8_t*)p)[i]; h *= 16777619u; }
    return h;
}

static size_t ShaderTokens(const DWORD* code) {
    // SM2/3 token stream: instruction lengths in bits 24-27, comments carry their length in bits 16-30
    if (!code) return 0;
    DWORD major = (code[0] >> 8) & 0xFF;
    size_t i = 1;
    while (i < 65536) {
        DWORD t = code[i];
        if (t == 0x0000FFFF) return i + 1;
        if ((t & 0xFFFF) == 0xFFFE) i += 1 + ((t >> 16) & 0x7FFF);
        else i += 1 + (major >= 2 ? ((t >> 24) & 0xF) : 0);
    }
    return 0;
}

void RegisterShader(void* shader, bool pixel, const DWORD* bytecode) {
    size_t n = ShaderTokens(bytecode);
    if (!shader || !n) return;
    ShaderInfo s;
    s.ps = pixel;
    s.code.assign(bytecode, bytecode + n);
    s.hash = Fnv(bytecode, n * 4);
    AcquireSRWLockExclusive(&g_shLock);
    g_shaders[shader] = std::move(s);
    ReleaseSRWLockExclusive(&g_shLock);
}

static bool FindShader(void* p, ShaderInfo* out) {
    AcquireSRWLockShared(&g_shLock);
    auto it = g_shaders.find(p);
    bool ok = it != g_shaders.end();
    if (ok) *out = it->second;
    ReleaseSRWLockShared(&g_shLock);
    return ok;
}

// ------------------------------------------------------------------ capture state
struct Cap {
    bool active = false;
    unsigned id = 0;
    std::wstring dir;
    std::string body;
    unsigned draws = 0, clears = 0, stretches = 0, snapshots = 0, events = 0;
    std::map<void*, std::string> names;          // resource -> short name
    std::map<std::string, std::string> descs;    // short name -> description
    unsigned nextSurf = 0, nextTex = 0, nextDecl = 0;
    IDirect3DSurface9* rt[4] = {};
    bool rtDirty[4] = {};
    IDirect3DSurface9* ds = nullptr;
    void* vs = nullptr; void* ps = nullptr;
    IDirect3DBaseTexture9* tex[20] = {};         // 0..15 pixel samplers, 16..19 vertex samplers (D3DVERTEXTEXTURESAMPLER0..3)
    IDirect3DVertexDeclaration9* decl = nullptr;
    UINT stride0 = 0;
    bool vsDirty[256] = {}, psDirty[224] = {};
    std::set<void*> usedShaders;
    std::map<std::string, unsigned> drawsPerRt;
    bool truncated = false;
};
static Cap g;
static const unsigned kMaxSnapshots = 120;
static const size_t kMaxBody = 48u << 20;

bool Capturing() { return g.active; }

static void Out(const char* fmt, ...) {
    if (g.body.size() > kMaxBody) { g.truncated = true; return; }
    char buf[2048];
    va_list a; va_start(a, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    if (n > 0) g.body.append(buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
    g.body += '\n';
}

static std::string SurfName(IDirect3DSurface9* s);
static std::string TexName(IDirect3DBaseTexture9* t) {
    if (!t) return "-";
    auto it = g.names.find(t);
    if (it != g.names.end()) return it->second;
    char nm[16]; sprintf_s(nm, "T%u", g.nextTex++);
    g.names[t] = nm;
    char d[256] = "";
    D3DRESOURCETYPE ty = t->GetType();
    if (ty == D3DRTYPE_TEXTURE) {
        D3DSURFACE_DESC sd;
        IDirect3DTexture9* tt = (IDirect3DTexture9*)t;
        if (SUCCEEDED(tt->GetLevelDesc(0, &sd))) {
            sprintf_s(d, "2D %ux%u %s levels %lu usage %lx pool %d", sd.Width, sd.Height, FormatName(sd.Format), t->GetLevelCount(), sd.Usage, (int)sd.Pool);
            IDirect3DSurface9* l0 = nullptr;
            if (SUCCEEDED(tt->GetSurfaceLevel(0, &l0)) && l0) {
                if (sd.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) {
                    std::string sn = SurfName(l0);
                    strcat_s(d, (" = surface " + sn).c_str());
                }
                l0->Release();
            }
        }
    } else if (ty == D3DRTYPE_CUBETEXTURE) {
        D3DSURFACE_DESC sd;
        if (SUCCEEDED(((IDirect3DCubeTexture9*)t)->GetLevelDesc(0, &sd)))
            sprintf_s(d, "CUBE %u %s levels %lu usage %lx pool %d", sd.Width, FormatName(sd.Format), t->GetLevelCount(), sd.Usage, (int)sd.Pool);
    } else if (ty == D3DRTYPE_VOLUMETEXTURE) {
        D3DVOLUME_DESC vd;
        if (SUCCEEDED(((IDirect3DVolumeTexture9*)t)->GetLevelDesc(0, &vd)))
            sprintf_s(d, "3D %ux%ux%u %s levels %lu pool %d", vd.Width, vd.Height, vd.Depth, FormatName(vd.Format), t->GetLevelCount(), (int)vd.Pool);
    }
    g.descs[nm] = d;
    return nm;
}

static std::string SurfName(IDirect3DSurface9* s) {
    if (!s) return "-";
    auto it = g.names.find(s);
    if (it != g.names.end()) return it->second;
    char nm[16]; sprintf_s(nm, "S%u", g.nextSurf++);
    g.names[s] = nm;
    char d[256] = "";
    D3DSURFACE_DESC sd;
    if (SUCCEEDED(s->GetDesc(&sd))) {
        sprintf_s(d, "%ux%u %s usage %lx pool %d msaa %d", sd.Width, sd.Height, FormatName(sd.Format), sd.Usage, (int)sd.Pool, (int)sd.MultiSampleType);
        IDirect3DTexture9* cont = nullptr;
        if (SUCCEEDED(s->GetContainer(IID_IDirect3DTexture9, (void**)&cont)) && cont) {
            g.descs[nm] = d;   // set before TexName (which may refer back to this surface)
            std::string tn = TexName(cont);
            strcat_s(d, (" (level of " + tn + ")").c_str());
            cont->Release();
        }
    }
    g.descs[nm] = d;
    return nm;
}

static std::string ShName(void* p) {
    if (!p) return "-";
    ShaderInfo si;
    char nm[32];
    if (FindShader(p, &si)) sprintf_s(nm, "%s_%08x", si.ps ? "ps" : "vs", si.hash);
    else sprintf_s(nm, "shader@%p", p);
    g.usedShaders.insert(p);
    return nm;
}

static std::string DeclName(IDirect3DVertexDeclaration9* d) {
    if (!d) return "-";
    auto it = g.names.find(d);
    if (it != g.names.end()) return it->second;
    char nm[16]; sprintf_s(nm, "D%u", g.nextDecl++);
    g.names[d] = nm;
    static const char* types[] = {"f1", "f2", "f3", "f4", "color", "ubyte4", "short2", "short4", "ubyte4n", "short2n", "short4n",
                                  "ushort2n", "ushort4n", "udec3", "dec3n", "half2", "half4", "unused"};
    static const char* usages[] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
                                   "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"};
    UINT n = 0;
    std::string s;
    if (SUCCEEDED(d->GetDeclaration(nullptr, &n)) && n) {
        std::vector<D3DVERTEXELEMENT9> el(n);
        if (SUCCEEDED(d->GetDeclaration(el.data(), &n)))
            for (auto& e : el) {
                if (e.Stream == 0xFF) break;
                char b[96];
                sprintf_s(b, "s%u+%u %s%u %s; ", e.Stream, e.Offset, e.Usage < 14 ? usages[e.Usage] : "?", e.UsageIndex, e.Type < 18 ? types[e.Type] : "?");
                s += b;
            }
    }
    g.descs[nm] = s;
    return nm;
}

static void Snapshot(IDirect3DDevice9* dev, int idx) {
    IDirect3DSurface9* s = g.rt[idx];
    if (!s || !g.rtDirty[idx]) return;
    g.rtDirty[idx] = false;
    if (g.snapshots >= kMaxSnapshots) return;
    std::string nm = SurfName(s);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%spass_%03u_rt%d_%S", g.dir.c_str(), g.snapshots++, idx, nm.c_str());
    std::string note;
    bool ok = SaveSurfaceImage(dev, s, path, &note);
    Out("    -- snapshot %S.png of RT%d %s: %s%s", wcsrchr(path, L'\\') + 1, idx, nm.c_str(), note.c_str(), ok ? "" : " (NOT SAVED)");
}

static void DumpConstants(bool pixel, bool all) {
    int n = pixel ? 224 : 256;
    bool* dirty = pixel ? g.psDirty : g.vsDirty;
    const float(*c)[4] = pixel ? Sh().psc : Sh().vsc;
    for (int i = 0; i < n; ++i) {
        if (all ? (c[i][0] == 0 && c[i][1] == 0 && c[i][2] == 0 && c[i][3] == 0) : !dirty[i]) continue;
        Out("      %s c%-3d = %12.6g %12.6g %12.6g %12.6g", pixel ? "ps" : "vs", i, c[i][0], c[i][1], c[i][2], c[i][3]);
        dirty[i] = false;
    }
}

void CaptureBegin(IDirect3DDevice9* dev, unsigned id) {
    g = Cap();
    g.active = true;
    g.id = id;
    wchar_t d[MAX_PATH];
    swprintf_s(d, L"%scapture_%u\\", G().runDir.c_str(), id);
    g.dir = d;
    CreateDirectoryW(g.dir.c_str(), nullptr);
    CreateDirectoryW((g.dir + L"shaders").c_str(), nullptr);
    LOGI("capture %u: recording frame %ld into %S", id, G().frame, g.dir.c_str());
    // starting state
    Out("==== starting state");
    for (DWORD i = 0; i < 4; ++i) {
        IDirect3DSurface9* s = nullptr;
        if (SUCCEEDED(dev->GetRenderTarget(i, &s)) && s) { g.rt[i] = s; Out("RT%lu = %s", i, SurfName(s).c_str()); s->Release(); }
    }
    IDirect3DSurface9* ds = nullptr;
    if (SUCCEEDED(dev->GetDepthStencilSurface(&ds)) && ds) { g.ds = ds; Out("DS = %s", SurfName(ds).c_str()); ds->Release(); }
    IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr;
    if (SUCCEEDED(dev->GetVertexShader(&vs)) && vs) { g.vs = vs; vs->Release(); }
    if (SUCCEEDED(dev->GetPixelShader(&ps)) && ps) { g.ps = ps; ps->Release(); }
    Out("VS = %s  PS = %s", ShName(g.vs).c_str(), ShName(g.ps).c_str());
    for (DWORD i = 0; i < 20; ++i) {
        IDirect3DBaseTexture9* t = nullptr;
        DWORD st = i < 16 ? i : D3DVERTEXTEXTURESAMPLER0 + (i - 16);
        if (SUCCEEDED(dev->GetTexture(st, &t)) && t) { g.tex[i] = t; Out("tex[%lu] = %s", st, TexName(t).c_str()); t->Release(); }
    }
    D3DVIEWPORT9 vp;
    if (SUCCEEDED(dev->GetViewport(&vp))) Out("viewport %lu,%lu %lux%lu z %.2f..%.2f", vp.X, vp.Y, vp.Width, vp.Height, vp.MinZ, vp.MaxZ);
    Out("vertex shader constants (non-zero):");
    DumpConstants(false, true);
    Out("pixel shader constants (non-zero):");
    DumpConstants(true, true);
    Out("==== frame");
}

void CapSetRenderTarget(IDirect3DDevice9* dev, DWORD idx, IDirect3DSurface9* s) {
    if (idx >= 4) return;
    if (g.rt[idx] != s) Snapshot(dev, (int)idx);
    g.rt[idx] = s;
    g.events++;
    Out("SetRenderTarget(%lu, %s)", idx, SurfName(s).c_str());
}
void CapSetDepthStencil(IDirect3DSurface9* s) { g.ds = s; Out("SetDepthStencil(%s)", SurfName(s).c_str()); }
void CapViewport(const D3DVIEWPORT9* vp) {
    if (vp) Out("SetViewport(%lu,%lu %lux%lu z %.2f..%.2f)", vp->X, vp->Y, vp->Width, vp->Height, vp->MinZ, vp->MaxZ);
}
void CapScissor(const RECT* r) { if (r) Out("SetScissorRect(%ld,%ld - %ld,%ld)", r->left, r->top, r->right, r->bottom); }
void CapShader(bool pixel, void* sh) {
    if (pixel) g.ps = sh; else g.vs = sh;
    Out("Set%sShader(%s)", pixel ? "Pixel" : "Vertex", ShName(sh).c_str());
}
void CapConstF(bool pixel, UINT start, const float*, UINT count) {
    bool* dirty = pixel ? g.psDirty : g.vsDirty;
    UINT n = pixel ? 224 : 256;
    for (UINT i = start; i < start + count && i < n; ++i) dirty[i] = true;
}
void CapTexture(DWORD stage, IDirect3DBaseTexture9* t) {
    int i = stage < 16 ? (int)stage : (stage >= D3DVERTEXTEXTURESAMPLER0 && stage <= D3DVERTEXTEXTURESAMPLER3) ? 16 + (int)(stage - D3DVERTEXTEXTURESAMPLER0) : -1;
    if (i < 0) return;
    if (g.tex[i] == t) return;
    g.tex[i] = t;
    Out("SetTexture(%lu, %s)", stage, TexName(t).c_str());
}

static const char* RsName(D3DRENDERSTATETYPE s) {
    switch (s) {
#define R(x) case D3DRS_##x: return #x;
    R(ZENABLE) R(FILLMODE) R(ZWRITEENABLE) R(ALPHATESTENABLE) R(SRCBLEND) R(DESTBLEND) R(CULLMODE) R(ZFUNC) R(ALPHAREF)
    R(ALPHAFUNC) R(ALPHABLENDENABLE) R(FOGENABLE) R(STENCILENABLE) R(STENCILFUNC) R(STENCILREF) R(STENCILPASS)
    R(COLORWRITEENABLE) R(BLENDOP) R(SCISSORTESTENABLE) R(SLOPESCALEDEPTHBIAS) R(DEPTHBIAS) R(SRGBWRITEENABLE)
    R(SEPARATEALPHABLENDENABLE) R(SRCBLENDALPHA) R(DESTBLENDALPHA) R(BLENDOPALPHA) R(TWOSIDEDSTENCILMODE)
    R(COLORWRITEENABLE1) R(COLORWRITEENABLE2) R(COLORWRITEENABLE3) R(BLENDFACTOR) R(CLIPPLANEENABLE) R(POINTSIZE)
#undef R
    default: return nullptr;
    }
}
void CapRenderState(D3DRENDERSTATETYPE s, DWORD v, DWORD prev, bool prevKnown) {
    if (prevKnown && prev == v) return;
    const char* n = RsName(s);
    if (n) Out("SetRenderState(%s, %lu / 0x%lx)", n, v, v);
    else Out("SetRenderState(%d, %lu / 0x%lx)", (int)s, v, v);
}
void CapStream(UINT n, IDirect3DVertexBuffer9* vb, UINT off, UINT stride) {
    if (n == 0) g.stride0 = stride;
    Out("SetStreamSource(%u, vb@%p, off %u, stride %u)", n, vb, off, stride);
}
void CapDecl(IDirect3DVertexDeclaration9* d) {
    if (g.decl == d) return;
    g.decl = d;
    Out("SetVertexDeclaration(%s)", DeclName(d).c_str());
}
void CapClear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR c, float z, DWORD s) {
    g.clears++;
    for (int i = 0; i < 4; ++i) if (g.rt[i] && (flags & D3DCLEAR_TARGET)) g.rtDirty[i] = true;
    Out("Clear(%lu rects, flags %lx%s%s%s, color %08lx, z %.3f, stencil %lu) RT0 %s DS %s", count, flags,
        flags & D3DCLEAR_TARGET ? " T" : "", flags & D3DCLEAR_ZBUFFER ? " Z" : "", flags & D3DCLEAR_STENCIL ? " S" : "", c, z, s,
        SurfName(g.rt[0]).c_str(), SurfName(g.ds).c_str());
    (void)rects;
}
void CapStretch(IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const RECT* dr, D3DTEXTUREFILTERTYPE f) {
    g.stretches++;
    char a[64] = "full", b[64] = "full";
    if (sr) sprintf_s(a, "%ld,%ld-%ld,%ld", sr->left, sr->top, sr->right, sr->bottom);
    if (dr) sprintf_s(b, "%ld,%ld-%ld,%ld", dr->left, dr->top, dr->right, dr->bottom);
    Out("StretchRect(%s [%s] -> %s [%s], filter %d)", SurfName(src).c_str(), a, SurfName(dst).c_str(), b, (int)f);
}

void CapDraw(const char* kind, D3DPRIMITIVETYPE type, UINT a, UINT b, UINT c, UINT d, UINT prims) {
    static const char* pt[] = {"?", "points", "lines", "linestrip", "tris", "tristrip", "trifan"};
    unsigned n = g.draws++;
    for (int i = 0; i < 4; ++i) if (g.rt[i]) g.rtDirty[i] = true;
    std::string rt0 = SurfName(g.rt[0]);
    g.drawsPerRt[rt0]++;
    std::string texs;
    for (int i = 0; i < 20; ++i)
        if (g.tex[i]) { char t[40]; sprintf_s(t, " %s%d=%s", i < 16 ? "s" : "v", i < 16 ? i : i - 16, TexName(g.tex[i]).c_str()); texs += t; }
    std::string rts = rt0;
    for (int i = 1; i < 4; ++i) if (g.rt[i]) rts += "," + SurfName(g.rt[i]);
    Out("#%u %s %s n=%u (%u %u %u %u) | RT %s DS %s | %s %s | decl %s stride %u |%s", n, kind, (unsigned)type < 7 ? pt[type] : "?", prims,
        a, b, c, d, rts.c_str(), SurfName(g.ds).c_str(), ShName(g.vs).c_str(), ShName(g.ps).c_str(), DeclName(g.decl).c_str(), g.stride0,
        texs.c_str());
    DumpConstants(false, false);
    DumpConstants(true, false);
}

typedef HRESULT(WINAPI* D3DDisassemble_t)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);

void CaptureEnd(IDirect3DDevice9* dev) {
    if (!g.active) return;
    for (int i = 0; i < 4; ++i) Snapshot(dev, i);
    IDirect3DSurface9* bb = nullptr;
    std::string bbNote = "n/a";
    if ((bb = GameBackBuffer(dev)) != nullptr) {
        SaveSurfaceImage(dev, bb, g.dir + L"backbuffer", &bbNote);
        Out("==== end of frame, backbuffer %s: %s", SurfName(bb).c_str(), bbNote.c_str());
        bb->Release();
    }
    // shaders
    static D3DDisassemble_t dis = [] {
        wchar_t p[MAX_PATH]; GetSystemDirectoryW(p, MAX_PATH); wcscat_s(p, L"\\d3dcompiler_47.dll");
        HMODULE h = LoadLibraryW(p);
        return h ? (D3DDisassemble_t)GetProcAddress(h, "D3DDisassemble") : nullptr;
    }();
    unsigned nsh = 0;
    for (void* p : g.usedShaders) {
        ShaderInfo si;
        if (!FindShader(p, &si)) continue;
        wchar_t base[MAX_PATH];
        swprintf_s(base, L"%sshaders\\%s_%08x", g.dir.c_str(), si.ps ? L"ps" : L"vs", si.hash);
        FILE* f = _wfopen((std::wstring(base) + L".bin").c_str(), L"wb");
        if (f) { fwrite(si.code.data(), 4, si.code.size(), f); fclose(f); }
        if (dis) {
            ID3DBlob* blob = nullptr;
            if (SUCCEEDED(dis(si.code.data(), si.code.size() * 4, 0, nullptr, &blob)) && blob) {
                f = _wfopen((std::wstring(base) + L".txt").c_str(), L"wb");
                if (f) { fwrite(blob->GetBufferPointer(), 1, blob->GetBufferSize() - 1, f); fclose(f); }
                blob->Release();
            }
        }
        nsh++;
    }
    // write the file: summary, resources, events
    std::string head;
    char b[512];
    sprintf_s(b, "FF13VR capture %u, frame %ld, %.1f s after start\n%u draws, %u clears, %u StretchRects, %u RT snapshots, %u shaders%s\n\n",
              g.id, G().frame, SecondsSinceStart(), g.draws, g.clears, g.stretches, g.snapshots, nsh, g.truncated ? " (LOG TRUNCATED)" : "");
    head += b;
    head += "==== draws per render target (RT0)\n";
    for (auto& kv : g.drawsPerRt) { sprintf_s(b, "%-6s %5u  %s\n", kv.first.c_str(), kv.second, g.descs[kv.first].c_str()); head += b; }
    head += "\n==== resources\n";
    for (auto& kv : g.descs) { head += kv.first + ": " + kv.second + "\n"; }
    head += "\n";
    FILE* f = _wfopen((g.dir + L"frame.txt").c_str(), L"wb");
    if (f) { fwrite(head.data(), 1, head.size(), f); fwrite(g.body.data(), 1, g.body.size(), f); fclose(f); }
    LOGI("capture %u: %u draws, %u clears, %u stretches, %u snapshots, %u shaders -> %Sframe.txt", g.id, g.draws, g.clears, g.stretches,
         g.snapshots, nsh, g.dir.c_str());
    g.active = false;
    g.body.clear();
    g.body.shrink_to_fit();
}

} // namespace fvr
