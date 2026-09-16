/*
 * d3d9out.h —— CPU 光栅的 D3D9 输出端
 *
 * 设计:
 *   1. 载入【真正的】系统 d3d9 (绝对路径, 避免 LoadLibrary("d3d9.dll")
 *      返回我们自己的模块句柄 —— dll 代理的经典陷阱)
 *   2. 用游戏给的参数建一个真设备
 *   3. 建一张与 backbuffer 同格式的 SYSTEMMEM 离屏面
 *   4. 每帧: LockRect -> 把 SR_buf 打包写进去 -> UnlockRect
 *            -> UpdateSurface 到 backbuffer -> Present
 *   5. 游戏自己的绘制调用全被 shim 截住走 CPU 光栅, 真设备只干"贴图+翻页"
 *
 */
#ifndef D3D9OUT_H
#define D3D9OUT_H

typedef IDirect3D9 * (WINAPI *PFN_D3D9CREATE)(UINT);

static HMODULE           O_mod   = NULL;   /* 真 d3d9 模块 */
static IDirect3D9       *O_d3d   = NULL;
static IDirect3DDevice9 *O_dev   = NULL;
static IDirect3DSurface9*O_off   = NULL;   /* SYSTEMMEM 离屏面 */
static D3DFORMAT         O_fmt   = D3DFMT_X8R8G8B8;
static UINT              O_w = 0, O_h = 0;
static int               O_ready = 0;
static int               O_frames = 0;
static int               O_failed = 0;

/* 选真 d3d9 的路径:
   1) soku_shim.ini [soku_shim] RealD3D9=<绝对路径或文件名>
   2) <游戏目录>\backends\<选中的后端>\*.dll
   3) 系统 d3d9 (GetSystemDirectory —— WOW64 进程下自动是 SysWOW64) */
static void O_LoadReal(void)
{
    char ini[MAX_PATH], buf[MAX_PATH], sysdir[MAX_PATH];
    char *p;

    if (O_mod) return;

    GetModuleFileNameA(NULL, ini, MAX_PATH);
    p = strrchr(ini, '\\');
    if (p) p[1] = 0;
    strcat_s(ini, MAX_PATH, "soku_shim.ini");

    GetPrivateProfileStringA("soku_shim", "RealD3D9", "", buf, MAX_PATH, ini);
    if (buf[0]) {
        O_mod = LoadLibraryA(buf);
        if (O_mod) { LogF("[out] 真 d3d9 <- %s (ini)", buf); }
        else       { LogF("[out] ini 里的路径载入失败: %s", buf); }
    }

    if (!O_mod) {
        /* 游戏目录下的后端候选 */
        char dir[MAX_PATH];
        GetModuleFileNameA(NULL, dir, MAX_PATH);
        p = strrchr(dir, '\\');
        if (p) p[1] = 0;

        static const char *cand[] = {
            "backends\\dxvk\\dxvk_d3d9.dll",
            "backends\\dgvoodoo2\\dgvoodoo_d3d9.dll",
            "backends\\wined3d\\wined3d_d3d9.dll",
            "dxvk_d3d9.dll",
            "dgvoodoo_d3d9.dll",
        };
        int i;
        for (i = 0; i < (int)(sizeof(cand) / sizeof(cand[0])); i++) {
            char full[MAX_PATH];
            sprintf_s(full, MAX_PATH, "%s%s", dir, cand[i]);
            if (GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES) continue;
            O_mod = LoadLibraryA(full);
            if (O_mod) { LogF("[out] 真 d3d9 <- %s", full); break; }
        }
    }

    if (!O_mod) {
        GetSystemDirectoryA(sysdir, MAX_PATH);
        strcat_s(sysdir, MAX_PATH, "\\d3d9.dll");
        O_mod = LoadLibraryA(sysdir);
        if (O_mod) LogF("[out] 真 d3d9 <- %s (系统)", sysdir);
        else       LogF("[out] [!] 连系统 d3d9 都载不到, err=%lu", GetLastError());
    }
}

