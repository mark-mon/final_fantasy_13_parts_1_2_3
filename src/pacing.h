#pragma once
#include <windows.h>

namespace fvr {

// Frame pacing diagnostics: hooks Sleep/SleepEx/WaitForSingleObject(Ex)/QueryPerformanceCounter/timeGetTime and
// attributes calls on the render thread to frames, to find out how the game limits itself to 60 fps.
void InstallPacingHooks();
void PacingPresentBegin();     // render thread, at the start of each Present
void PacingPresentEnd();       // after the real Present returned
void PacingLogStatus();        // per status period
// Headset sync / phase lock (render thread, at Present): computes the QPC time at which this frame is to be handed to
// the XR host (fixed pattern locked to the headset's frames). Returns true while active.
bool PacingHeadsetSync(LONGLONG* handover);
// Then: the render thread may run at most one frame ahead of that schedule (exact 60 game frames per second).
void PacingSyncThrottle(LONGLONG handover);
void PacingSyncLogStatus();

} // namespace fvr
