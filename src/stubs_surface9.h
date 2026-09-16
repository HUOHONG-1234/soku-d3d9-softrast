/* 自动生成, 勿手改: tools/gen_stubs.py */
#pragma once
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override { LogStub("GetDevice"); if (ppDevice) *ppDevice = nullptr; return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID refguid,CONST void* pData,DWORD SizeOfData,DWORD Flags) override { return HF_SetPrivateData(refguid, pData, SizeOfData, Flags); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid,void* pData,DWORD* pSizeOfData) override { return HF_GetPrivateData(refguid, pData, pSizeOfData); }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid) override { return HF_FreePrivateData(refguid); }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD PriorityNew) override { LogStub("SetPriority"); return {}; }
    DWORD STDMETHODCALLTYPE GetPriority() override { LogStub("GetPriority"); return {}; }
    void STDMETHODCALLTYPE PreLoad() override { LogStub("PreLoad"); }
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { LogStub("GetType"); return {}; }
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid,void** ppContainer) override { LogStub("GetContainer"); if (ppContainer) *ppContainer = nullptr; return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *pDesc) override { return HF_GetDesc(pDesc); }
    HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT* pLockedRect,CONST RECT* pRect,DWORD Flags) override { return HF_LockRect(pLockedRect, pRect, Flags); }
    HRESULT STDMETHODCALLTYPE UnlockRect() override { return HF_UnlockRect(); }
    HRESULT STDMETHODCALLTYPE GetDC(HDC *phdc) override { return HF_GetDC(phdc); }
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC hdc) override { return HF_ReleaseDC(hdc); }
