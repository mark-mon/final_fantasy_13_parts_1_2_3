#pragma once
#include <d3d9.h>
#include <cstdint>

namespace fvr {

// Shadow of state the capture needs even when it starts mid-stream (maintained by the device hooks every frame).
struct Shadow {
    float vsc[256][4] = {};
    float psc[224][4] = {};
    DWORD rs[256] = {};
    bool rsKnown[256] = {};
};
Shadow& Sh();

// Shader bytecode registry (filled at Create*Shader so captures can dump what was used).
void RegisterShader(void* shader, bool pixel, const DWORD* bytecode);

// One-frame capture. Begin right after a Present, End at the next Present (before it is forwarded).
bool Capturing();
void CaptureBegin(IDirect3DDevice9* dev, unsigned id);
void CaptureEnd(IDirect3DDevice9* dev);

// Event recorders (only call while Capturing()).
void CapSetRenderTarget(IDirect3DDevice9* dev, DWORD idx, IDirect3DSurface9* s);
void CapSetDepthStencil(IDirect3DSurface9* s);
void CapViewport(const D3DVIEWPORT9* vp);
void CapScissor(const RECT* r);
void CapShader(bool pixel, void* shader);
void CapConstF(bool pixel, UINT start, const float* data, UINT count);
void CapTexture(DWORD stage, IDirect3DBaseTexture9* t);
void CapRenderState(D3DRENDERSTATETYPE s, DWORD v, DWORD prev, bool prevKnown);
void CapStream(UINT n, IDirect3DVertexBuffer9* vb, UINT off, UINT stride);
void CapDecl(IDirect3DVertexDeclaration9* d);
void CapClear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR c, float z, DWORD s);
void CapStretch(IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const RECT* dr, D3DTEXTUREFILTERTYPE f);
void CapDraw(const char* kind, D3DPRIMITIVETYPE type, UINT a, UINT b, UINT c, UINT d, UINT prims);

} // namespace fvr
