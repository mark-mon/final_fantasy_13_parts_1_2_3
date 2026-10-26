#pragma once
#include <windows.h>

namespace fvr {

// Detours DirectInput8Create (and later the created devices) plus XInputGetState, so the test script can press
// keys / buttons the game reads through its normal input path. Safe to call several times.
void InstallInputHooks();

// Called once per Present on the render thread: advances the autoload script, hotkeys, injected key timers.
void InputTick();

// Summary of the input devices the game created (for the log).
void LogInputDevices();

} // namespace fvr
