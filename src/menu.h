#pragma once
// In-game settings menu (vrkit, D3D9 backend): hold L3+R3 or press Insert. See claude/settings-menu.md.
#include <d3d9.h>

namespace fvr {

void MenuInit(IDirect3DDevice9* dev);   // once the device exists (BeforePresent)
bool MenuUpdate();                       // each frame: input, open/close; true while open
// Draw the open menu onto `surface` inside `rect` (the HUD region of the image going to the headset).
void MenuRender(IDirect3DDevice9* dev, IDirect3DSurface9* surface, const RECT& rect);
bool MenuBlocksGameInput();              // the game should see no pad / keyboard input now
void MenuOnLostDevice();

} // namespace fvr
