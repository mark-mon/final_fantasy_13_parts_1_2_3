#pragma once
// Minimal 32-bit BMP writer for debug captures (top-down, BI_RGB; alpha byte kept as the 4th channel).
#include <windows.h>
#include <cstdio>
#include <cstdint>

namespace xrb {

// pixels: rows of `pitch` bytes, 4 bytes per pixel. rgba=true if memory order is R,G,B,A (swapped to BMP's B,G,R,A).
inline bool WriteBmp32(const wchar_t* path, const uint8_t* pixels, unsigned w, unsigned h, unsigned pitch, bool rgba) {
    FILE* f = _wfopen(path, L"wb");
    if (!f) return false;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih); ih.biWidth = (LONG)w; ih.biHeight = -(LONG)h; ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
    ih.biSizeImage = w * h * 4;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + ih.biSizeImage;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    uint8_t* row = new uint8_t[w * 4];
    for (unsigned y = 0; y < h; ++y) {
        const uint8_t* s = pixels + (size_t)y * pitch;
        for (unsigned x = 0; x < w; ++x) {
            const uint8_t* p = s + x * 4;
            uint8_t* d = row + x * 4;
            if (rgba) { d[0] = p[2]; d[1] = p[1]; d[2] = p[0]; d[3] = p[3]; }
            else { d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = p[3]; }
        }
        fwrite(row, w * 4, 1, f);
    }
    delete[] row;
    fclose(f);
    return true;
}

} // namespace xrb
