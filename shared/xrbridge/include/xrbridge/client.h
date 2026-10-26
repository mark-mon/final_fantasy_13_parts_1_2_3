#pragma once
// XrBridge game-side client: creates the shared memory, launches XrBridgeHost.exe and exchanges poses/frames.
// Compile src/client/client.cpp + src/common/log.cpp into the mod DLL (32- or 64-bit).
//
// Typical frame (SYNC_SLOT_STATE / D3D9Ex, see ring_d3d9.h which wraps all of this):
//     ClientGetLatestPoses(pose, fov, &t);      // render both eyes with these
//     int s = ClientAcquireSlot();               // reserve a texture slot (-1: none free -> skip VR output)
//     ... render into slot s, wait until the GPU finished ...
//     ClientSubmitSlot(s, pose, fov, t, flags);
#include <cstdint>
#include <string>
#include "ipc.h"

namespace xrb {

struct ClientSettings {
    std::string instanceName = "Game";      // [A-Za-z0-9_-], <= 31 chars, unique per mod
    std::string appName = "XrBridge game";  // shown by the OpenXR runtime
    std::wstring hostExePath;               // full path of XrBridgeHost.exe ("" = do not launch, e.g. debugging)
    std::wstring hostLogPath;               // "" = next to the host exe
    int logLevel = 2;                       // host log level 0..4
    float hudDistance = 1.6f, hudWidth = 1.6f, hudOffsetY = -0.05f;
    bool hudFollowHead = false;
    bool hudPremultiplied = false;
    float menuDistance = 2.0f, menuWidth = 2.4f;
};

struct TextureSet {
    uint32_t atlasWidth = 0, atlasHeight = 0;
    uint32_t dxgiFormat = 0;                // e.g. 87 (B8G8R8A8_UNORM) for D3D9 A8R8G8B8
    SyncMode sync = SYNC_SLOT_STATE;
    IpcRect eyeRect[2] = {};
    IpcRect hudRect = {};
    uint64_t handles[kSlots] = {};
};

// All Client* calls must come from one thread (normally the render thread).
bool ClientInit(const ClientSettings& s);                 // create shared memory + launch the host. Call once.
void ClientShutdown();                                    // asks the host to quit
bool ClientHostConnected();                               // host alive and OpenXR ready (heartbeat < 2 s old)
bool ClientHostVisible();                                 // session visible/focused: frames are being shown
const char* ClientHostError();                            // "" if none
bool ClientWaitRecommendedSize(unsigned* w, unsigned* h, unsigned timeoutMs);
uint64_t ClientAdapterLuid();                             // GPU the headset runtime uses (0 = unknown)
bool ClientGetLatestPoses(IpcPose pose[2], IpcFov fov[2], int64_t* time);

void ClientPublishTextures(const TextureSet& t);          // (re)announce the texture ring; resets all slot states
int  ClientAcquireSlot();                                 // SYNC_SLOT_STATE: reserve a slot for rendering, -1 if none
void ClientCancelSlot(int slot);                          // give back an acquired slot without submitting it
void ClientSubmitSlot(int slot, const IpcPose pose[2], const IpcFov fov[2], int64_t time, uint32_t flags,
                      const IpcMotion* motion = nullptr);   // motion: with SLOT_REPROJECT
int  ClientLastSubmittedSlot();

void ClientRequestRecenter();
void ClientRequestDump(uint32_t id);                       // debug: host saves its next frame as host_<id>_*.bmp
bool ClientWaitHostFrame(unsigned ms);                    // pace the game to the headset; false if not paced
uint32_t ClientHostFrameCount();                          // increments once per headset frame (host heartbeat)
int64_t ClientDisplayPeriodNs();                          // headset frame period (0 = unknown)
void ClientSetHud(float distance, float width, float offsetY, bool followHead);
void ClientSetMenu(float distance, float width);

// Pose helpers (OpenXR convention). Builds a view matrix (world -> eye) in row-vector, right-handed form and the
// off-axis projection for a given FOV. Games usually need their own conventions; these are reference versions.
void PoseToViewRH(const IpcPose& p, float out[16]);
void FovToProjRH(const IpcFov& f, float nearZ, float farZ, float out[16]);

} // namespace xrb
