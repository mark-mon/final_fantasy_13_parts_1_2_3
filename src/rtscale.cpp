// Render-target scaling - see rtscale.h.
#include "rtscale.h"
#include "d3d9hooks.h"
#include "globals.h"
#include "xrbridge/client.h"
#include <cmath>

namespace fvr {

static const GUID kScaleGuid = {0x8b0e44a1, 0x7d2c, 0x4c33, {0x9a, 0x51, 0x0f, 0x6e, 0x22, 0xd3, 0x88, 0x17}};
static const DWORD kScaleMagic = 0x53434C31;   // 'SCL1'
static bool g_active = false;
static UINT g_baseW = 0, g_baseH = 0, g_sceneW = 0, g_sceneH = 0;
static float g_sx = 1, g_sy = 1;
static ScaleInfo g_rt0{};
static bool g_rt0Scaled = false;

bool RtScaleActive() { return g_active; }
UINT RtSceneW() { return g_active ? g_sceneW : g_baseW; }
UINT RtSceneH() { return g_active ? g_sceneH : g_baseH; }
UINT RtBaseW() { return g_baseW; }
UINT RtBaseH() { return g_baseH; }

void RtScaleInit(UINT baseW, UINT baseH) {
    g_baseW = baseW; g_baseH = baseH;
    g_active = false;
    const Settings& s = G().s;
    UINT w = 0, h = 0;
    const char* src = "";
    if (s.renderWidth > 0 && s.renderHeight > 0) { w = s.renderWidth; h = s.renderHeight; src = "render_width/height"; }
    else if (s.renderScale > 0 && s.vrEnabled) {
        unsigned rw = 0, rh = 0;
        DWORD t0 = GetTickCount();
        if (xrb::ClientWaitRecommendedSize(&rw, &rh, 20000)) {
            w = (UINT)lroundf(rw * s.renderScale);
            h = (UINT)lroundf(rh * s.renderScale);
            src = "headset";
            LOGI("render size: headset recommends %ux%u per eye, render_scale %.2f (waited %lu ms)", rw, rh, s.renderScale, GetTickCount() - t0);
        } else LOGW("render size: no headset size from the XR host - using the game's resolution");
    }
    if (!w || !h) { LOGI("render size: game resolution %ux%u (no scaling)", baseW, baseH); return; }
    // the XrBridge atlas is [left | right | HUD], all at this size: keep it within 16384 texels
    const UINT kMax = 16384;
    if (3 * w > kMax) {
        UINT nw = kMax / 3;
        h = (UINT)((double)h * nw / w);
        w = nw;
        LOGW("render size limited to %ux%u (max texture width)", w, h);
    }
    if (h > kMax) h = kMax;
    w &= ~1u; h &= ~1u;
    if (w == baseW && h == baseH) { LOGI("render size: %ux%u = game resolution", w, h); return; }
    g_sceneW = w; g_sceneH = h;
    g_sx = (float)w / baseW; g_sy = (float)h / baseH;
    g_active = true;
    LOGI("render size: scene targets %ux%u per eye (from %s), game renders %ux%u -> scale %.3f x %.3f", w, h, src, baseW, baseH, g_sx, g_sy);
}

bool RtScaleDims(UINT w, UINT h, UINT* ow, UINT* oh) {
    if (!g_active || !w || !h) return false;
    for (int k = 0; k <= 8; ++k) {
        UINT d = 1u << k;
        // the game computes W/2^k with some rounding: 720/32 -> 23, 720/64 -> 12
        if ((UINT)abs((int)(w * d) - (int)g_baseW) < d + (k ? d : 1) && (UINT)abs((int)(h * d) - (int)g_baseH) < d + (k ? d : 1)) {
            *ow = (g_sceneW + d - 1) / d;
            *oh = (g_sceneH + d - 1) / d;
            if (!*ow) *ow = 1;
            if (!*oh) *oh = 1;
            return true;
        }
    }
    return false;
}

void RtMarkSurface(IDirect3DSurface9* s, UINT origW, UINT origH, UINT realW, UINT realH) {
    if (!s) return;
    ScaleInfo si{kScaleMagic, origW, origH, (float)realW / origW, (float)realH / origH};
    s->SetPrivateData(kScaleGuid, &si, sizeof(si), 0);
}

void RtMarkTexture(IDirect3DTexture9* t, UINT origW, UINT origH, UINT realW, UINT realH) {
    if (!t) return;
    InternalScope in;
    ScaleInfo si{kScaleMagic, origW, origH, (float)realW / origW, (float)realH / origH};
    t->SetPrivateData(kScaleGuid, &si, sizeof(si), 0);
    for (DWORD l = 0; l < t->GetLevelCount(); ++l) {
        IDirect3DSurface9* s = nullptr;
        if (SUCCEEDED(t->GetSurfaceLevel(l, &s)) && s) {
            UINT ow = origW >> l, oh = origH >> l, rw = realW >> l, rh = realH >> l;
            RtMarkSurface(s, ow ? ow : 1, oh ? oh : 1, rw ? rw : 1, rh ? rh : 1);
            s->Release();
        }
    }
}

bool RtGetInfo(IDirect3DResource9* r, ScaleInfo* out) {
    if (!g_active || !r) return false;
    ScaleInfo si{};
    DWORD sz = sizeof(si);
    if (FAILED(r->GetPrivateData(kScaleGuid, &si, &sz)) || sz != sizeof(si) || si.magic != kScaleMagic) return false;
    if (out) *out = si;
    return true;
}

void RtOnSetRT0(IDirect3DSurface9* s) { g_rt0Scaled = RtGetInfo(s, &g_rt0); }
bool RtRT0Scaled(ScaleInfo* out) { if (g_rt0Scaled && out) *out = g_rt0; return g_rt0Scaled; }

D3DVIEWPORT9 RtScaleViewport(const D3DVIEWPORT9& vp, const ScaleInfo& si) {
    D3DVIEWPORT9 o = vp;
    UINT rw = (UINT)lroundf(si.origW * si.sx), rh = (UINT)lroundf(si.origH * si.sy);
    o.X = (DWORD)lroundf(vp.X * si.sx);
    o.Y = (DWORD)lroundf(vp.Y * si.sy);
    o.Width = (DWORD)lroundf((vp.X + vp.Width) * si.sx) - o.X;
    o.Height = (DWORD)lroundf((vp.Y + vp.Height) * si.sy) - o.Y;
    if (o.X + o.Width > rw) o.Width = rw > o.X ? rw - o.X : 0;
    if (o.Y + o.Height > rh) o.Height = rh > o.Y ? rh - o.Y : 0;
    return o;
}

RECT RtScaleRect(const RECT& r, const ScaleInfo& si) {
    return RECT{lroundf(r.left * si.sx), lroundf(r.top * si.sy), lroundf(r.right * si.sx), lroundf(r.bottom * si.sy)};
}

} // namespace fvr
