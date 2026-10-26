#pragma once
#include <d3d9.h>
#include "xrbridge/ipc.h"

namespace fvr {

// Stereo rendering of the 3D scene. Two methods (config vr_method):
//   native: every non-square render target / depth buffer gets a right-eye twin when the game creates it. Each draw
//           into a twinned target is issued twice back to back: left eye into the game's targets, right eye into the
//           twins (with twinned textures swapped in). Clears and StretchRects on twinned targets are mirrored.
//           Both eyes are rendered every frame.
//   aer:    alternate-eye frames - every game frame renders one eye.
// In both, the camera matrices of 3D draws (vertex shaders with worldViewProjMatrix / worldViewMatrix / viewITMatrix
// in their constant table) are rebuilt for the eye and restored after the draw. The finished scene (the StretchRect
// into the game's UI target) becomes the eye image; the UI drawn afterwards is repeated into a transparent HUD
// texture (premultiplied alpha).

void StereoRegisterVS(void* shader, const DWORD* bytecode);   // parses the constant table
void StereoOnSetVS(void* shader);
void StereoOnSetRT(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s);
void StereoOnSetDS(IDirect3DSurface9* s);
void StereoOnSetTexture(DWORD stage, IDirect3DBaseTexture9* t);
void StereoOnViewport(const D3DVIEWPORT9* vp);
void StereoOnScissor(const RECT* r);

// Resource creation (native method: creates the right-eye twins). Call after the game's resource was created.
void StereoOnCreateTexture(IDirect3DDevice9* dev, IDirect3DTexture9* tex, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool);
void StereoOnCreateSurface(IDirect3DDevice9* dev, IDirect3DSurface9* s, bool depth, UINT w, UINT h, D3DFORMAT f,
                           D3DMULTISAMPLE_TYPE ms, DWORD quality, BOOL flag);

// Draw wrapping (see d3d9hooks.cpp):
//   m = StereoPreDraw(); draw;
//   if (m == 3 && StereoRightBegin()) { draw; StereoRightEnd(); }          native: right eye
//   if (m == 2 && StereoHudBegin())   { draw; StereoHudEnd(); }            UI into the HUD texture
//   StereoPostDraw(m);
enum { SD_NONE = 0, SD_AER = 1, SD_HUD = 2, SD_NATIVE = 3 };
int StereoPreDraw(IDirect3DDevice9* dev);
bool StereoRightBegin(IDirect3DDevice9* dev);
void StereoRightEnd(IDirect3DDevice9* dev);
bool StereoHudBegin(IDirect3DDevice9* dev);
void StereoHudEnd(IDirect3DDevice9* dev);
void StereoPostDraw(IDirect3DDevice9* dev, int mode);

// Mirroring of clears and copies for the right eye (native). Call after the game's call was forwarded.
void StereoAfterClear(IDirect3DDevice9* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR c, float z, DWORD s);
void StereoOnStretchRect(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const RECT* dr);
void StereoAfterStretchRect(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const RECT* dr,
                            D3DTEXTUREFILTERTYPE f);

// Frame boundaries (render thread): after the real Present returns / before the next frame starts.
void StereoBeginFrame(IDirect3DDevice9* dev);
void StereoOnReset();

// For the VR submit at Present: true if this frame rendered the 3D scene in stereo.
bool StereoFrameReady();
IDirect3DSurface9* StereoEyeSurface(int eye);
IDirect3DSurface9* StereoHudSurface();
void StereoPoses(xrb::IpcPose pose[2], xrb::IpcFov fov[2], int64_t* time);
void StereoLogStatus();
void StereoCameraPos(float out[3]);   // world position of the game camera (last 3D scene draw)
// Rotation of the camera of the newest captured scene (rows = view axes in world space), its game frame number, and
// the game frame each eye image comes from (AER: one of them is a frame older). False before the first scene.
bool StereoSceneCamera(float rot[3][3], long* frame, long eyeFrame[2]);
// Native, batched (stereobatch.cpp).
struct StereoPatchReg { int reg, count; float right[4][4]; float game[4][4]; };
int StereoRightPatches(StereoPatchReg out[4]);                       // right-eye constants of the current draw
void StereoBindPassTargets(IDirect3DDevice9* dev, bool right);       // the pass's targets: right-eye twins or the game's
IDirect3DBaseTexture9* StereoTwinTexture(IDirect3DBaseTexture9* t);  // right-eye twin of a texture, or null
void StereoNoteRightDraws(unsigned n);
IDirect3DSurface9* StereoBoundRT(DWORD i);                         // the game's bound targets (as the hooks saw them)
IDirect3DSurface9* StereoBoundDS();
bool StereoClearWouldMirror();                                      // a clear now must also clear the right eye

// The game's own projection (P00, P11) of the main scene camera and the frame it was seen in. False before any 3D frame.
bool StereoGameProjection(float* p00, float* p11, long* frame);

} // namespace fvr
