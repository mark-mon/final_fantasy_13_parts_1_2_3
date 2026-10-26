#pragma once
#include <d3d9.h>

namespace fvr {

// called from the exported Direct3DCreate9/Ex
void OnDirect3DCreated(IDirect3D9* d3d, bool isEx);
void SetGameNativeEx();   // the game called Direct3DCreate9Ex itself
IDirect3DSurface9* GameBackBuffer(IDirect3DDevice9* dev);   // AddRef'd: the back buffer the game presents (see d3d9hooks.cpp)
// called once when the game first touches D3D
void OnGameStartingGraphics();

// requests from the input script / hotkeys (executed at the next Present)
void RequestScreenshot();
void RequestAtlasShot();   // saves the next XrBridge atlas (eyes + HUD) we submit
void RequestCapture();
void RequestQuit();
void LogMemory(const char* when);   // 32-bit address space usage
void DumpApiTrace();                // last D3D calls (crash handler)

// true while one of our own D3D calls is running (hooks must pass through untouched)
bool InternalCall();
// marks our own D3D calls for the duration of a scope
struct InternalScope { InternalScope(); ~InternalScope(); };

} // namespace fvr
