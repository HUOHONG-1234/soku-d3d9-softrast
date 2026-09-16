/* 手写: 纹理的 surface 层 (IDirect3DSurface9)
   顺序严格照 d3d9.h 的 IDirect3DSurface9 声明 —— 与 stubs_surface9.h 同序。 */
#pragma once
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override { return HT_S_GetDevice(this, ppDevice); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID refguid,CONST void* pData,DWORD SizeOfData,DWORD Flags) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid,void* pData,DWORD* pSizeOfData) override { if (pSizeOfData) *pSizeOfData = 0; return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid) override { return S_OK; }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD PriorityNew) override { return 0; }
    DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
    void STDMETHODCALLTYPE PreLoad() override { }
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_SURFACE; }
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid,void** ppContainer) override { return HT_S_GetContainer(this, riid, ppContainer); }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *pDesc) override { return HT_S_GetDesc(this, pDesc); }
    HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT* pLockedRect,CONST RECT* pRect,DWORD Flags) override { return HT_S_LockRect(this, pLockedRect, pRect, Flags); }
    HRESULT STDMETHODCALLTYPE UnlockRect() override { return HT_S_UnlockRect(this); }
    HRESULT STDMETHODCALLTYPE GetDC(HDC *phdc) override { if (phdc) *phdc = NULL; return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC hdc) override { return S_OK; }
