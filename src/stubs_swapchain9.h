/* 自动生成, 勿手改: tools/gen_stubs.py */
#pragma once
    HRESULT STDMETHODCALLTYPE Present(CONST RECT* pSourceRect,CONST RECT* pDestRect,HWND hDestWindowOverride,CONST RGNDATA* pDirtyRegion,DWORD dwFlags) override { return HS_Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags); }
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(IDirect3DSurface9* pDestSurface) override { return HS_GetFrontBufferData(pDestSurface); }
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT iBackBuffer,D3DBACKBUFFER_TYPE Type,IDirect3DSurface9** ppBackBuffer) override { return HS_GetBackBuffer(iBackBuffer, Type, ppBackBuffer); }
    HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS* pRasterStatus) override { return HS_GetRasterStatus(pRasterStatus); }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE* pMode) override { return HS_GetDisplayMode(pMode); }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override { LogStub("GetDevice"); if (ppDevice) *ppDevice = nullptr; return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE GetPresentParameters(D3DPRESENT_PARAMETERS* pPresentationParameters) override { return HS_GetPresentParameters(pPresentationParameters); }
