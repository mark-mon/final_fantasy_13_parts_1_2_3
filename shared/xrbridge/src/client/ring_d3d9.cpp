// D3D9Ex shared texture ring for XrBridge (see include/xrbridge/ring_d3d9.h).
#include "xrbridge/ring_d3d9.h"
#include "../common/log.h"
#include <cstring>

namespace xrb {

static constexpr uint32_t kDxgiB8G8R8A8Unorm = 87;   // DXGI_FORMAT_B8G8R8A8_UNORM == D3DFMT_A8R8G8B8 memory layout

bool D3D9Ring::Create(IDirect3DDevice9Ex* dev, unsigned eyeW, unsigned eyeH, unsigned hudW, unsigned hudH) {
    Destroy();
    if (!dev || !eyeW || !eyeH || !hudW || !hudH) return false;
    unsigned aw = eyeW * 2 + hudW, ah = eyeH > hudH ? eyeH : hudH;
    for (int i = 0; i < kSlots; ++i) {
        Slot& s = m_slot[i];
        HRESULT hr = dev->CreateTexture(aw, ah, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s.tex, &s.shared);
        if (FAILED(hr) || !s.shared) {
            LOGE("xrbridge d3d9: shared texture %ux%u failed (%08lX, handle %p)", aw, ah, hr, s.shared);
            ReleaseSlots();
            return false;
        }
        if (FAILED(s.tex->GetSurfaceLevel(0, &s.surf)) || !s.surf) { LOGE("xrbridge d3d9: GetSurfaceLevel failed"); ReleaseSlots(); return false; }
        if (FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &s.query))) {
            LOGW("xrbridge d3d9: event queries unsupported - using a flush per frame");
            s.query = nullptr;
        }
        // start transparent black (HUD region alpha = 0)
        dev->ColorFill(s.surf, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
    }
    m_dev = dev;
    m_dev->AddRef();
    m_eyeW = eyeW; m_eyeH = eyeH; m_hudW = hudW; m_hudH = hudH;

    TextureSet t;
    t.atlasWidth = aw; t.atlasHeight = ah;
    t.dxgiFormat = kDxgiB8G8R8A8Unorm;
    t.sync = SYNC_SLOT_STATE;
    t.eyeRect[0] = {0, 0, (int32_t)eyeW, (int32_t)eyeH};
    t.eyeRect[1] = {(int32_t)eyeW, 0, (int32_t)eyeW, (int32_t)eyeH};
    t.hudRect = {(int32_t)eyeW * 2, 0, (int32_t)hudW, (int32_t)hudH};
    for (int i = 0; i < kSlots; ++i) t.handles[i] = (uint64_t)(uintptr_t)m_slot[i].shared;
    ClientPublishTextures(t);
    LOGI("xrbridge d3d9: ring of %d atlases %ux%u (eyes %ux%u, HUD %ux%u)", kSlots, aw, ah, eyeW, eyeH, hudW, hudH);
    return true;
}

void D3D9Ring::ReleaseSlots() {
    for (auto& s : m_slot) {
        if (s.query) s.query->Release();
        if (s.surf) s.surf->Release();
        if (s.tex) s.tex->Release();
        s = Slot();
    }
}

void D3D9Ring::Destroy() {
    ReleaseSlots();
    if (m_dev) {
        // tell the host the textures are gone
        TextureSet t;
        ClientPublishTextures(t);
        m_dev->Release();
    }
    m_dev = nullptr;
    m_eyeW = m_eyeH = m_hudW = m_hudH = 0;
}

RECT D3D9Ring::EyeRect(int eye) const {
    LONG x = eye ? (LONG)m_eyeW : 0;
    return {x, 0, x + (LONG)m_eyeW, (LONG)m_eyeH};
}

RECT D3D9Ring::HudRect() const {
    return {(LONG)m_eyeW * 2, 0, (LONG)(m_eyeW * 2 + m_hudW), (LONG)m_hudH};
}

int D3D9Ring::Begin() {
    if (!m_dev) return -1;
    if (m_autoPump) Pump();
    if (!ClientHostConnected()) return -1;
    return ClientAcquireSlot();
}

void D3D9Ring::Cancel(int slot) {
    if (slot < 0 || slot >= kSlots) return;
    m_slot[slot].pending = false;
    ClientCancelSlot(slot);
}

void D3D9Ring::End(int slot, const IpcPose pose[2], const IpcFov fov[2], int64_t time, uint32_t flags, bool blocking,
                    const IpcMotion* motion) {
    if (!m_dev || slot < 0 || slot >= kSlots) return;
    Slot& s = m_slot[slot];
    memcpy(s.pose, pose, sizeof(s.pose));
    memcpy(s.fov, fov, sizeof(s.fov));
    s.time = time; s.flags = flags;
    s.motion = motion ? *motion : IpcMotion{};
    if (!motion) s.flags &= ~SLOT_REPROJECT;
    s.pending = true;
    s.order = ++m_order;
    if (s.query) {
        s.query->Issue(D3DISSUE_END);
        if (blocking) {
            DWORD t0 = GetTickCount();
            while (s.query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE && GetTickCount() - t0 < 100) YieldProcessor();
        } else {
            s.query->GetData(nullptr, 0, D3DGETDATA_FLUSH);   // kick the command buffer to the GPU now
        }
    }
    if (m_autoPump) Pump();
}

bool D3D9Ring::PumpOne() {
    if (!m_dev) return false;
    int best = -1;
    for (int i = 0; i < kSlots; ++i)
        if (m_slot[i].pending && (best < 0 || m_slot[i].order < m_slot[best].order)) best = i;
    if (best < 0) return false;
    Slot& s = m_slot[best];
    if (s.query && s.query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) return false;   // GPU not done yet
    s.pending = false;
    ClientSubmitSlot(best, s.pose, s.fov, s.time, s.flags, &s.motion);
    return true;
}

int D3D9Ring::PendingCount() const {
    int n = 0;
    for (const auto& s : m_slot) n += s.pending ? 1 : 0;
    return n;
}

void D3D9Ring::Pump() {
    if (!m_dev) return;
    // Submit completed slots oldest-first so lastSlot always ends up as the newest finished frame.
    for (;;) {
        int best = -1;
        for (int i = 0; i < kSlots; ++i)
            if (m_slot[i].pending && (best < 0 || m_slot[i].order < m_slot[best].order)) best = i;
        if (best < 0) return;
        Slot& s = m_slot[best];
        HRESULT r = s.query ? s.query->GetData(nullptr, 0, 0) : S_OK;
        if (r == S_FALSE) return;   // oldest not done yet -> newer ones aren't either
        // S_OK, or an error such as D3DERR_DEVICELOST: either way the slot is no longer in flight
        s.pending = false;
        ClientSubmitSlot(best, s.pose, s.fov, s.time, s.flags, &s.motion);
    }
}

} // namespace xrb
