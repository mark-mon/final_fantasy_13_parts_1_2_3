@echo off
rem Builds dist\d3d9.dll (32-bit D3D9 proxy for FF XIII, FF XIII-2 and Lightning Returns) with the MSVC Build Tools,
rem and copies XrBridgeHost.exe (64-bit OpenXR host) next to it, building it first if needed.
setlocal
rem Visual Studio / Build Tools: the newest install vswhere knows about (needs the x86/x64 C++ tools).
set VCVARS=
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VCVARS=%%i\VC\Auxiliary\Build\vcvars32.bat
call "%VCVARS%" >nul || (echo vcvars32 failed: install the MSVC C++ build tools & exit /b 1)
cd /d "%~dp0"
rem Shared code (XrBridge client/host, vrkit menu, MinHook, Dear ImGui, OpenXR): .\shared, or FF13_SHARED=<dir>.
set SHARED=shared
if defined FF13_SHARED set SHARED=%FF13_SHARED%
echo shared code: %SHARED%
set XRB=%SHARED%\xrbridge
set MH=%XRB%\third_party\minhook\src
set VK=%SHARED%\vrkit
set IMGUI=%VK%\third_party\imgui
if not exist build mkdir build
if not exist dist mkdir dist

cl /nologo /LD /O2 /Oy- /Zi /MT /EHsc /W3 /std:c++17 /arch:SSE2 /utf-8 ^
   /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX ^
   /Isrc /I%XRB%\include /I%XRB%\src\common /I%XRB%\third_party\minhook\include /I%VK% /I%IMGUI% ^
   /Fobuild\ /Fdbuild\vc.pdb /Fedist\d3d9.dll ^
   src\dllmain.cpp src\d3d9hooks.cpp src\capture.cpp src\stereo.cpp src\stereobatch.cpp src\pacing.cpp src\rtscale.cpp src\camfov.cpp src\imageio.cpp src\input.cpp src\menu.cpp src\hookutil.cpp src\vtbl.c ^
   %VK%\vrkit_options.cpp %VK%\vrkit_input.cpp %VK%\vrkit_menu_core.cpp %VK%\vrkit_menu_d3d9.cpp ^
   %IMGUI%\imgui.cpp %IMGUI%\imgui_draw.cpp %IMGUI%\imgui_tables.cpp %IMGUI%\imgui_widgets.cpp ^
   %XRB%\src\client\client.cpp %XRB%\src\client\ring_d3d9.cpp %XRB%\src\common\log.cpp %XRB%\src\common\config.cpp ^
   %MH%\hook.c %MH%\buffer.c %MH%\trampoline.c %MH%\hde\hde32.c ^
   /link /DEF:src\d3d9.def /DEBUG /PDB:dist\d3d9.pdb /MAP:dist\d3d9.map /OPT:REF /OPT:ICF /IMPLIB:build\d3d9.lib ^
   user32.lib gdi32.lib ole32.lib windowscodecs.lib advapi32.lib dxguid.lib
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
if not exist %XRB%\dist\XrBridgeHost.exe (call %XRB%\build_win_host.cmd || (echo XrBridgeHost BUILD FAILED & exit /b 1))
cd /d "%~dp0"
copy /y %XRB%\dist\XrBridgeHost.exe dist\ >nul
echo BUILD OK