/* 建真设备 + 离屏面。返回 1 = 可用。 */
static int O_CreateDevice(UINT Adapter, D3DDEVTYPE Type, HWND hwnd,
                          DWORD Behavior, D3DPRESENT_PARAMETERS *pp)
{
    PFN_D3D9CREATE pCreate;
    D3DPRESENT_PARAMETERS pp2;
    HRESULT hr;

    if (O_ready) return 1;
    if (O_failed) return 0;

    O_LoadReal();
    if (!O_mod) { O_failed = 1; return 0; }

    pCreate = (PFN_D3D9CREATE)GetProcAddress(O_mod, "Direct3DCreate9");
    if (!pCreate) { LogF("[out] 真 d3d9 无 Direct3DCreate9"); O_failed = 1; return 0; }

    O_d3d = pCreate(D3D_SDK_VERSION);
    if (!O_d3d) { LogF("[out] Direct3DCreate9 返回空"); O_failed = 1; return 0; }

    if (!pp) { LogF("[out] 没拿到 PRESENT_PARAMETERS, 无法建设备"); O_failed = 1; return 0; }

    pp2 = *pp;
    /* 我们的设备只用来贴最终帧, 越简单越好 */
    pp2.SwapEffect       = D3DSWAPEFFECT_DISCARD;
    pp2.EnableAutoDepthStencil = FALSE;
    pp2.BackBufferFormat = (pp2.BackBufferFormat == D3DFMT_UNKNOWN)
                           ? D3DFMT_X8R8G8B8 : pp2.BackBufferFormat;
    pp2.BackBufferCount  = (pp2.BackBufferCount < 1) ? 1 : pp2.BackBufferCount;
    if (pp2.BackBufferWidth == 0 || pp2.BackBufferHeight == 0) {
        pp2.BackBufferWidth = 640; pp2.BackBufferHeight = 480;
    }

    hr = O_d3d->CreateDevice(Adapter, Type, hwnd,
                             Behavior | D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                             &pp2, &O_dev);
    if (FAILED(hr) || !O_dev) {
        LogF("[out] 真设备创建失败 hr=0x%08X, 改试 HAL", (unsigned)hr);
        hr = O_d3d->CreateDevice(Adapter, D3DDEVTYPE_HAL, hwnd,
                                 D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                 &pp2, &O_dev);
    }
    if (FAILED(hr) || !O_dev) {
        LogF("[out] [!] device creation failed hr=0x%08X", (unsigned)hr);
        O_failed = 1;
        return 0;
    }

    O_w = pp2.BackBufferWidth;
    O_h = pp2.BackBufferHeight;
    O_fmt = pp2.BackBufferFormat;

    hr = O_dev->CreateOffscreenPlainSurface(O_w, O_h, O_fmt,
                                            D3DPOOL_SYSTEMMEM, &O_off, NULL);
    if (FAILED(hr) || !O_off) {
        LogF("[out] [!] 离屏面创建失败 hr=0x%08X", (unsigned)hr);
        O_failed = 1;
        return 0;
    }

    O_ready = 1;
    LogF("[out] *** D3D9 输出端就绪: %ux%u fmt=%d ***", O_w, O_h, (int)O_fmt);
    return 1;
}

/* 把 src（w*h ARGB8888）打包并推上屏。返回 1 = 成功。
   src may be NULL to use the current SR_buf. */
