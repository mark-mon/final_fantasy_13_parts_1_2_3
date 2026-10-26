#pragma once
// XrBridge shared-memory protocol (v6) between a game-side mod DLL (32- or 64-bit) and
// XrBridgeHost.exe (64-bit OpenXR process).
//
// Every field is fixed-size and naturally aligned so 32-bit and 64-bit builds agree on the layout.
// Each mod uses its own instance name, so several mods (or games) never share a mapping:
//     Local\XrBridge_<name>_v6          shared memory (IpcShared)
//     Local\XrBridge_<name>_v6_frame    auto-reset event, game -> host: a slot was submitted
//     Local\XrBridge_<name>_v6_host     auto-reset event, host -> game: new poses for a new display frame
//
// Texture layout: each slot is ONE shared 2D texture (an "atlas") holding the left eye, the right eye and
// the HUD in the rectangles eyeRect[0], eyeRect[1] and hudRect.
//
// Two synchronisation modes:
//   SYNC_KEYED_MUTEX  D3D11 producer, textures created with D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX.
//                     Game AcquireSync(0 or 1) -> render -> ReleaseSync(1) -> submit.
//                     Host AcquireSync(1) -> copy -> ReleaseSync(0).
//   SYNC_SLOT_STATE   Producers without keyed mutexes (D3D9Ex). Ownership is the slotState[] word,
//                     changed only with InterlockedCompareExchange:
//                       game: FREE -> RENDERING (or a stale READY that is not lastSlot -> RENDERING)
//                             ... GPU work finished (event query) ... RENDERING -> READY, then submit
//                       host: READY -> READING (only lastSlot) ... copy + wait for GPU ... READING -> FREE
#include <cstdint>
#include <cstddef>

namespace xrb {

constexpr uint32_t kIpcMagic = 0x52425258;    // 'XRBR'
constexpr uint32_t kIpcVersion = 6;
constexpr int kSlots = 4;                      // texture ring size
constexpr int kMaxNameLen = 31;                // instance name: letters, digits, '_' and '-'

struct IpcPose { float px, py, pz; float qx, qy, qz, qw; };   // OpenXR convention (RH, +Y up, -Z forward, meters)
struct IpcFov { float left, right, up, down; };               // radians (left/down negative)
struct IpcRect { int32_t x, y, w, h; };
// Motion of the game camera, for camera-aware reprojection. Between game frames the host shows each eye image rotated
// by exp(angVel * dt), dt = how far the headset frame's display time is past the eye's contentTime (the host learns
// the constant offset between the game's clock and XrTime itself, so contentTime can be any steady ns clock).
struct IpcMotion {
    int64_t contentTime[2];    // per eye: time (ns) the eye image shows the game at (an AER eye can be a frame older)
    float angVel[3];           // rad/s: rotation of the scene content in reference space (axis * angle rate)
    float pad;
};

enum HostState : uint32_t {
    HOST_NONE = 0, HOST_STARTING = 1, HOST_READY = 2, HOST_RUNNING = 3, HOST_FOCUSED = 4, HOST_ERROR = 100
};
enum SyncMode : uint32_t { SYNC_KEYED_MUTEX = 0, SYNC_SLOT_STATE = 1 };
enum SlotState : uint32_t { SLOT_FREE = 0, SLOT_RENDERING = 1, SLOT_READY = 2, SLOT_READING = 3 };
enum SlotFlags : uint32_t {
    SLOT_HUD_VISIBLE = 1,      // show the HUD quad
    SLOT_MENU_MODE = 2,        // no 3D world: show only the HUD region on the (larger) menu panel
    SLOT_REPROJECT = 4,        // camera-aware reprojection: rotate the eye layers by slotMotion (see IpcMotion)
};

struct alignas(8) IpcShared {
    uint32_t magic;
    uint32_t version;
    uint32_t gamePid;
    uint32_t hostPid;
    char appName[64];                          // OpenXR application name

