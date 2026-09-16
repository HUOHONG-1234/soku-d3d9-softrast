/* 自动生成, 勿手改: tools/gen_stubs.py */
#pragma once
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { LogStub("TestCooperativeLevel"); return S_OK; }
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() override { return HD_GetAvailableTextureMem_Val(); }
    HRESULT STDMETHODCALLTYPE EvictManagedResources() override { LogStub("EvictManagedResources"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** ppD3D9) override { return HD_GetDirect3D(ppD3D9); }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* pCaps) override { return HD_GetDeviceCaps(pCaps); }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT iSwapChain,D3DDISPLAYMODE* pMode) override { return HD_GetDisplayMode(iSwapChain, pMode); }
    HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters) override { LogStub("GetCreationParameters"); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT XHotSpot,UINT YHotSpot,IDirect3DSurface9* pCursorBitmap) override { LogStub("SetCursorProperties"); return S_OK; }
    void STDMETHODCALLTYPE SetCursorPosition(int X,int Y,DWORD Flags) override { LogStub("SetCursorPosition"); }
    BOOL STDMETHODCALLTYPE ShowCursor(BOOL bShow) override { return TRUE; }
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters,IDirect3DSwapChain9** pSwapChain) override { LogStub("CreateAdditionalSwapChain"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT iSwapChain,IDirect3DSwapChain9** pSwapChain) override { return HD_GetSwapChain(iSwapChain, pSwapChain); }
    UINT STDMETHODCALLTYPE GetNumberOfSwapChains() override { return 1; }
    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) override { return HD_Reset(pPresentationParameters); }
    HRESULT STDMETHODCALLTYPE Present(CONST RECT* pSourceRect,CONST RECT* pDestRect,HWND hDestWindowOverride,CONST RGNDATA* pDirtyRegion) override { return Core_Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion); }
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT iSwapChain,UINT iBackBuffer,D3DBACKBUFFER_TYPE Type,IDirect3DSurface9** ppBackBuffer) override { return HD_GetBackBuffer(iSwapChain, iBackBuffer, Type, ppBackBuffer); }
    HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT iSwapChain,D3DRASTER_STATUS* pRasterStatus) override { LogStub("GetRasterStatus"); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL bEnableDialogs) override { LogStub("SetDialogBoxMode"); return S_OK; }
    void STDMETHODCALLTYPE SetGammaRamp(UINT iSwapChain,DWORD Flags,CONST D3DGAMMARAMP* pRamp) override { LogStub("SetGammaRamp"); }
    void STDMETHODCALLTYPE GetGammaRamp(UINT iSwapChain,D3DGAMMARAMP* pRamp) override { LogStub("GetGammaRamp"); }
    HRESULT STDMETHODCALLTYPE CreateTexture(UINT Width,UINT Height,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DTexture9** ppTexture,HANDLE* pSharedHandle) override { return Core_CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT Width,UINT Height,UINT Depth,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DVolumeTexture9** ppVolumeTexture,HANDLE* pSharedHandle) override { LogStub("CreateVolumeTexture"); if (ppVolumeTexture) *ppVolumeTexture = nullptr; return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT EdgeLength,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DCubeTexture9** ppCubeTexture,HANDLE* pSharedHandle) override { LogStub("CreateCubeTexture"); if (ppCubeTexture) *ppCubeTexture = nullptr; return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT Length,DWORD Usage,DWORD FVF,D3DPOOL Pool,IDirect3DVertexBuffer9** ppVertexBuffer,HANDLE* pSharedHandle) override { return Core_CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT Length,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DIndexBuffer9** ppIndexBuffer,HANDLE* pSharedHandle) override { return Core_CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT Width,UINT Height,D3DFORMAT Format,D3DMULTISAMPLE_TYPE MultiSample,DWORD MultisampleQuality,BOOL Lockable,IDirect3DSurface9** ppSurface,HANDLE* pSharedHandle) override { LogStub("CreateRenderTarget"); if (ppSurface) *ppSurface = nullptr; return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT Width,UINT Height,D3DFORMAT Format,D3DMULTISAMPLE_TYPE MultiSample,DWORD MultisampleQuality,BOOL Discard,IDirect3DSurface9** ppSurface,HANDLE* pSharedHandle) override { LogStub("CreateDepthStencilSurface"); if (ppSurface) *ppSurface = nullptr; return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9* pSourceSurface,CONST RECT* pSourceRect,IDirect3DSurface9* pDestinationSurface,CONST POINT* pDestPoint) override { return HD_UpdateSurface(pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint); }
    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* pSourceTexture,IDirect3DBaseTexture9* pDestinationTexture) override { return HD_UpdateTexture(pSourceTexture, pDestinationTexture); }
    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* pRenderTarget,IDirect3DSurface9* pDestSurface) override { LogStub("GetRenderTargetData"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT iSwapChain,IDirect3DSurface9* pDestSurface) override { return HD_GetFrontBufferData(iSwapChain, pDestSurface); }
    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* pSourceSurface,CONST RECT* pSourceRect,IDirect3DSurface9* pDestSurface,CONST RECT* pDestRect,D3DTEXTUREFILTERTYPE Filter) override { LogStub("StretchRect"); return S_OK; }
    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9* pSurface,CONST RECT* pRect,D3DCOLOR color) override { LogStub("ColorFill"); return S_OK; }
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT Width,UINT Height,D3DFORMAT Format,D3DPOOL Pool,IDirect3DSurface9** ppSurface,HANDLE* pSharedHandle) override { LogStub("CreateOffscreenPlainSurface"); if (ppSurface) *ppSurface = nullptr; return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD RenderTargetIndex,IDirect3DSurface9* pRenderTarget) override { LogStub("SetRenderTarget"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD RenderTargetIndex,IDirect3DSurface9** ppRenderTarget) override { return HD_GetRenderTarget(RenderTargetIndex, ppRenderTarget); }
    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) override { LogStub("SetDepthStencilSurface"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface) override { return HD_GetDepthStencilSurface(ppZStencilSurface); }
    HRESULT STDMETHODCALLTYPE BeginScene() override { return Core_BeginScene(); }
    HRESULT STDMETHODCALLTYPE EndScene() override { return Core_EndScene(); }
    HRESULT STDMETHODCALLTYPE Clear(DWORD Count,CONST D3DRECT* pRects,DWORD Flags,D3DCOLOR Color,float Z,DWORD Stencil) override { return Core_Clear(Count, pRects, Flags, Color, Z, Stencil); }
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE State,CONST D3DMATRIX* pMatrix) override { return Core_SetTransform(State, pMatrix); }
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE State,D3DMATRIX* pMatrix) override { LogStub("GetTransform"); return S_OK; }
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE a0,CONST D3DMATRIX*) override { LogStub("MultiplyTransform"); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetViewport(CONST D3DVIEWPORT9* pViewport) override { return Core_SetViewport(pViewport); }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* pViewport) override { LogStub("GetViewport"); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetMaterial(CONST D3DMATERIAL9* pMaterial) override { LogStub("SetMaterial"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* pMaterial) override { LogStub("GetMaterial"); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD Index,CONST D3DLIGHT9*) override { LogStub("SetLight"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetLight(DWORD Index,D3DLIGHT9* a1) override { LogStub("GetLight"); return S_OK; }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index,BOOL Enable) override { LogStub("LightEnable"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index,BOOL* pEnable) override { LogStub("GetLightEnable"); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD Index,CONST float* pPlane) override { LogStub("SetClipPlane"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index,float* pPlane) override { LogStub("GetClipPlane"); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE State,DWORD Value) override { return Core_SetRenderState(State, Value); }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE State,DWORD* pValue) override { return Core_GetRenderState(State, pValue); }
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE Type,IDirect3DStateBlock9** ppSB) override { LogStub("CreateStateBlock"); if (ppSB) *ppSB = nullptr; return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override { LogStub("BeginStateBlock"); return S_OK; }
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** ppSB) override { LogStub("EndStateBlock"); if (ppSB) *ppSB = nullptr; return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE SetClipStatus(CONST D3DCLIPSTATUS9* pClipStatus) override { LogStub("SetClipStatus"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* pClipStatus) override { LogStub("GetClipStatus"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD Stage,IDirect3DBaseTexture9** ppTexture) override { LogStub("GetTexture"); if (ppTexture) *ppTexture = nullptr; return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD Stage,IDirect3DBaseTexture9* pTexture) override { return Core_SetTexture(Stage, pTexture); }
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD Stage,D3DTEXTURESTAGESTATETYPE Type,DWORD* pValue) override { LogStub("GetTextureStageState"); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD Stage,D3DTEXTURESTAGESTATETYPE Type,DWORD Value) override { return Core_SetTextureStageState(Stage, Type, Value); }
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD Sampler,D3DSAMPLERSTATETYPE Type,DWORD* pValue) override { LogStub("GetSamplerState"); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD Sampler,D3DSAMPLERSTATETYPE Type,DWORD Value) override { return Core_SetSamplerState(Sampler, Type, Value); }
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* pNumPasses) override { LogStub("ValidateDevice"); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT PaletteNumber,CONST PALETTEENTRY* pEntries) override { LogStub("SetPaletteEntries"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT PaletteNumber,PALETTEENTRY* pEntries) override { LogStub("GetPaletteEntries"); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT PaletteNumber) override { LogStub("SetCurrentTexturePalette"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT *PaletteNumber) override { LogStub("GetCurrentTexturePalette"); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetScissorRect(CONST RECT* pRect) override { LogStub("SetScissorRect"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* pRect) override { LogStub("GetScissorRect"); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL bSoftware) override { LogStub("SetSoftwareVertexProcessing"); return S_OK; }
    BOOL STDMETHODCALLTYPE GetSoftwareVertexProcessing() override { return FALSE; }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float nSegments) override { LogStub("SetNPatchMode"); return S_OK; }
    float STDMETHODCALLTYPE GetNPatchMode() override { return 0.0f; }
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType,UINT StartVertex,UINT PrimitiveCount) override { return Core_DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount); }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE a0,INT BaseVertexIndex,UINT MinVertexIndex,UINT NumVertices,UINT startIndex,UINT primCount) override { return Core_DrawIndexedPrimitive(a0, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount); }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType,UINT PrimitiveCount,CONST void* pVertexStreamZeroData,UINT VertexStreamZeroStride) override { return Core_DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride); }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType,UINT MinVertexIndex,UINT NumVertices,UINT PrimitiveCount,CONST void* pIndexData,D3DFORMAT IndexDataFormat,CONST void* pVertexStreamZeroData,UINT VertexStreamZeroStride) override { LogStub("DrawIndexedPrimitiveUP"); return S_OK; }
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT SrcStartIndex,UINT DestIndex,UINT VertexCount,IDirect3DVertexBuffer9* pDestBuffer,IDirect3DVertexDeclaration9* pVertexDecl,DWORD Flags) override { LogStub("ProcessVertices"); return S_OK; }
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(CONST D3DVERTEXELEMENT9* pVertexElements,IDirect3DVertexDeclaration9** ppDecl) override { LogStub("CreateVertexDeclaration"); if (ppDecl) *ppDecl = nullptr; return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) override { LogStub("SetVertexDeclaration"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) override { LogStub("GetVertexDeclaration"); if (ppDecl) *ppDecl = nullptr; return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD FVF) override { return Core_SetFVF(FVF); }
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD* pFVF) override { LogStub("GetFVF"); return S_OK; }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(CONST DWORD* pFunction,IDirect3DVertexShader9** ppShader) override { LogStub("CreateVertexShader"); if (ppShader) *ppShader = nullptr; return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* pShader) override { LogStub("SetVertexShader"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** ppShader) override { LogStub("GetVertexShader"); if (ppShader) *ppShader = nullptr; return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT StartRegister,CONST float* pConstantData,UINT Vector4fCount) override { LogStub("SetVertexShaderConstantF"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT StartRegister,float* pConstantData,UINT Vector4fCount) override { LogStub("GetVertexShaderConstantF"); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT StartRegister,CONST int* pConstantData,UINT Vector4iCount) override { LogStub("SetVertexShaderConstantI"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT StartRegister,int* pConstantData,UINT Vector4iCount) override { LogStub("GetVertexShaderConstantI"); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT StartRegister,CONST BOOL* pConstantData,UINT BoolCount) override { LogStub("SetVertexShaderConstantB"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT StartRegister,BOOL* pConstantData,UINT BoolCount) override { LogStub("GetVertexShaderConstantB"); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT StreamNumber,IDirect3DVertexBuffer9* pStreamData,UINT OffsetInBytes,UINT Stride) override { return Core_SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride); }
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT StreamNumber,IDirect3DVertexBuffer9** ppStreamData,UINT* pOffsetInBytes,UINT* pStride) override { LogStub("GetStreamSource"); if (ppStreamData) *ppStreamData = nullptr; return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT StreamNumber,UINT Setting) override { LogStub("SetStreamSourceFreq"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT StreamNumber,UINT* pSetting) override { LogStub("GetStreamSourceFreq"); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* pIndexData) override { return Core_SetIndices(pIndexData); }
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** ppIndexData) override { LogStub("GetIndices"); if (ppIndexData) *ppIndexData = nullptr; return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE CreatePixelShader(CONST DWORD* pFunction,IDirect3DPixelShader9** ppShader) override { LogStub("CreatePixelShader"); if (ppShader) *ppShader = nullptr; return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* pShader) override { LogStub("SetPixelShader"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** ppShader) override { LogStub("GetPixelShader"); if (ppShader) *ppShader = nullptr; return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT StartRegister,CONST float* pConstantData,UINT Vector4fCount) override { LogStub("SetPixelShaderConstantF"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT StartRegister,float* pConstantData,UINT Vector4fCount) override { LogStub("GetPixelShaderConstantF"); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT StartRegister,CONST int* pConstantData,UINT Vector4iCount) override { LogStub("SetPixelShaderConstantI"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT StartRegister,int* pConstantData,UINT Vector4iCount) override { LogStub("GetPixelShaderConstantI"); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT StartRegister,CONST BOOL* pConstantData,UINT BoolCount) override { LogStub("SetPixelShaderConstantB"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT StartRegister,BOOL* pConstantData,UINT BoolCount) override { LogStub("GetPixelShaderConstantB"); return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT Handle,CONST float* pNumSegs,CONST D3DRECTPATCH_INFO* pRectPatchInfo) override { LogStub("DrawRectPatch"); return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT Handle,CONST float* pNumSegs,CONST D3DTRIPATCH_INFO* pTriPatchInfo) override { LogStub("DrawTriPatch"); return S_OK; }
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle) override { LogStub("DeletePatch"); return S_OK; }
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE Type,IDirect3DQuery9** ppQuery) override { LogStub("CreateQuery"); if (ppQuery) *ppQuery = nullptr; return D3DERR_INVALIDCALL; }
