#include "imageio.h"
#include "d3d9hooks.h"
#include "log.h"
#include "bmp.h"
#include <wincodec.h>
#include <vector>
#include <cstdio>
#include <cmath>
#include <cfloat>

namespace fvr {

const char* FormatName(D3DFORMAT f) {
    static char buf[8][16];
    static int k = 0;
    switch (f) {
#define F(x) case D3DFMT_##x: return #x;
    F(UNKNOWN) F(R8G8B8) F(A8R8G8B8) F(X8R8G8B8) F(R5G6B5) F(X1R5G5B5) F(A1R5G5B5) F(A4R4G4B4) F(A8) F(A2B10G10R10)
    F(A8B8G8R8) F(X8B8G8R8) F(G16R16) F(A2R10G10B10) F(A16B16G16R16) F(L8) F(A8L8) F(L16) F(V8U8) F(Q8W8V8U8)
    F(D16_LOCKABLE) F(D32) F(D15S1) F(D24S8) F(D24X8) F(D24X4S4) F(D16) F(D32F_LOCKABLE) F(D24FS8) F(INDEX16) F(INDEX32)
    F(R16F) F(G16R16F) F(A16B16G16R16F) F(R32F) F(G32R32F) F(A32B32G32R32F) F(DXT1) F(DXT2) F(DXT3) F(DXT4) F(DXT5)
    F(VERTEXDATA)
#undef F
    default: break;
    }
    char* b = buf[k++ & 7];
    DWORD v = (DWORD)f;
    if (v > 0xFFFF) {   // FOURCC (INTZ, RAWZ, NULL, ATI2, ...)
        b[0] = (char)(v & 255); b[1] = (char)((v >> 8) & 255); b[2] = (char)((v >> 16) & 255); b[3] = (char)(v >> 24); b[4] = 0;
    } else sprintf_s(b, 16, "fmt%lu", v);
    return b;
}

static float Half(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = ldexpf((float)m, -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = ldexpf((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}

static bool WritePng(const std::wstring& path, const uint8_t* bgr, unsigned w, unsigned h) {
    static thread_local bool comInit = false;
    if (!comInit) { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); comInit = true; }   // RPC_E_CHANGED_MODE is fine
    IWICImagingFactory* fac = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory1, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac)))) return false;
    bool ok = false;
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    if (SUCCEEDED(fac->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(w, h))) {
        WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
        if (SUCCEEDED(fr->SetPixelFormat(&pf)) && IsEqualGUID(pf, GUID_WICPixelFormat24bppBGR) &&
            SUCCEEDED(fr->WritePixels(h, w * 3, w * 3 * h, (BYTE*)bgr)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit()))
            ok = true;
    }
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    fac->Release();
    return ok;
}