    // ---------------------------------------------------------------- host -> game
    volatile uint32_t hostState;
    volatile uint32_t hostHeartbeat;           // incremented every host frame
    uint32_t recWidth, recHeight;               // recommended per-eye size
    uint64_t adapterLuid;                       // GPU the runtime wants (render on this one!)
    volatile uint32_t poseSeq;                  // seqlock: odd while writing
    uint32_t pad0;
    IpcPose eyePose[2];                         // predicted eye poses relative to the recentered reference
    IpcFov eyeFov[2];
    int64_t predictedTime;                      // XrTime of the predicted display time
    int64_t displayPeriodNs;                    // headset frame period
    char runtimeName[64];
    char systemName[64];
    char hostError[128];
    uint32_t swapFormat;                        // DXGI format the host chose for its swapchains (info)
    uint32_t hostCopyMode;                      // 0 = direct copy, 1 = shader blit (format conversion) (info)

    // ---------------------------------------------------------------- game -> host: textures
    volatile uint32_t gameState;                // texture generation; 0 = none. Bump AFTER filling the fields below
    uint32_t atlasWidth, atlasHeight;
    uint32_t texFormat;                         // DXGI_FORMAT of the shared textures (B8G8R8A8_UNORM for D3D9 A8R8G8B8)
    uint32_t syncMode;                          // SyncMode
    IpcRect eyeRect[2];                         // must have equal sizes
    IpcRect hudRect;
    uint32_t pad3;
    uint64_t sharedHandle[kSlots];              // legacy (global) shared handles, 32-bit values

    // ---------------------------------------------------------------- game -> host: frames
    volatile uint32_t submitSeq;                // incremented after each submitted slot
    volatile int32_t lastSlot;                  // newest submitted slot (-1 = none)
    volatile uint32_t slotState[kSlots];        // SYNC_SLOT_STATE only (SlotState)
    IpcPose slotPose[kSlots][2];                // poses the slot was rendered with
    IpcFov slotFov[kSlots][2];
    int64_t slotTime[kSlots];
    uint32_t slotFlags[kSlots];

    // ---------------------------------------------------------------- control / settings (game -> host)
    volatile uint32_t recenterRequest;          // game increments; host recenters when it changes
    volatile uint32_t quitRequest;
    uint32_t hostLogLevel;                      // 0=error .. 4=trace
    float hudDistance, hudWidth, hudOffsetY;    // meters
    uint32_t hudFollowHead;                     // 1 = HUD locked to the head, 0 = locked to the recentered space
    uint32_t hudPremultiplied;                  // 1 = HUD alpha is premultiplied, 0 = straight alpha
    float menuDistance, menuWidth;              // meters
    volatile uint32_t dumpRequest;              // debug: game increments; host saves the next frame's swapchain images
    uint32_t pad4;
    IpcMotion slotMotion[kSlots];               // SLOT_REPROJECT
    uint32_t reserved[30];
};

static_assert(sizeof(IpcPose) == 28, "layout");
// Offsets verified identical for i686 and x86_64 MSVC ABIs (clang -fdump-record-layouts).
// v5: 4 slots (v4 had 3). A game that hands frames over on a schedule (FF XIII phase lock) holds one slot waiting for
// its time and one being rendered while the host still holds one, so 3 starved it.
static_assert(kSlots == 4, "the padding-free layout below assumes 4 slots");
static_assert(sizeof(IpcMotion) == 32, "layout");
static_assert(sizeof(IpcShared) == 1304, "layout");
static_assert(offsetof(IpcShared, predictedTime) == 200, "layout");
static_assert(offsetof(IpcShared, sharedHandle) == 552, "layout");
static_assert(offsetof(IpcShared, submitSeq) == 584, "layout");
static_assert(offsetof(IpcShared, slotState) == 592, "layout");
static_assert(offsetof(IpcShared, slotPose) == 608, "layout");
static_assert(offsetof(IpcShared, slotTime) == 960, "layout");
static_assert(offsetof(IpcShared, recenterRequest) == 1008, "layout");
static_assert(offsetof(IpcShared, slotMotion) == 1056, "layout");
// v6: slotMotion (camera-aware reprojection).

} // namespace xrb
