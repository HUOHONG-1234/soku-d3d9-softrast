/* 手写 (照 tools/gen_stubs.py 风格): IDirect3DTexture9 方法体
   顺序严格照 d3d9.h 的声明 —— 虚表槽位铁律, 一个都不能挪。
   纹理对象有多个实例, 所以把 this 传给 HT_* 实现。 */
#pragma once
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override { return HT_GetDevice(ppDevice); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID refguid,CONST void* pData,DWORD SizeOfData,DWORD Flags) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid,void* pData,DWORD* pSizeOfData) override { if (pSizeOfData) *pSizeOfData = 0; return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid) override { return S_OK; }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD PriorityNew) override { return 0; }
    DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
    void STDMETHODCALLTYPE PreLoad() override { }
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_TEXTURE; }
    DWORD STDMETHODCALLTYPE SetLOD(DWORD LODNew) override { return LODNew; }
    DWORD STDMETHODCALLTYPE GetLOD() override { return 0; }
    DWORD STDMETHODCALLTYPE GetLevelCount() override { return 1; }
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) override { return S_OK; }
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override { return D3DTEXF_POINT; }
    void STDMETHODCALLTYPE GenerateMipSubLevels() override { }
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level,D3DSURFACE_DESC *pDesc) override { return HT_GetLevelDesc(this, Level, pDesc); }
    HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT Level,IDirect3DSurface9** ppSurfaceLevel) override { return HT_GetSurfaceLevel(this, Level, ppSurfaceLevel); }
    HRESULT STDMETHODCALLTYPE LockRect(UINT Level,D3DLOCKED_RECT* pLockedRect,CONST RECT* pRect,DWORD Flags) override { return HT_LockRect(this, Level, pLockedRect, pRect, Flags); }
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT Level) override { return HT_UnlockRect(this, Level); }
    HRESULT STDMETHODCALLTYPE AddDirtyRect(CONST RECT* pDirtyRect) override { return S_OK; }
