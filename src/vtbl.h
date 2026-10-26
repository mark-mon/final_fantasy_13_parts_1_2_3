#pragma once
/* COM vtable slot indices computed in vtbl.c from the SDK's C declarations (never hand-maintained). */

#define VT_D3D9_LIST(X) X(CreateDevice)
#define VT_D3D9EX_LIST(X) X(CreateDeviceEx)
#define VT_DEVEX_LIST(X) X(ResetEx)
#define VT_DEV_LIST(X) \
    X(Reset) X(Present) X(GetBackBuffer) X(CreateTexture) X(CreateVolumeTexture) X(CreateCubeTexture) \
    X(CreateVertexBuffer) X(CreateIndexBuffer) X(SetRenderTarget) X(SetDepthStencilSurface) X(BeginScene) \
    X(EndScene) X(Clear) X(SetViewport) X(SetTexture) X(DrawPrimitive) X(DrawIndexedPrimitive) \
    X(DrawPrimitiveUP) X(DrawIndexedPrimitiveUP) X(CreateVertexShader) X(SetVertexShader) \
    X(SetVertexShaderConstantF) X(CreatePixelShader) X(SetPixelShader) X(SetPixelShaderConstantF) \
    X(StretchRect) X(SetRenderState) X(SetScissorRect) X(SetStreamSource) X(SetVertexDeclaration) \
    X(UpdateTexture) X(UpdateSurface) X(CreateRenderTarget) X(CreateDepthStencilSurface) X(GetRenderTargetData) X(ColorFill) \
    X(CreateQuery) X(SetSamplerState) X(SetTextureStageState) X(SetIndices) X(SetFVF) X(SetStreamSourceFreq) \
    X(SetVertexShaderConstantI) X(SetVertexShaderConstantB) X(SetPixelShaderConstantI) X(SetPixelShaderConstantB) \
    X(SetClipPlane) X(CreateAdditionalSwapChain)
#define VT_QUERY_LIST(X) X(Issue) X(GetData) X(GetType)
#define VT_SC_LIST(X) X(Present)
#define VT_TEX_LIST(X) X(LockRect) X(UnlockRect) X(AddDirtyRect) X(GetLevelDesc)
#define VT_CUBE_LIST(X) X(LockRect) X(UnlockRect) X(AddDirtyRect) X(GetLevelDesc)
#define VT_SURF_LIST(X) X(GetDesc) X(LockRect) X(UnlockRect)
#define VT_VOL_LIST(X) X(LockBox)
#define VT_VB_LIST(X) X(Lock)
#define VT_IB_LIST(X) X(Lock)
#define VT_DI_LIST(X) X(CreateDevice)
#define VT_DIDEV_LIST(X) X(GetDeviceState) X(GetDeviceData) X(SetDataFormat) X(GetDeviceInfo) X(Acquire) X(Poll)

#ifdef __cplusplus
extern "C" {
#endif
#define FVR_DECL_D3D9(m) extern const int VT_D3D9_##m;
#define FVR_DECL_DEV(m) extern const int VT_DEV_##m;
#define FVR_DECL_SC(m) extern const int VT_SC_##m;
#define FVR_DECL_SURF(m) extern const int VT_SURF_##m;
VT_SURF_LIST(FVR_DECL_SURF)
#define FVR_DECL_QUERY(m) extern const int VT_QUERY_##m;
VT_QUERY_LIST(FVR_DECL_QUERY)
#define FVR_DECL_TEX(m) extern const int VT_TEX_##m;
#define FVR_DECL_CUBE(m) extern const int VT_CUBE_##m;
#define FVR_DECL_VOL(m) extern const int VT_VOL_##m;
#define FVR_DECL_VB(m) extern const int VT_VB_##m;
#define FVR_DECL_IB(m) extern const int VT_IB_##m;
VT_TEX_LIST(FVR_DECL_TEX)
VT_CUBE_LIST(FVR_DECL_CUBE)
VT_VOL_LIST(FVR_DECL_VOL)
VT_VB_LIST(FVR_DECL_VB)
VT_IB_LIST(FVR_DECL_IB)
#define FVR_DECL_DI(m) extern const int VT_DI_##m;
#define FVR_DECL_DIDEV(m) extern const int VT_DIDEV_##m;
VT_D3D9_LIST(FVR_DECL_D3D9)
#define FVR_DECL_D3D9EX(m) extern const int VT_D3D9EX_##m;
VT_D3D9EX_LIST(FVR_DECL_D3D9EX)
#define FVR_DECL_DEVEX(m) extern const int VT_DEVEX_##m;
VT_DEVEX_LIST(FVR_DECL_DEVEX)
VT_DEV_LIST(FVR_DECL_DEV)
VT_SC_LIST(FVR_DECL_SC)
VT_DI_LIST(FVR_DECL_DI)
VT_DIDEV_LIST(FVR_DECL_DIDEV)
#ifdef __cplusplus
}
#endif