// Converts locked pixels to BGR8. Returns false for unsupported formats.
static bool Convert(D3DFORMAT f, const uint8_t* bits, int pitch, unsigned w, unsigned h, std::vector<uint8_t>& out, std::string& note) {
    out.assign((size_t)w * h * 3, 0);
    char tmp[160];
    auto put = [&](unsigned x, unsigned y, float r, float g, float b) {
        uint8_t* d = &out[((size_t)y * w + x) * 3];
        auto c = [](float v) { return (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255 + 0.5f); };
        d[0] = c(b); d[1] = c(g); d[2] = c(r);
    };
    switch (f) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: {
        bool swap = f == D3DFMT_A8B8G8R8 || f == D3DFMT_X8B8G8R8;
        unsigned amin = 255, amax = 0;
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x) {
                const uint8_t* p = bits + (size_t)y * pitch + x * 4;
                uint8_t* d = &out[((size_t)y * w + x) * 3];
                if (swap) { d[0] = p[2]; d[1] = p[1]; d[2] = p[0]; } else { d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; }
                if (p[3] < amin) amin = p[3];
                if (p[3] > amax) amax = p[3];
            }
        if (f == D3DFMT_A8R8G8B8 || f == D3DFMT_A8B8G8R8) { sprintf_s(tmp, "alpha %u..%u", amin, amax); note = tmp; }
        return true;
    }
    case D3DFMT_A2R10G10B10: case D3DFMT_A2B10G10R10:
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x) {
                uint32_t v = *(const uint32_t*)(bits + (size_t)y * pitch + x * 4);
                float a = (v & 1023) / 1023.f, b = ((v >> 10) & 1023) / 1023.f, c = ((v >> 20) & 1023) / 1023.f;
                if (f == D3DFMT_A2R10G10B10) put(x, y, c, b, a); else put(x, y, a, b, c);
            }
        return true;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5:
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x) {
                uint16_t v = *(const uint16_t*)(bits + (size_t)y * pitch + x * 2);
                if (f == D3DFMT_R5G6B5) put(x, y, (v >> 11) / 31.f, ((v >> 5) & 63) / 63.f, (v & 31) / 31.f);
                else put(x, y, ((v >> 10) & 31) / 31.f, ((v >> 5) & 31) / 31.f, (v & 31) / 31.f);
            }
        return true;
    case D3DFMT_L8: case D3DFMT_A8:
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x) { float v = bits[(size_t)y * pitch + x] / 255.f; put(x, y, v, v, v); }
        return true;
    default: break;
    }
    // float / 16-bit formats: gather channels, then normalise per image
    int ch = 0;
    switch (f) {
    case D3DFMT_A16B16G16R16F: case D3DFMT_A32B32G32R32F: case D3DFMT_A16B16G16R16: ch = 4; break;
    case D3DFMT_G16R16F: case D3DFMT_G32R32F: case D3DFMT_G16R16: ch = 2; break;
    case D3DFMT_R16F: case D3DFMT_R32F: case D3DFMT_L16: ch = 1; break;
    default: note = std::string("unsupported format ") + FormatName(f); return false;
    }
    std::vector<float> px((size_t)w * h * 4, 0);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            const uint8_t* row = bits + (size_t)y * pitch;
            float* d = &px[((size_t)y * w + x) * 4];
            for (int c = 0; c < ch; ++c) {
                float v;
                switch (f) {
                case D3DFMT_A16B16G16R16F: case D3DFMT_G16R16F: case D3DFMT_R16F: v = Half(((const uint16_t*)row)[x * ch + c]); break;
                case D3DFMT_A16B16G16R16: case D3DFMT_G16R16: case D3DFMT_L16: v = ((const uint16_t*)row)[x * ch + c] / 65535.f; break;
                default: v = ((const float*)row)[x * ch + c]; break;
                }
                d[c] = v;
            }
        }
    float mn = FLT_MAX, mx = -FLT_MAX, amn = FLT_MAX, amx = -FLT_MAX;
    int colorCh = ch == 4 ? 3 : ch;
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        for (int c = 0; c < colorCh; ++c) { float v = px[i * 4 + c]; if (std::isfinite(v)) { if (v < mn) mn = v; if (v > mx) mx = v; } }
        if (ch == 4) { float v = px[i * 4 + 3]; if (std::isfinite(v)) { if (v < amn) amn = v; if (v > amx) amx = v; } }
    }
    // colour formats with values inside 0..1 are shown as-is; anything else (HDR, depth) is normalised to its range
    bool normalise = ch != 4 || mn < 0 || mx > 1;
    float lo = normalise ? mn : 0, sc = normalise ? (mx > mn ? 1 / (mx - mn) : 1) : 1;
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            const float* d = &px[((size_t)y * w + x) * 4];
            float r = (d[0] - lo) * sc, g = ch >= 2 ? (d[1] - lo) * sc : r, b = ch >= 3 ? (d[2] - lo) * sc : (ch == 2 ? 0 : r);
            put(x, y, r, g, b);
        }
    sprintf_s(tmp, "rgb %.4g..%.4g%s", mn, mx, normalise ? " (normalised)" : "");
    note = tmp;
    if (ch == 4) { sprintf_s(tmp, ", alpha %.4g..%.4g", amn, amx); note += tmp; }
    return true;
}

bool SaveSurfaceImage(IDirect3DDevice9* dev, IDirect3DSurface9* src, const std::wstring& pathNoExt, std::string* note) {
    std::string dummy;
    std::string& n = note ? *note : dummy;
    InternalScope in;   // real sizes (resized render targets report their original size to the game)
    D3DSURFACE_DESC d;
    if (!src || FAILED(src->GetDesc(&d))) { n = "no surface"; return false; }
    IDirect3DSurface9* rt = src;
    IDirect3DSurface9* resolved = nullptr;
    if (d.MultiSampleType != D3DMULTISAMPLE_NONE) {
        if (FAILED(dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr)) ||
            FAILED(dev->StretchRect(src, nullptr, resolved, nullptr, D3DTEXF_NONE))) {
            n = "MSAA resolve failed";
            if (resolved) resolved->Release();
            return false;
        }
        rt = resolved;
    }
    IDirect3DSurface9* sys = nullptr;
    bool ok = false;
    HRESULT hr = dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    if (SUCCEEDED(hr)) hr = dev->GetRenderTargetData(rt, sys);
    if (SUCCEEDED(hr)) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            std::vector<uint8_t> bgr;
            std::string cn;
            if (Convert(d.Format, (const uint8_t*)lr.pBits, lr.Pitch, d.Width, d.Height, bgr, cn)) {
                ok = WritePng(pathNoExt + L".png", bgr.data(), d.Width, d.Height);
                if (!ok) {   // BMP fallback (expects 4 bytes per pixel)
                    std::vector<uint8_t> bgra((size_t)d.Width * d.Height * 4, 255);
                    for (size_t i = 0; i < (size_t)d.Width * d.Height; ++i) memcpy(&bgra[i * 4], &bgr[i * 3], 3);
                    ok = xrb::WriteBmp32((pathNoExt + L".bmp").c_str(), bgra.data(), d.Width, d.Height, d.Width * 4, false);
                }
            }
            n = std::string(FormatName(d.Format)) + " " + std::to_string(d.Width) + "x" + std::to_string(d.Height) + (cn.empty() ? "" : ", " + cn);
            sys->UnlockRect();
        } else n = "LockRect failed";
    } else {
        char t[96]; sprintf_s(t, "readback of %s %ux%u failed (%08lX)", FormatName(d.Format), d.Width, d.Height, hr); n = t;
    }
    if (sys) sys->Release();
    if (resolved) resolved->Release();
    return ok;
}

} // namespace fvr
