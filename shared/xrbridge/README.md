# XrBridge: shared 64-bit OpenXR host for VR mods

XrBridge lets a game-side mod DLL show its frames in a SteamVR (or any OpenXR) headset without the game process
ever touching OpenXR. That matters for 32-bit games, and for D3D9 games, which OpenXR doesn't support directly.

```
 game.exe (32-bit, D3D9 / D3D11)                         XrBridgeHost.exe (64-bit)
 ┌──────────────────────────────┐   shared memory    ┌──────────────────────────────┐
 │ mod DLL                      │◄── eye poses/FOV ──│ OpenXR session (D3D11)       │
 │  renders L | R | HUD into a  │                    │  opens the shared textures   │
 │  ring of 3 shared textures   │─── frame ready ───►│  copies into swapchains      │
 └──────────────────────────────┘   + shared GPU     │  projection layer + HUD quad │
                                      textures       └──────────────────────────────┘
```

It is derived from the Kingdoms of Amalur VR mod's `AmalurVR_XrHost` and turned into a reusable module.

## What's new compared with the Amalur host (protocol v4)

| | Amalur (v3) | XrBridge (v4) |
|---|---|---|
| Object names | fixed `AmalurVR_*` | per mod: `Local\XrBridge_<instance>_v4…`, so several mods can run at once |
| Producers | D3D11 + keyed mutex only | D3D11 keyed mutex **or** D3D9Ex (`SYNC_SLOT_STATE`: lock-free slot ownership in shared memory + GPU event queries) |
| Texture format | RGBA8 only | RGBA8 or BGRA8. The host picks a matching swapchain, or falls back to a shader blit if the runtime lacks that family |
| HUD size | same as an eye | its own rectangle (e.g. 16:9 1280×720 for FF XIII's UI) |
| HUD alpha | premultiplied only | straight or premultiplied (`hudPremultiplied`) |
| Host log | `<game>\AmalurVR\xrhost.log` | path passed by the mod (`--log`) |

The frame flow (pose seqlock, 3-slot ring, recenter keeping yaw only, pacing event, menu mode) is unchanged.

## Layout

```
include/xrbridge/ipc.h        shared-memory protocol (read the header comment for the sync rules)
include/xrbridge/names.h      kernel object names per instance
include/xrbridge/client.h     game-side API
include/xrbridge/ring_d3d9.h  D3D9Ex shared texture ring (what the FF XIII mods use)
src/host/main.cpp             XrBridgeHost.exe
src/client/client.cpp         game-side link (compile into the mod DLL)
src/client/ring_d3d9.cpp      D3D9Ex ring
src/common/log.*, config.*    logging + key = value config.txt parser (shared by all mods)
test/d3d9test.cpp             XrBridgeTest_d3d9.exe, a 32-bit end-to-end test without any game
third_party/openxr, minhook   OpenXR loader (static, x64) and MinHook (for the mod DLLs)
build.sh                      cross-build with clang-cl + lld-link against the MSVC toolchain
dist/                         built binaries
```

## Using it from a mod

1. Compile `src/client/client.cpp`, `src/common/log.cpp` (plus `ring_d3d9.cpp` for D3D9 games) into the mod DLL
   and add `include/` to the include path.
2. At startup call `xrb::ClientInit()` with a unique `instanceName` (e.g. `"FF13VR"`) and the full path of
   `XrBridgeHost.exe`, which ships next to the game exe. Then `ClientWaitRecommendedSize()` gives the per-eye size.
3. Create the D3D9 device on the headset's GPU (`ClientAdapterLuid()` → `IDirect3D9Ex::GetAdapterLUID`), create a
   `D3D9Ring`, and each frame: `Begin()` → render both eyes with `ClientGetLatestPoses()` + HUD → `End()`.
4. Optionally pace the game with `ClientWaitHostFrame()`.

D3D9 requirement: only an `IDirect3DDevice9Ex` can create shared textures. A D3D9 game mod therefore creates its
device through `Direct3DCreate9Ex`/`CreateDeviceEx` (and redirects any `D3DPOOL_MANAGED` resources to
`D3DPOOL_DEFAULT`/`SYSTEMMEM`, which D3D9Ex doesn't allow).

## Testing the bridge without a game

Copy `dist/XrBridgeHost.exe` and `dist/XrBridgeTest_d3d9.exe` into the same folder, start SteamVR, then run
`XrBridgeTest_d3d9.exe`. In the headset you should see eight coloured pillars around you (red straight ahead), a floor
grid, a white marker ahead, and a framed HUD panel with animated bars. They should stay fixed in space as you move
your head and look solid, not doubled.

Keys in the desktop window: **R** recenter, **H** HUD on/off, **M** menu mode, **F** HUD follows head,
**B** blocking GPU wait, **Esc** quit. The window title shows the host state and frame rates.
Logs: `XrBridgeTest_d3d9.log` and `XrBridgeHost_XrBridgeTest.log` next to the exes.

## Host changes 2026-10-20 (protocol unchanged, v4)

* No busy-wait for the host's GPU copy inside a headset frame. It used to spin up to 4 ms and missed 90 Hz deadlines
  while a game kept the GPU and all cores busy. Slots are now released on a later frame once the copy fence passed.
* Runs at HIGH process priority (frame thread HIGHEST).
* `timing:` line in the host log every 10 s: xrWaitFrame / fetch+copy / xrEndFrame times, longest frame interval,
  late frames.
* Windows build: `build_win_host.cmd` (MSVC x64) → `dist\XrBridgeHost.exe`. The previous binary is kept as
  `dist\XrBridgeHost_v4_prebuilt.exe`.

Additions 2026-10-21 (still protocol v4): client `ClientHostFrameCount()`, `ClientDisplayPeriodNs()`;
`D3D9Ring::SetAutoPump()`, `PumpOne()`, `PendingCount()` (for games that schedule their own hand-over). The host
also releases a slot right after xrEndFrame once its copy has finished.

## Protocol v5 (2026-10-21)

The ring has **4** slots (`kSlots`; v4 had 3). A game that hands frames over on its own schedule (FF XIII's phase
lock: one slot waiting for its time, one being rendered, one still held by the host) ran out of slots and dropped
about a quarter of its frames. The shared-memory layout changed (explicit padding removed, see the static_asserts
in ipc.h) and the object names end in `_v5`, so a v4 host and a v5 mod simply don't find each other. Mods built
against the v4 headers must ship the matching v4 host (`dist\XrBridgeHost_v4_prebuilt.exe`) or be rebuilt.

## Protocol v6 (2026-10-21): camera-aware reprojection

Per slot `IpcMotion` (content time per eye in any steady ns clock, angular velocity of the scene in reference space)
and the slot flag `SLOT_REPROJECT`. The host rotates each eye layer by `exp(angVel * dt)` at every headset frame,
dt = how far the display time is past the eye's content time (the clock offset is learned by the host). Games
below the headset rate then pan smoothly instead of in steps. Pass the motion with `D3D9Ring::End(..., &motion)` or
`ClientSubmitSlot(..., &motion)`; without it the flag is cleared. The host logs a `reproject:` line every 10 s.

## Building

`./build.sh` (Linux, clang-cl from zig 0.17 / clang 22 + lld-link, MSVC STL + Windows SDK 10.0.28000 from the
`toolchain32bit.zip` / `toolchain64bit.zip` archives). The MSVC STL in that toolchain requires clang ≥ 22.
Binaries are statically linked (`/MT`) and depend only on system DLLs.
