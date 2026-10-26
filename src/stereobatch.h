#pragma once
// Native stereo, right eye in batches (native_batch = 1): see claude/stereo.md "Right-eye batching".
//
// The per-draw path (left draw, switch to the right eye's targets, right draw, switch back) cost the 5090 PC ~4-5 ms
// of GPU time per frame (~700 render-target switches). Here the left eye is drawn as the game issues it, and
// everything that happens in the same render-target pass (state changes, clears, draws with their right-eye
// constants) is recorded. At the end of the pass (before anything that would change what the recorded draws use)
// the right eye is drawn in one go: switch once, restore the state of the pass start, replay, switch back.
#include <d3d9.h>

namespace fvr {

bool BatchEnabled();   // native stereo with native_batch = 1

// Full device-state shadow (the device is pure, nothing can be read back). Call from every state hook, for the
// game's calls only (not our internal ones), with the values actually sent to the device.
void BsRenderState(D3DRENDERSTATETYPE s, DWORD v);
void BsSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE t, DWORD v);
void BsTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v);
void BsTexture(DWORD stage, IDirect3DBaseTexture9* t);
void BsVertexShader(IDirect3DVertexShader9* s);
void BsPixelShader(IDirect3DPixelShader9* s);
void BsDecl(IDirect3DVertexDeclaration9* d);
void BsFVF(DWORD fvf);
void BsStream(UINT n, IDirect3DVertexBuffer9* vb, UINT off, UINT stride);
void BsStreamFreq(UINT n, UINT div);
void BsIndices(IDirect3DIndexBuffer9* ib);
void BsVSConstF(UINT start, const float* d, UINT n);
void BsVSConstI(UINT start, const int* d, UINT n);
void BsVSConstB(UINT start, const BOOL* d, UINT n);
void BsPSConstF(UINT start, const float* d, UINT n);
void BsPSConstI(UINT start, const int* d, UINT n);
void BsPSConstB(UINT start, const BOOL* d, UINT n);
void BsViewport(const D3DVIEWPORT9& deviceUnits);
void BsScissor(const RECT& deviceUnits);
void BsClipPlane(DWORD i, const float* plane);
void BsOnSetRT0(UINT width, UINT height);   // SetRenderTarget(0) resets viewport and scissor to the whole target
void BsOnReset();
void BsInScene(bool in);   // the game's BeginScene / EndScene (a replay outside a scene brings its own)

// Recording (render thread). Draw kinds: 0 DP, 1 DIP, 2 DPUP, 3 DIPUP.
struct BatchDraw {
    int kind;
    D3DPRIMITIVETYPE type;
    UINT a, b, c, d;          // DP: sv, pc | DIP: mi, nv, si, pc (+ bv) | DPUP: pc, stride | DIPUP: mi, nv, pc, stride
    INT bv;
    const void* idx; D3DFORMAT idxFmt; const void* verts;
};
void BatchRecordDraw(IDirect3DDevice9* dev, const BatchDraw& d);   // after the game's (left) draw
bool BatchRecordClear(IDirect3DDevice9* dev, DWORD n, const D3DRECT* r, DWORD fl, D3DCOLOR c, float z, DWORD s);
enum BatchFlushReason { BF_TARGET, BF_DEPTH, BF_COPY, BF_LOCK, BF_ENDSCENE, BF_PRESENT, BF_UPDATE, BF_FULL, BF_COUNT };
void BatchFlush(IDirect3DDevice9* dev, int reason);   // draw the recorded right eye now (no-op when nothing is recorded)
bool BatchOpen();
void BatchLogStatus();
// After our own drawing (in-game menu): the game's render target 0, depth buffer and the states an overlay touches.
void BsRestoreDevice(IDirect3DDevice9* dev);

} // namespace fvr
