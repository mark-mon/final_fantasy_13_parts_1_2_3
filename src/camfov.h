#pragma once
// Culling FOV: finds the game camera's field-of-view value in the game's memory (data only, never code) so that it
// can be widened. The game culls objects with its own camera frustum (~62x38 degrees in the field); in the headset you
// see much more, so objects at the edges of your view were missing. See claude/culling-fov.md.

namespace fvr {

void CamFovTick();                 // once per frame (BeforePresent)
void CamFovScan();                 // script "fovscan": search again now
void CamFovSet(float scale);       // script "fovset <scale>": culling FOV factor (overrides cull_fov; 1 = the game's own)
void CamFovLogStatus();            // status line (every 10 s)

} // namespace fvr
