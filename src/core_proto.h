/* 自动生成, 勿手改: tools/gen_stubs.py  —— Core_* 精确原型 */
#pragma once
HRESULT Core_Present(CONST RECT* pSourceRect,CONST RECT* pDestRect,HWND hDestWindowOverride,CONST RGNDATA* pDirtyRegion);
HRESULT Core_CreateTexture(UINT Width,UINT Height,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DTexture9** ppTexture,HANDLE* pSharedHandle);
HRESULT Core_CreateVertexBuffer(UINT Length,DWORD Usage,DWORD FVF,D3DPOOL Pool,IDirect3DVertexBuffer9** ppVertexBuffer,HANDLE* pSharedHandle);
HRESULT Core_CreateIndexBuffer(UINT Length,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DIndexBuffer9** ppIndexBuffer,HANDLE* pSharedHandle);
HRESULT Core_BeginScene();
HRESULT Core_EndScene();
HRESULT Core_Clear(DWORD Count,CONST D3DRECT* pRects,DWORD Flags,D3DCOLOR Color,float Z,DWORD Stencil);
HRESULT Core_SetTransform(D3DTRANSFORMSTATETYPE State,CONST D3DMATRIX* pMatrix);
HRESULT Core_SetViewport(CONST D3DVIEWPORT9* pViewport);
HRESULT Core_SetRenderState(D3DRENDERSTATETYPE State,DWORD Value);
HRESULT Core_GetRenderState(D3DRENDERSTATETYPE State,DWORD* pValue);
HRESULT Core_SetTexture(DWORD Stage,IDirect3DBaseTexture9* pTexture);
HRESULT Core_SetTextureStageState(DWORD Stage,D3DTEXTURESTAGESTATETYPE Type,DWORD Value);
HRESULT Core_SetSamplerState(DWORD Sampler,D3DSAMPLERSTATETYPE Type,DWORD Value);
HRESULT Core_DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType,UINT StartVertex,UINT PrimitiveCount);
HRESULT Core_DrawIndexedPrimitive(D3DPRIMITIVETYPE a0,INT BaseVertexIndex,UINT MinVertexIndex,UINT NumVertices,UINT startIndex,UINT primCount);
HRESULT Core_DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType,UINT PrimitiveCount,CONST void* pVertexStreamZeroData,UINT VertexStreamZeroStride);
HRESULT Core_SetFVF(DWORD FVF);
HRESULT Core_SetStreamSource(UINT StreamNumber,IDirect3DVertexBuffer9* pStreamData,UINT OffsetInBytes,UINT Stride);
HRESULT Core_SetIndices(IDirect3DIndexBuffer9* pIndexData);
