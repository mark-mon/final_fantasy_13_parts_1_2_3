#pragma once
#include <windows.h>
#include <string>
#include "log.h"
#include "config.h"

namespace fvr {

enum VrMethod { VR_AER = 0, VR_NATIVE = 1 };
// Which game loaded us (from the exe name). One DLL for the FF XIII trilogy; per-game differences key off this.
enum Game { GAME_UNKNOWN = 0, GAME_FF13 = 1, GAME_FF13_2 = 2, GAME_LR = 3 };

struct Settings {
    // general
    bool vrEnabled = true;
    int logLevel = 2;
    // display / device
    bool forceWindowed = true;          // run the game in a window (robust, needed for unattended tests)
    float renderScale = 1.0f;           // scene resolution = headset per-eye size x this (0 = the game's own resolution)
    int renderWidth = 0, renderHeight = 0;   // >0: explicit scene resolution instead (no headset needed)
    int renderMsaa = -1;                // -1 = as the game asks (4x), else 0/2/4/8 samples for the resized scene targets
    bool upgradeToEx = true;            // D3D9 -> D3D9Ex (required for shared textures / VR output)
    bool managedStaging = true;         // managed textures: DEFAULT + staging on lock (saves address space) vs DEFAULT+DYNAMIC
    // virtual screen (milestone 1: flat game image on a panel in the headset)
    float screenDistance = 2.5f, screenWidth = 3.2f;
    int recenterKey = VK_F8;            // recenter the headset view
    // stereo (milestone 2)
    bool stereo = true;                 // 3D scene in stereo; menus/movies stay on the flat screen
    int vrMethod = VR_NATIVE;           // native: both eyes every frame (twin targets); aer: alternate-eye frames
    int fovMode = 1;                    // 1 = headset FOV (immersive), 0 = game FOV (a window; nothing can be culled)
    float worldScale = 1.0f;            // game units per meter
    float hudDistance = 1.5f, hudWidth = 1.6f, hudOffsetY = -0.05f;
    float debugHeadYaw = 0;             // test aid: extra head yaw in degrees
    float debugHeadPitch = 0;
    bool debugNativeNoSwitch = false;   // timing test only: native right-eye draws without switching targets (wrong image)
    bool nativeBatch = true;            // native: draw the right eye per render pass in one batch (stereobatch.cpp)
    bool lrDepthStereo = true;          // Lightning Returns: patch the scene depth pass (localProjection/ViewMatrix) per eye (t4)
    bool lrFlatUntilPlay = true;        // Lightning Returns: no stereo frames until gameplay is seen (start-up logo animation)
    bool lrStillUntilPlay = true;       // Lightning Returns: no head tracking until gameplay is seen (title screen crest)
    bool screenRecipFix = true;         // pixel shaders reading the screen at vPos * screenSizeReciprocal: scale it per target (t11)
    bool lrClipFix = true;              // Lightning Returns: one clip-space eye correction for all patched draws (depth pre-pass match)
    // in-game settings menu (menu.cpp, vrkit)
    std::string menuChord = "l3r3";     // l3r3 | ltrt | none
    int menuHoldMs = 300;
    int menuKey = VK_INSERT;
    // diagnostics
    int captureKey = VK_F11;            // one-frame draw capture
    int captureAtFrame = 0;             // >0: also capture automatically at this frame number
    bool pacingDiag = false;            // log how the render thread spends each frame (Sleep/waits/Present)
    bool perfDiag = false;              // log GPU busy time / render-thread CPU time per frame
    bool queryNoWait = false;           // the game's EVENT queries report "done" at once (lets CPU and GPU overlap)
    int frameRate = 0;                  // >0 and != 60: target frame rate of the game's frame limiter (it runs at 60)
    float cullFov = 1.0f;               // >1: widen the game camera's culling FOV by this factor (camfov.cpp); 1 = off
    bool reproject = false;              // camera-aware reprojection: the host turns each image with the game camera between frames
    bool syncBelow60 = false;           // phase lock also below 60 Hz (e.g. OFXR 3X: host at 30 Hz gets every 2nd frame, evenly)
    bool headsetSync = true;            // headset at a multiple of 60 Hz: show each game frame for exactly N headset frames
    float debugSyncPeriodMs = 0;        // test aid: pretend the headset period is this (and visible)
    float screenshotInterval = 0;       // seconds between backbuffer screenshots (0 = off)
    int screenshotMax = 30;
    // input / automation
    bool autoload = false;
    std::string autoloadScript;
    float quitAfter = 0;                // seconds after start; 0 = never (test automation)
};

struct Globals {
    std::wstring dllDir;                // folder of d3d9.dll (= game exe folder)
    std::wstring outDir;                // dllDir\FF13VR\  (logs, dumps, captures)
    std::wstring runDir;                // outDir\run_<date>_<time>\  (screenshots and captures of this run)
    xrb::Config cfg;
    Settings s;
    DWORD startTick = 0;
    HWND gameWnd = nullptr;
    volatile LONG frame = 0;
    int game = GAME_UNKNOWN;
    const char* gameName = "unknown game";
};
Globals& G();

float SecondsSinceStart();

// Set on threads that read memory which may vanish (camfov's scan): the crash logger ignores their faults.
extern thread_local bool t_memProbe;

} // namespace fvr
