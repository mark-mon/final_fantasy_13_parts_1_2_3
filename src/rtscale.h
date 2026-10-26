#pragma once
#include <d3d9.h>

namespace fvr {

// Render-target scaling: the game sizes its scene / post-processing / UI targets from its own (16:9) resolution
// setting. To render at the headset's resolution, every screen-derived target (base W/2^k x H/2^k) is created at the
// headset size instead (W*sx/2^k x H*sy/2^k). The game never notices: those surfaces report their original size,
// and its viewports, scissor rects, StretchRect rects and Clear rects are scaled on the way to D3D.

struct ScaleInfo { DWORD magic; UINT origW, origH; float sx, sy; };

// Called for the game's real device (before it creates any target). Decides the scene size: render_width/height,
// else the headset's recommended per-eye size x render_scale (waits for the XR host), else no scaling.
void RtScaleInit(UINT baseW, UINT baseH);
bool RtScaleActive();
UINT RtSceneW();   // size of the game's full-screen targets as created (= base size when not scaling)
UINT RtSceneH();
UINT RtBaseW();
UINT RtBaseH();

// If a target of this size is screen-derived, returns true and the size to create it at.
bool RtScaleDims(UINT w, UINT h, UINT* outW, UINT* outH);
// Marks a created resource (and, for textures, every level surface) with its original size.
void RtMarkSurface(IDirect3DSurface9* s, UINT origW, UINT origH, UINT realW, UINT realH);
void RtMarkTexture(IDirect3DTexture9* t, UINT origW, UINT origH, UINT realW, UINT realH);
bool RtGetInfo(IDirect3DResource9* r, ScaleInfo* out);

// Current RT0 (for viewport / scissor / clear scaling).
void RtOnSetRT0(IDirect3DSurface9* s);
bool RtRT0Scaled(ScaleInfo* out);
D3DVIEWPORT9 RtScaleViewport(const D3DVIEWPORT9& vp, const ScaleInfo& si);
RECT RtScaleRect(const RECT& r, const ScaleInfo& si);

} // namespace fvr
