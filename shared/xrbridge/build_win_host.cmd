@echo off
rem Builds dist\XrBridgeHost.exe (64-bit) with the local MSVC Build Tools (alternative to build.sh on Linux).
setlocal
set VCVARS=
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat
call "%VCVARS%" >nul || (echo vcvars64 failed & exit /b 1)
cd /d "%~dp0"
if not exist build\x64msvc mkdir build\x64msvc
if not exist dist mkdir dist
cl /nologo /O2 /Zi /MT /EHsc /W3 /std:c++17 /utf-8 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX ^
   /DXR_USE_PLATFORM_WIN32 /DXR_USE_GRAPHICS_API_D3D11 /Iinclude /Ithird_party\openxr\include ^
   /Fobuild\x64msvc\ /Fdbuild\x64msvc\vc.pdb /Fedist\XrBridgeHost.exe ^
   src\host\main.cpp src\common\log.cpp src\common\config.cpp ^
   /link /DEBUG /PDB:dist\XrBridgeHost.pdb /SUBSYSTEM:WINDOWS third_party\openxr\lib\openxr_loader_static.lib ^
   user32.lib gdi32.lib d3d11.lib dxgi.lib dxguid.lib advapi32.lib shlwapi.lib pathcch.lib ole32.lib shell32.lib d3dcompiler.lib
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo BUILD OK
