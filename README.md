# Final Fantasy XIII trilogy VR

A native VR mod for the Steam versions of **Final Fantasy XIII**, **Final Fantasy XIII-2** and **Lightning Returns:
Final Fantasy XIII**. One mod for all three games.

- The 3D world is rendered in true stereo at your headset's resolution, with head tracking. You still play in third
  person with a gamepad.
- The HUD (gauges, minimap) floats on its own panel in front of you.
- Menus, pre-rendered movies and the title screens are shown on a large flat screen.
- An in-game settings menu changes most options without restarting.

The mod works on the game's Direct3D 9 output at runtime. It contains no game files and does not modify the games'
program code.

## Requirements

- Windows 10 or 11 with a PC VR headset running **SteamVR**, which must be the active OpenXR runtime.
- The Steam versions of the games.
- A gamepad (XInput). A DualSense or DualShock through DS4Windows works.
- A strong GPU: every frame is drawn twice at headset resolution. Developed on an RTX 5090 with a 90 Hz Lighthouse
  headset. If performance is tight, lower `render_scale`.

## Install

1. Download `FF13VR_v1.0.zip` from the [Releases](../../releases) page.
2. Copy `d3d9.dll`, `XrBridgeHost.exe` and `config.txt` next to the game's exe:

   | Game | Folder (inside `steamapps\common`) |
   |---|---|
   | Final Fantasy XIII | `FINAL FANTASY XIII\white_data\prog\win\bin\` (next to `ffxiiiimg.exe`) |
   | Final Fantasy XIII-2 | `FINAL FANTASY XIII-2\alba_data\prog\win\bin\` (next to `ffxiii2img.exe`) |
   | Lightning Returns | `LIGHTNING RETURNS FINAL FANTASY XIII\` (next to `LRFF13.exe`) |

3. Start SteamVR, then start the game from Steam as usual. `XrBridgeHost.exe` starts by itself.

To uninstall, delete `d3d9.dll` (and `XrBridgeHost.exe`, `config.txt` and the `FF13VR` log folder).

The mod runs the game in a window (`force_windowed = 1`). The window on your desktop shows the flat game image.

## Controls

| | |
|---|---|
| Settings menu | hold **L3 + R3** (both stick clicks), or press **Insert** |
| In the menu | d-pad / left stick to move and change values, **A** select, **B** back / close, **Y** reset, **LB / RB** pages |
| Menu on the keyboard | arrows, **Enter**, **Backspace** (back), **Delete** (reset), **Page Up / Page Down** (pages) |
| Recenter the view | **F8**, or *Recenter view* in the settings menu |

## Settings

All settings live in `config.txt` next to the game exe. Lines starting with `#` are comments, and a missing line uses
its default. Put each value alone on its line: `name = 1  some text` is not read as 1. Most settings can also be
changed in the settings menu.

| Setting | Default | What it does |
|---|---|---|
| `render_scale` | `1.00` | 3D resolution per eye relative to the headset's recommended size. Lower it for more frame rate. |
| `fov_mode` | `1` | `1` = the headset's field of view (fills your view), `0` = the game's own narrower view. |
| `cull_fov` | `1.5` | Draws objects outside the game camera's view (e.g. a ceiling when you look up). Higher costs frame rate; `1` = off. |
| `world_scale` | `1.0` | Game units per metre: higher makes the world feel smaller. |
| `headset_sync` | `1` | Locks the game's frames to the headset's refresh for a steady rhythm. |
| `native_batch` | `1` | Draws the right eye in batches (much faster on most GPUs). |
| `hud_distance`, `hud_width`, `hud_offset_y` | `1.5`, `1.6`, `-0.05` | HUD panel position and size, in metres. |
| `screen_distance`, `screen_width` | `2.5`, `3.2` | Flat screen for menus and movies, in metres. |
| `menu_chord` | `l3r3` | Pad buttons that open the menu: `l3r3`, `ltrt` (both triggers) or `none` (keyboard only). |

Lightning Returns only (all default to `1`; set one to `0` only to compare):

| Setting | What it does |
|---|---|
| `lr_clip_fix` | Gives the depth pass and the colour pass identical eye matrices (prevents flashing and black outlines). |
| `lr_depth_stereo` | Renders the scene depth pass per eye. |
| `lr_flat_until_play` | Keeps the real-time logo animation after the opening movie on the flat screen. |
| `lr_still_until_play` | Keeps the title screen still (no head tracking before gameplay). |

Logs go to `FF13VR\ff13vr.log` (and `xrhost.log`) next to the game exe. Please attach both when reporting a problem.

## Known limitations

- The games still decide what to draw from their own camera. With a wide headset FOV, objects at the edges can pop in;
  raise `cull_fov` (in Lightning Returns this setting has no effect yet).
- Lightning Returns: shadows and some lights are computed for the game's own field of view, so they may sit slightly off
  at the edges of your view.
- Lightning Returns: after quitting to the title screen from a save, the title sequence is treated like gameplay again
  until the game is restarted.
- Lightning Returns may run at 30 fps on some PCs. The mod shows 30 fps evenly at 90 Hz.

## Building from source

You need the Visual Studio 2022 (or newer) Build Tools with the C++ x86/x64 tools. Then run:

```
build.cmd
```

This builds `dist\d3d9.dll` (32-bit) and `XrBridgeHost.exe` (64-bit OpenXR host, built from `shared\xrbridge` on the
first run) and copies both into `dist\`. Use `config\config.txt` as the `config.txt` next to the game.

| Folder | Contents |
|---|---|
| `src\` | The D3D9 proxy: D3D9Ex upgrade, per-eye camera patching, right-eye render targets and batching, render-target scaling, frame pacing, HUD capture, input hooks, settings menu adapter |
| `shared\xrbridge\` | OpenXR host (SteamVR) and the client that hands it the eye images through shared textures |
| `shared\vrkit\` | The in-game settings menu (Dear ImGui) and its input handling |
| `config\config.txt` | The default settings, with comments |

## Licence

The mod's own code is licensed under the GNU General Public License v3.0 (see `LICENSE`). Bundled third-party code
keeps its own licence; see `THIRD_PARTY_NOTICES.md`.

Final Fantasy XIII, Final Fantasy XIII-2 and Lightning Returns: Final Fantasy XIII are trademarks of Square Enix. This
is an unofficial fan project, not affiliated with or endorsed by Square Enix.