static int O_PresentSrc(const unsigned int *src, int w, int h)
{
    D3DLOCKED_RECT lr;
    IDirect3DSurface9 *bb = NULL;
    HRESULT hr;
    UINT x, y;
    long long tA, tB;

    if (!src) src = SR_buf;
    if (!O_ready || !O_off || !src) return 0;
    if ((UINT)w != O_w || (UINT)h != O_h) w = (int)O_w, h = (int)O_h;
    if (w <= 0 || h <= 0) return 0;

    /* ---- ① Lock + 拷图 ---- */
    tA = QpcNow();
    hr = O_off->LockRect(&lr, NULL, D3DLOCK_DISCARD);
    if (FAILED(hr)) { if (O_frames < 3) LogF("[out] LockRect 失败 hr=0x%08X", (unsigned)hr); return 0; }

    for (y = 0; y < (UINT)h; y++) {
        const unsigned int *s = src + (size_t)y * w;
        unsigned char *dst = (unsigned char *)lr.pBits + (size_t)y * lr.Pitch;
        switch (O_fmt) {
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
            memcpy(dst, s, (size_t)w * 4);      /* 内存布局同为 0xAARRGGBB */
            break;
        case D3DFMT_R5G6B5:
            for (x = 0; x < (UINT)w; x++) {
                unsigned int c = s[x];
                ((unsigned short *)dst)[x] = (unsigned short)
                    ((((c >> 16) & 0xF8) << 8) | (((c >> 8) & 0xFC) << 3) | ((c & 0xF8) >> 3));
            }
            break;
        case D3DFMT_A1R5G5B5:
            for (x = 0; x < (UINT)w; x++) {
                unsigned int c = s[x];
                ((unsigned short *)dst)[x] = (unsigned short)
                    (((c >> 24) ? 0x8000 : 0) | (((c >> 16) & 0xF8) << 7)
                   | (((c >> 8) & 0xF8) << 2) | ((c & 0xF8) >> 3));
            }
            break;
        default:
            memcpy(dst, s, (size_t)w * 4);
            break;
        }
    }
    O_off->UnlockRect();
    tB = QpcNow();
    g_ticksLock += tB - tA;

    /* ---- ② Blit 到 backbuffer ---- */
    tA = QpcNow();
    hr = O_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (SUCCEEDED(hr) && bb) {
        hr = O_dev->UpdateSurface(O_off, NULL, bb, NULL);
        if (FAILED(hr)) {
            /* UpdateSurface 不接受时用 StretchRect 兜底 */
            hr = O_dev->StretchRect(O_off, NULL, bb, NULL, D3DTEXF_NONE);
            if (FAILED(hr) && O_frames < 3)
                LogF("[out] 贴图失败 hr=0x%08X (UpdateSurface+StretchRect 都不行)", (unsigned)hr);
        }
        bb->Release();
    }
    tB = QpcNow();
    g_ticksBlit += tB - tA;

    /* ---- ③ Present (DXVK 转 Vulkan) ---- */
    tA = QpcNow();
    hr = O_dev->Present(NULL, NULL, NULL, NULL);
    if (FAILED(hr) && O_frames < 3)
        LogF("[out] Present 失败 hr=0x%08X", (unsigned)hr);
    tB = QpcNow();
    g_ticksSwap += tB - tA;

    O_frames++;
    if (O_frames % 60 == 0) LogF("[out] 已输出 %d 帧 (CPU 光栅 -> D3D9)", O_frames);
    return 1;
}

static int O_Present(void) { return O_PresentSrc(NULL, SR_w, SR_h); }

/* 重建输出端。
   之前它是空桩(返回 S_OK 但什么都不做) -> 游戏以为切了, 实际还在窗口里。
   操开不了全屏。
   解决方案是把真设备的 Reset 转发出去, 然后按新尺寸/格式重建离屏面。 */
static int O_Reset(D3DPRESENT_PARAMETERS *pp)
{
    HRESULT hr;
    UINT nw, nh;
    D3DFORMAT nf;

    if (!O_ready || !O_dev || !pp) return 0;

    /* 先把已入队的绘制做完, 并放掉本帧持住的纹理 ——
       否则 Reset 后缓冲区尺寸变了, 未完成的命令会写越界 */
    SR_Flush();
    TexReleaseHeld();

    nw = pp->BackBufferWidth  ? pp->BackBufferWidth  : O_w;
    nh = pp->BackBufferHeight ? pp->BackBufferHeight : O_h;
    nf = (pp->BackBufferFormat == D3DFMT_UNKNOWN) ? O_fmt : pp->BackBufferFormat;

    if (O_off) { O_off->Release(); O_off = NULL; }
    O_ready = 0;

    hr = O_dev->Reset(pp);
    if (FAILED(hr)) {
        LogF("[out] Reset 失败 hr=0x%08X (%ux%u windowed=%d)",
             (unsigned)hr, nw, nh, pp->Windowed ? 1 : 0);
        return 0;   /* 设备处于丢失态, 让游戏自己处理 */
    }

    O_w = nw; O_h = nh; O_fmt = nf;

    hr = O_dev->CreateOffscreenPlainSurface(O_w, O_h, O_fmt,
                                            D3DPOOL_SYSTEMMEM, &O_off, NULL);
    if (FAILED(hr) || !O_off) {
        LogF("[out] Reset 后离屏面重建失败 hr=0x%08X", (unsigned)hr);
        return 0;
    }
    O_ready = 1;
    SR_Reset((int)O_w, (int)O_h);
    LogF("[out] *** Reset 完成: %ux%u fmt=%d windowed=%d ***",
         O_w, O_h, (int)O_fmt, pp->Windowed ? 1 : 0);
    return 1;
}

#endif /* D3D9OUT_H */
