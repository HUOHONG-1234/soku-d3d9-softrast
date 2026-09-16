/* 自动生成, 勿手改: tools/gen_stubs.py */
#pragma once
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* pInitializeFunction) override { LogStub("RegisterSoftwareDevice"); return S_OK; }
    UINT STDMETHODCALLTYPE GetAdapterCount() override { return H9_GetAdapterCount(); }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT Adapter,DWORD Flags,D3DADAPTER_IDENTIFIER9* pIdentifier) override { return H9_GetAdapterIdentifier(Adapter, Flags, pIdentifier); }
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter,D3DFORMAT Format) override { return 1; }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter,D3DFORMAT Format,UINT Mode,D3DDISPLAYMODE* pMode) override { LogStub("EnumAdapterModes"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT Adapter,D3DDISPLAYMODE* pMode) override { return H9_GetAdapterDisplayMode(Adapter, pMode); }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT Adapter,D3DDEVTYPE DevType,D3DFORMAT AdapterFormat,D3DFORMAT BackBufferFormat,BOOL bWindowed) override { LogStub("CheckDeviceType"); return S_OK; }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT Adapter,D3DDEVTYPE DeviceType,D3DFORMAT AdapterFormat,DWORD Usage,D3DRESOURCETYPE RType,D3DFORMAT CheckFormat) override { LogStub("CheckDeviceFormat"); return S_OK; }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT Adapter,D3DDEVTYPE DeviceType,D3DFORMAT SurfaceFormat,BOOL Windowed,D3DMULTISAMPLE_TYPE MultiSampleType,DWORD* pQualityLevels) override { LogStub("CheckDeviceMultiSampleType"); return S_OK; }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT Adapter,D3DDEVTYPE DeviceType,D3DFORMAT AdapterFormat,D3DFORMAT RenderTargetFormat,D3DFORMAT DepthStencilFormat) override { LogStub("CheckDepthStencilMatch"); return S_OK; }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT Adapter,D3DDEVTYPE DeviceType,D3DFORMAT SourceFormat,D3DFORMAT TargetFormat) override { LogStub("CheckDeviceFormatConversion"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter,D3DDEVTYPE DeviceType,D3DCAPS9* pCaps) override { return H9_GetDeviceCaps(Adapter, DeviceType, pCaps); }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) override { return H9_GetAdapterMonitor(Adapter); }
