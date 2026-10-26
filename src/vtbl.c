/* Computes COM vtable slot indices from the SDK's C interface declarations. */
#define CINTERFACE
#define DIRECTINPUT_VERSION 0x0800
#include <stddef.h>
#include <d3d9.h>
#include <dinput.h>
#include "vtbl.h"

#define D3D9(m) const int VT_D3D9_##m = (int)(offsetof(IDirect3D9Vtbl, m) / sizeof(void*));
#define DEV(m) const int VT_DEV_##m = (int)(offsetof(IDirect3DDevice9Vtbl, m) / sizeof(void*));
#define SC(m) const int VT_SC_##m = (int)(offsetof(IDirect3DSwapChain9Vtbl, m) / sizeof(void*));
#define DI(m) const int VT_DI_##m = (int)(offsetof(IDirectInput8WVtbl, m) / sizeof(void*));
#define DIDEV(m) const int VT_DIDEV_##m = (int)(offsetof(IDirectInputDevice8WVtbl, m) / sizeof(void*));
VT_D3D9_LIST(D3D9)
#define D3D9EX(m) const int VT_D3D9EX_##m = (int)(offsetof(IDirect3D9ExVtbl, m) / sizeof(void*));
VT_D3D9EX_LIST(D3D9EX)
#define DEVEX(m) const int VT_DEVEX_##m = (int)(offsetof(IDirect3DDevice9ExVtbl, m) / sizeof(void*));
VT_DEVEX_LIST(DEVEX)
VT_DEV_LIST(DEV)
VT_SC_LIST(SC)
#define SURF(m) const int VT_SURF_##m = (int)(offsetof(IDirect3DSurface9Vtbl, m) / sizeof(void*));
VT_SURF_LIST(SURF)
#define QUERY(m) const int VT_QUERY_##m = (int)(offsetof(IDirect3DQuery9Vtbl, m) / sizeof(void*));
VT_QUERY_LIST(QUERY)
#define TEX(m) const int VT_TEX_##m = (int)(offsetof(IDirect3DTexture9Vtbl, m) / sizeof(void*));
#define CUBE(m) const int VT_CUBE_##m = (int)(offsetof(IDirect3DCubeTexture9Vtbl, m) / sizeof(void*));
#define VOL(m) const int VT_VOL_##m = (int)(offsetof(IDirect3DVolumeTexture9Vtbl, m) / sizeof(void*));
#define VB(m) const int VT_VB_##m = (int)(offsetof(IDirect3DVertexBuffer9Vtbl, m) / sizeof(void*));
#define IB(m) const int VT_IB_##m = (int)(offsetof(IDirect3DIndexBuffer9Vtbl, m) / sizeof(void*));
VT_TEX_LIST(TEX)
VT_CUBE_LIST(CUBE)
VT_VOL_LIST(VOL)
VT_VB_LIST(VB)
VT_IB_LIST(IB)
VT_DI_LIST(DI)
VT_DIDEV_LIST(DIDEV)
