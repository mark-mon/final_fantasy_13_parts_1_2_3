// vrkit - in-game settings menu, D3D9 backend (for D3D9 / D3D9Ex games, e.g. the FF XIII VR mod).
// Same menu, options and controls as the D3D11 version (vrkit_menu.h); only the drawing differs: the menu is
// drawn directly onto a surface the host chooses (e.g. the HUD region of the image it hands to its VR
// compositor), scaled into a rectangle, blended "over" with premultiplied output (colour = src*a + dst*(1-a),
// alpha = a + dst_a*(1-a)).
//
// Per frame, on the device's thread:   if (menu9::Update()) { ... menu9::Render(dev, surface, rect); ... }
// Render() changes device state (render target, depth buffer, viewport, scissor, shaders, FVF, stream 0,
// texture 0, render / sampler / texture-stage states it uses). Pure devices can't read state back, so the host
// restores its state afterwards: Settings9::restore is called at the end of Render (required).
#pragma once
#include <d3d9.h>
#include <functional>
#include "vrkit_menu_core.h"

namespace vrkit { namespace menu9 {

struct Settings9 {
    menu::Settings menu;                                  // title, size, font, config path, input
    std::function<void(IDirect3DDevice9*)> restore;       // put the device state back (see above)
};

bool Init(IDirect3DDevice9* dev, const Settings9& s);   // after the device exists; creates the font texture
void Shutdown();
void OnLostDevice();                                    // before Reset (releases the font texture)
// Poll input, handle open/close and navigation, build the menu. Returns true while the menu is open.
bool Update();
// Draw the menu (built by the last Update) onto `target`, fitted into `rect` (aspect kept, centred).
void Render(IDirect3DDevice9* dev, IDirect3DSurface9* target, const RECT& rect);
bool IsOpen();
void SetOpen(bool open);

} } // namespace vrkit::menu9
