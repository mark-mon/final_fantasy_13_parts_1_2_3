#pragma once
// D3D9Ex producer for XrBridge: a ring of shared A8R8G8B8 render-target textures ("atlases" holding left eye,
// right eye and HUD) plus GPU-completion tracking, using SYNC_SLOT_STATE.
//
// Requirements: the device must be an IDirect3DDevice9Ex (plain D3D9 devices cannot create shared resources).
// Resources are D3DPOOL_DEFAULT; on D3D9Ex they survive Reset/ResetEx, so recreate only when the size changes.
//
//   D3D9Ring ring;
//   ring.Create(devEx, eyeW, eyeH, hudW, hudH);            // publishes the textures to the host
//   per frame:
//     ring.Pump();                                         // hand finished frames to the host (cheap, non-blocking)
//     int s = ring.Begin();                                // -1: no free slot / host not connected
//     if (s >= 0) {
//         IDirect3DSurface9* atlas = ring.Surface(s);      // render/StretchRect into ring.EyeRect(e) / ring.HudRect()
//         ...
//         ring.End(s, pose, fov, time, flags);             // queues an event query; submitted once the GPU is done
//     }
#include <windows.h>
#include <d3d9.h>
#include "client.h"

namespace xrb {

class D3D9Ring {
public:
    // eyeW x eyeH per eye, hudW x hudH for the HUD. Layout: [ left | right | HUD ] side by side.
    bool Create(IDirect3DDevice9Ex* dev, unsigned eyeW, unsigned eyeH, unsigned hudW, unsigned hudH);
    void Destroy();
    bool Valid() const { return m_dev != nullptr; }

    int Begin();                                    // reserve a slot (Pump()s first); -1 if none
    void Cancel(int slot);                          // give back a slot without submitting
    // Finish a slot. blocking=false: an event query is issued and Pump() submits it once the GPU is done
    // (adds up to one game frame of latency, no CPU stall). blocking=true: waits for the GPU right here.
    void End(int slot, const IpcPose pose[2], const IpcFov fov[2], int64_t time, uint32_t flags, bool blocking = false,
             const IpcMotion* motion = nullptr);
    void Pump();                                    // submit slots whose GPU work completed
    int PendingCount() const;                       // slots End()ed but not yet submitted (GPU still working)
    // Manual submission (frame pacing): with auto-pump off, Begin()/End() never submit; the caller submits each
    // finished slot itself with PumpOne() (oldest pending slot only; returns true if it was submitted).
    void SetAutoPump(bool on) { m_autoPump = on; }
    bool PumpOne();

    IDirect3DSurface9* Surface(int slot) const { return (slot >= 0 && slot < kSlots) ? m_slot[slot].surf : nullptr; }
    IDirect3DTexture9* Texture(int slot) const { return (slot >= 0 && slot < kSlots) ? m_slot[slot].tex : nullptr; }
    RECT EyeRect(int eye) const;
    RECT HudRect() const;
    unsigned EyeWidth() const { return m_eyeW; }
    unsigned EyeHeight() const { return m_eyeH; }

private:
    struct Slot {
        IDirect3DTexture9* tex = nullptr;
        IDirect3DSurface9* surf = nullptr;
        IDirect3DQuery9* query = nullptr;
        HANDLE shared = nullptr;
        bool pending = false;                       // End() called, waiting for the GPU
        uint32_t order = 0;                         // submission order of pending slots
        IpcPose pose[2];
        IpcFov fov[2];
        int64_t time = 0;
        uint32_t flags = 0;
        IpcMotion motion = {};
    };
    void ReleaseSlots();
    IDirect3DDevice9Ex* m_dev = nullptr;
    Slot m_slot[kSlots];
    unsigned m_eyeW = 0, m_eyeH = 0, m_hudW = 0, m_hudH = 0;
    uint32_t m_order = 0;
    bool m_autoPump = true;
};

} // namespace xrb
