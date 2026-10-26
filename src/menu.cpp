// FF XIII adapter for the vrkit in-game settings menu (sharedtools\vrkit, D3D9 backend).
// Registers the config.txt settings that make sense to change in the headset, pointing straight at G().s, and wires
// each one to what applies it live. Settings the game only reads at start-up are marked "restart".
#include "menu.h"
#include "globals.h"
#include "d3d9hooks.h"
#include "stereobatch.h"
#include "xrbridge/client.h"
#include "vrkit_options.h"
#include "vrkit_menu_d3d9.h"
#include "vrkit_input.h"
#include <string>
#include <cstdio>

namespace fvr {

namespace {
bool g_ok = false;
// choice settings kept as text for the menu (the config stores them as text too). Restart-only settings are copies:
// they are saved, but the running game keeps what it started with.
std::string g_vrMethod, g_msaa, g_fovMode, g_chord;
float g_renderScale = 1.f;

void LogToMod(const char* m) { LOGI("%s", m); }

void ApplyHud() { xrb::ClientSetHud(G().s.hudDistance, G().s.hudWidth, G().s.hudOffsetY, false); }
void ApplyScreen() { xrb::ClientSetMenu(G().s.screenDistance, G().s.screenWidth); }

void Register() {
    using vrkit::Options;
    Options& o = Options::Get();
    if (!o.All().empty()) return;
    Settings& s = G().s;
    g_vrMethod = s.vrMethod == VR_AER ? "aer" : "native";
    g_msaa = s.renderMsaa < 0 ? "game" : std::to_string(s.renderMsaa);
    g_fovMode = std::to_string(s.fovMode);
    g_chord = s.menuChord;
    g_renderScale = s.renderScale;

    // ---- View
    o.AddAction("View", "Recenter view", [] { xrb::ClientRequestRecenter(); }, "Make the current head position the centre (also F8).");
    o.AddFloat("View", "world_scale", "World scale", &s.worldScale, 0.25f, 4.f, 0.05f, 2,
               "Game units per metre: higher = the world feels smaller, eye distance smaller.");
    auto& fov = o.AddChoice("View", "fov_mode", "Field of view", &g_fovMode, {"1", "0"},
                            "Headset = fills your view. Game = the game's own narrower view (a window into the world).",
                            [] { G().s.fovMode = atoi(g_fovMode.c_str()); });
    fov.choiceLabels = {"Headset", "Game"};
    o.AddFloat("View", "cull_fov", "Culling FOV", &s.cullFov, 1.f, 5.f, 0.25f, 2,
               "Draw objects the game's own camera wouldn't see (e.g. a ceiling when you look up). Higher costs frame rate. 1 = off.");

    // ---- Stereo & timing
    auto& m = o.AddChoice("Stereo", "vr_method", "Stereo method", &g_vrMethod, {"native", "aer"},
                          "Native = both eyes every frame. AER = alternate eyes (lighter on the GPU, eyes 1 frame apart).");
    m.choiceLabels = {"Native", "AER"};
    m.needsRestart = true;   // the right-eye targets are created with the game's
    o.AddBool("Stereo", "native_batch", "Right eye in batches", &s.nativeBatch,
              "Native: draw the right eye once per render pass (far fewer target switches). Off = after every draw.")
        .visible = [] { return G().s.vrMethod == VR_NATIVE; };
    o.AddBool("Stereo", "headset_sync", "Lock to headset", &s.headsetSync,
              "Even 60 fps locked to the headset's refresh (steady 2-1 rhythm at 90 Hz).");
    o.AddBool("Stereo", "sync_below_60", "Lock below 60 Hz", &s.syncBelow60,
              "Also lock when the VR output runs below 60 Hz (e.g. OFXR Bridge 3X frame generation at 30 Hz).");
    o.AddBool("Stereo", "reproject", "Camera reprojection", &s.reproject,
              "Turn the image with the game camera between frames. Smoother pans, but near objects ghost.");

    // ---- Resolution
    o.AddFloat("Resolution", "render_scale", "Render scale", &g_renderScale, 0.5f, 2.f, 0.05f, 2,
               "3D resolution per eye relative to the headset's recommended size.").needsRestart = true;
    auto& aa = o.AddChoice("Resolution", "render_msaa", "Anti-aliasing (MSAA)", &g_msaa, {"game", "0", "2", "4"},
                           "MSAA of the 3D scene. Game = 4x as the game asks.");
    aa.choiceLabels = {"Game (4x)", "Off", "2x", "4x"};
    aa.needsRestart = true;

    // ---- HUD & screen
    o.AddFloat("HUD", "hud_distance", "HUD distance (m)", &s.hudDistance, 0.5f, 5.f, 0.1f, 1, "How far away the HUD panel floats.", ApplyHud);
    o.AddFloat("HUD", "hud_width", "HUD width (m)", &s.hudWidth, 0.4f, 5.f, 0.1f, 1, "Width of the HUD panel (and of this menu).", ApplyHud);
    o.AddFloat("HUD", "hud_offset_y", "HUD height (m)", &s.hudOffsetY, -1.f, 1.f, 0.05f, 2, "Move the HUD panel up or down.", ApplyHud);
    o.AddFloat("HUD", "screen_distance", "Screen distance (m)", &s.screenDistance, 1.f, 10.f, 0.1f, 1,
               "Menus and movies: distance of the flat screen.", ApplyScreen);
    o.AddFloat("HUD", "screen_width", "Screen width (m)", &s.screenWidth, 1.f, 10.f, 0.1f, 1, "Menus and movies: width of the flat screen.",
               ApplyScreen);

    // ---- Diagnostics
    o.AddBool("Diagnostics", "perf_diag", "Performance log", &s.perfDiag, "GPU / CPU timings in ff13vr.log every 10 s.");
    auto& ch = o.AddChoice("Diagnostics", "menu_chord", "Menu button", &g_chord, {"l3r3", "ltrt", "none"},
                           "Pad buttons that open this menu (hold). Insert always works.");
    ch.choiceLabels = {"L3 + R3", "LT + RT", "Keyboard only"};
    ch.needsRestart = true;
}

std::string StatusLine() {
    static LONG lastFrame = 0;
    static DWORD lastTick = 0;
    static char text[64] = "";
    DWORD now = GetTickCount();
    if (now - lastTick >= 500) {
        LONG f = G().frame;
        if (lastTick) sprintf_s(text, "%.1f fps", (f - lastFrame) * 1000.0 / (now - lastTick));
        lastFrame = f;
        lastTick = now;
    }
    return text;
}
} // namespace

void MenuInit(IDirect3DDevice9* dev) {
    if (g_ok || !dev) return;
    vrkit::SetLog(LogToMod);
    Register();
    vrkit::menu9::Settings9 s;
    s.menu.title = std::string(G().game == GAME_FF13_2 ? "FF XIII-2" : G().game == GAME_LR ? "Lightning Returns" : "FF XIII") + " VR";
    s.menu.configPath = G().dllDir + L"config.txt";
    s.menu.statusLine = StatusLine;
    s.menu.fontOversample = 2.f;   // drawn up to ~1.8x larger into the HUD region
    s.menu.backgroundAlpha = 1.f;  // the HUD (minimap etc.) is behind it
    const Settings& c = G().s;
    s.menu.input.chordHoldMs = c.menuHoldMs;
    s.menu.input.chordButtons = c.menuChord == "l3r3" ? (0x0040 | 0x0080) : 0;   // XINPUT_GAMEPAD_LEFT_THUMB | RIGHT_THUMB
    s.menu.input.chordTriggers = c.menuChord == "ltrt";
    s.menu.input.keyboardToggleVk = c.menuKey;
    s.restore = BsRestoreDevice;
    InternalScope in;
    g_ok = vrkit::menu9::Init(dev, s);
    if (!g_ok) LOGE("settings menu: init failed (menu unavailable)");
    else LOGI("settings menu ready: hold %s %d ms or press key 0x%X", c.menuChord == "l3r3" ? "L3+R3" : c.menuChord == "ltrt" ? "LT+RT" : "(no pad)",
              c.menuHoldMs, c.menuKey);
}

bool MenuUpdate() {
    if (!g_ok) return false;
    static bool wasOpen = false;
    // diagnostics for "the menu doesn't open": what the menu's input sees
    static int lastPad = -2;
    int pad = vrkit::input::PadUser();
    if (pad != lastPad) {
        if (pad >= 0) LOGI("settings menu: XInput pad found (user %d)", pad);
        else if (lastPad >= 0 || lastPad == -2) LOGI("settings menu: no XInput pad found (the keyboard key still works)");
        lastPad = pad;
    }
    static bool keyWas = false;
    bool keyNow = G().s.menuKey && (GetAsyncKeyState(G().s.menuKey) & 0x8000) != 0;
    if (keyNow && !keyWas) LOGI("settings menu: key 0x%X pressed", G().s.menuKey);
    keyWas = keyNow;
    bool open = vrkit::menu9::Update();
    if (open != wasOpen) { LOGI("settings menu %s", open ? "opened" : "closed"); wasOpen = open; }
    return open;
}

void MenuRender(IDirect3DDevice9* dev, IDirect3DSurface9* surface, const RECT& rect) {
    if (!g_ok || !vrkit::menu9::IsOpen()) return;
    InternalScope in;
    // a 4:3 panel over the middle ~80 % of the HUD region
    LONG w = rect.right - rect.left, h = rect.bottom - rect.top;
    RECT r = {rect.left + w / 10, rect.top + h / 10, rect.right - w / 10, rect.bottom - h / 10};
    vrkit::menu9::Render(dev, surface, r);
}

bool MenuBlocksGameInput() { return g_ok && vrkit::input::GameInputBlocked(); }
void MenuOnLostDevice() { if (g_ok) vrkit::menu9::OnLostDevice(); }

} // namespace fvr
