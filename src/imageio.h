#pragma once
#include <d3d9.h>
#include <string>

namespace fvr {

// Copies a render-target surface to system memory (resolving MSAA if needed), converts it to 8-bit RGB and writes
// a PNG (WIC; falls back to BMP). Float / single-channel formats are normalised to their min..max range.
// `note` receives a short description (format, value range) or the reason it failed.
bool SaveSurfaceImage(IDirect3DDevice9* dev, IDirect3DSurface9* src, const std::wstring& pathNoExt, std::string* note);

const char* FormatName(D3DFORMAT f);

} // namespace fvr
