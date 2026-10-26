#include "vrkit_menu_d3d9.h"
#include "vrkit_options.h"
#include <vector>
#include <algorithm>
#include "imgui.h"

namespace vrkit { namespace menu9 {

namespace {
Settings9 g_s;
IDirect3DTexture9* g_font = nullptr;
bool g_ok = false, g_built = false;

struct Vtx { float x, y, z, rhw; D3DCOLOR col; float u, v; };
const DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
std::vector<Vtx> g_vtx;

bool CreateFont(IDirect3DDevice9* dev) {
    if (g_font) return true;
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* px = nullptr; int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    // DEFAULT + DYNAMIC: works on D3D9Ex devices too (no MANAGED pool there)
    if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_font, nullptr)) || !g_font) {
        Log("vrkit: D3D9 font texture failed");
        return false;
    }
    D3DLOCKED_RECT lr;
    if (FAILED(g_font->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD))) { g_font->Release(); g_font = nullptr; return false; }
    for (int y = 0; y < h; ++y) {
        const unsigned char* s = px + (size_t)y * w * 4;
        DWORD* d = (DWORD*)((unsigned char*)lr.pBits + (size_t)y * lr.Pitch);
        for (int x = 0; x < w; ++x, s += 4) d[x] = D3DCOLOR_ARGB(s[3], s[0], s[1], s[2]);
    }
    g_font->UnlockRect(0);
    io.Fonts->SetTexID((ImTextureID)(intptr_t)g_font);
    return true;
}
} // namespace

bool Init(IDirect3DDevice9* dev, const Settings9& s) {
    if (g_ok) return true;
    g_s = s;
    if (!menu::core::InitImGui(s.menu)) return false;
    if (!CreateFont(dev)) return false;
    g_ok = true;
    Log("vrkit: menu ready, D3D9 (%zu options on %zu pages)", Options::Get().All().size(), Options::Get().Pages().size());
    return true;
}

void OnLostDevice() { if (g_font) { g_font->Release(); g_font = nullptr; } }

void Shutdown() {
    OnLostDevice();
    menu::core::ShutdownImGui();
    g_ok = false;
}

bool Update() {
    g_built = false;
    if (!g_ok || !menu::core::UpdateInput()) return false;
    menu::core::BuildFrame();
    g_built = true;
    return true;
}

void Render(IDirect3DDevice9* dev, IDirect3DSurface9* target, const RECT& rect) {
    if (!g_ok || !g_built || !target) return;
    if (!g_font && !CreateFont(dev)) return;
    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || dd->CmdListsCount == 0) return;
    const menu::Settings& ms = menu::core::Get();
    // fit the menu (texW x texH layout units) into rect, aspect kept, centred
    float rw = (float)(rect.right - rect.left), rh = (float)(rect.bottom - rect.top);
    float sc = (std::min)(rw / ms.texW, rh / ms.texH);
    float ox = rect.left + (rw - ms.texW * sc) * 0.5f, oy = rect.top + (rh - ms.texH * sc) * 0.5f;

    dev->SetRenderTarget(0, target);
    dev->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp = { (DWORD)rect.left, (DWORD)rect.top, (DWORD)rw, (DWORD)rh, 0.f, 1.f };
    dev->SetViewport(&vp);
    dev->SetPixelShader(nullptr);
    dev->SetVertexShader(nullptr);
    dev->SetFVF(kFvf);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);   // premultiplied result: alpha = a + dst*(1-a)
    dev->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* cl = dd->CmdLists[n];
        // positions to target pixels (pre-transformed; -0.5 for D3D9's pixel centres), colours ABGR -> ARGB
        g_vtx.resize(cl->VtxBuffer.Size);
        for (int i = 0; i < cl->VtxBuffer.Size; ++i) {
            const ImDrawVert& v = cl->VtxBuffer[i];
            ImU32 c = v.col;
            g_vtx[i] = { ox + v.pos.x * sc - 0.5f, oy + v.pos.y * sc - 0.5f, 0.f, 1.f,
                         (c & 0xFF00FF00) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF), v.uv.x, v.uv.y };
        }
        for (const ImDrawCmd& cmd : cl->CmdBuffer) {
            if (cmd.UserCallback) { if (cmd.UserCallback != ImDrawCallback_ResetRenderState) cmd.UserCallback(cl, &cmd); continue; }
            RECT r = { (LONG)(ox + cmd.ClipRect.x * sc), (LONG)(oy + cmd.ClipRect.y * sc), (LONG)(ox + cmd.ClipRect.z * sc + 0.999f),
                       (LONG)(oy + cmd.ClipRect.w * sc + 0.999f) };
            r.left = (std::max)(r.left, rect.left); r.top = (std::max)(r.top, rect.top);
            r.right = (std::min)(r.right, rect.right); r.bottom = (std::min)(r.bottom, rect.bottom);
            if (r.right <= r.left || r.bottom <= r.top) continue;
            dev->SetScissorRect(&r);
            dev->SetTexture(0, (IDirect3DTexture9*)(intptr_t)cmd.GetTexID());
            dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)g_vtx.size() - cmd.VtxOffset, cmd.ElemCount / 3, cl->IdxBuffer.Data + cmd.IdxOffset,
                                        sizeof(ImDrawIdx) == 2 ? D3DFMT_INDEX16 : D3DFMT_INDEX32, g_vtx.data() + cmd.VtxOffset, sizeof(Vtx));
        }
    }
    if (g_s.restore) g_s.restore(dev);
}

bool IsOpen() { return menu::core::IsOpen(); }
void SetOpen(bool open) { menu::core::SetOpen(open); }

} } // namespace vrkit::menu9
