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
static unsigned int     *O_down  = NULL;   /* 下行采样 / FXAA 暂存 (O_w x O_h) */

/* ---- 抗锯齿模式 (运行时由 soku_shim.ini 的 AAMode= 决定) ----
   0 = OFF   直接 1:1 输出
   1 = SSAA  超采样 SR_ss 倍后盒式均值缩回 (SR_ss 同时被设置)
   2 = FXAA  在输出尺寸上做亮度边缘定向混合 (SR_ss 强制 1)
   AA 与 SSAA 互斥: FXAA 优先, 选了 FXAA 就忽略超采样倍率. */
#define O_AA_OFF   0
#define O_AA_SSAA  1
#define O_AA_FXAA  2
static int   O_aaMode = O_AA_OFF;
static float O_aaStrength = 0.60f;   /* FXAA 混合强度 0..1 */
static void  O_AllocAux(void);        /* 前置声明: O_CreateDevice 在定义前就要用 */
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
        /* 首选：系统 d3d9。

           这是软件光栅器，它要的是一个能把最终帧推上屏的真 D3D9 —— 不是翻译层。
           旧顺序把 dxvk 排第一，RealD3D9= 空着时就被 DXVK 截胡
           （实测日志: 真 d3d9 <- backends\dxvk\dxvk_d3d9.dll，随后崩溃）。

           想固定用别的后端，就写 RealD3D9=<绝对路径>。 */
        GetSystemDirectoryA(sysdir, MAX_PATH);
        strcat_s(sysdir, MAX_PATH, "\\d3d9.dll");
        O_mod = LoadLibraryA(sysdir);
        if (O_mod) LogF("[out] 真 d3d9 <- %s (系统, 首选)", sysdir);
        else       LogF("[out] [!] 系统 d3d9 载不到: %s", sysdir);
    }

    if (!O_mod) {
        /* 其次才是用户自己放的后端（仅当系统 d3d9 也不可用时）*/
        char dir[MAX_PATH];
        GetModuleFileNameA(NULL, dir, MAX_PATH);
        p = strrchr(dir, '\\');
        if (p) p[1] = 0;

        static const char *cand[] = {
            "backends\\dgvoodoo2\\dgvoodoo_d3d9.dll",
            "backends\\dxvk\\dxvk_d3d9.dll",
            "backends\\wined3d\\wined3d_d3d9.dll",
            "dgvoodoo_d3d9.dll",
            "dxvk_d3d9.dll",
        };
        int i;
        for (i = 0; i < (int)(sizeof(cand) / sizeof(cand[0])); i++) {
            char full[MAX_PATH];
            sprintf_s(full, MAX_PATH, "%s%s", dir, cand[i]);
            if (GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES) continue;
            O_mod = LoadLibraryA(full);
            if (O_mod) { LogF("[out] 真 d3d9 <- %s (后端回退)", full); break; }
        }
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
    /* 我们的设备只用来贴最终帧, 越简单越好.
       不支持的字段原样透传会招 D3DERR_INVALIDCALL (0x8876086C).
       实测致命值: MultiSampleType=NONMASKABLE, PresentationInterval=TWO. */
    LogF("[out] 游戏 pp 原始字段: msType=0x%08X msQ=%u flags=0x%08X interval=0x%08X depth=%d",
         (unsigned)pp->MultiSampleType, (unsigned)pp->MultiSampleQuality,
         (unsigned)pp->Flags, (unsigned)pp->PresentationInterval,
         pp->EnableAutoDepthStencil ? 1 : 0);
    pp2.SwapEffect       = D3DSWAPEFFECT_DISCARD;
    pp2.EnableAutoDepthStencil = FALSE;
    pp2.AutoDepthStencilFormat = D3DFMT_UNKNOWN;
    pp2.MultiSampleType  = D3DMULTISAMPLE_NONE;   /* 我们自己做下行采样, 不要 MSAA */
    pp2.MultiSampleQuality = 0;
    pp2.Flags            = 0;                     /* LOCKABLE_BACKBUFFER 等一律不要 */
    if (pp2.PresentationInterval == D3DPRESENT_INTERVAL_TWO ||
        pp2.PresentationInterval == D3DPRESENT_INTERVAL_THREE ||
        pp2.PresentationInterval == D3DPRESENT_INTERVAL_FOUR) {
        pp2.PresentationInterval = D3DPRESENT_INTERVAL_ONE;   /* 降到单缓冲等垂直同步 */
    }
    pp2.BackBufferFormat = (pp2.BackBufferFormat == D3DFMT_UNKNOWN)
                           ? D3DFMT_X8R8G8B8 : pp2.BackBufferFormat;
    pp2.BackBufferCount  = (pp2.BackBufferCount < 1) ? 1 : pp2.BackBufferCount;
    pp2.hDeviceWindow    = hwnd;                  /* 确保有输出窗口 */
    if (pp2.BackBufferWidth == 0 || pp2.BackBufferHeight == 0) {
        pp2.BackBufferWidth = 640; pp2.BackBufferHeight = 480;
    }

    /* 顶点处理标志互斥: HW / SW / MIXED 只能选一个.
       原来无条件 OR 上 SOFTWARE —— 游戏若给 HW 就成 HW|SW,
       原生 d3d9 直接返 D3DERR_INVALIDCALL (0x8876086C).
       DXVK 对此宽容, 所以拿掉 DXVK 后才暴露.
       我们的设备只用来贴最终帧, 顶点处理模式无所谓 ->
       游戏给什么就用什么, 游戏没给才补 SOFTWARE. */
    {
        const DWORD kVP = D3DCREATE_HARDWARE_VERTEXPROCESSING
                        | D3DCREATE_SOFTWARE_VERTEXPROCESSING
                        | D3DCREATE_MIXED_VERTEXPROCESSING;
        if (!(Behavior & kVP)) Behavior |= D3DCREATE_SOFTWARE_VERTEXPROCESSING;
        LogF("[out] Behavior=0x%08X  顶点处理位=0x%08X  (%s)",
             (unsigned)Behavior, (unsigned)(Behavior & kVP),
             (Behavior & D3DCREATE_HARDWARE_VERTEXPROCESSING) ? "HW" :
             (Behavior & D3DCREATE_MIXED_VERTEXPROCESSING)    ? "MIXED" : "SW");
    }

    hr = O_d3d->CreateDevice(Adapter, Type, hwnd, Behavior, &pp2, &O_dev);
    if (FAILED(hr) || !O_dev) {
        LogF("[out] 真设备创建失败 hr=0x%08X, 重建 IDirect3D9 后改试 SW+HAL", (unsigned)hr);
        /* 换个干净的 IDirect3D9 再试, 避免前一次失败残留的状态 */
        if (O_d3d) { O_d3d->Release(); O_d3d = NULL; }
        O_dev = NULL;
        O_d3d = pCreate(D3D_SDK_VERSION);
        if (O_d3d) {
            hr = O_d3d->CreateDevice(Adapter, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                     &pp2, &O_dev);
        } else {
            hr = E_FAIL;
        }
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
    /* AA 暂存缓冲也在启动路径分配 —— 启动走 O_CreateDevice, 不经过 O_Reset.
       漏了它出场会因无缓冲跳过 (黑屏). */
    O_AllocAux();
    SR_Reset((int)O_w * SR_ss, (int)O_h * SR_ss);
    LogF("[out] *** D3D9 输出端就绪: %ux%u fmt=%d (光栅 %dx%d, SSAA x%d) ***",
         O_w, O_h, (int)O_fmt, SR_w, SR_h, SR_ss);
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

/* ---------------- FXAA (简化版, 真边缘定向混合) ----------------
   思路同 FXAA: 用亮度差检测边缘, 沿边缘方向做定向混合而不是盲目模糊.
   在【输出尺寸】上跑 (用前需把超采样结果先降到 O_w x O_h, 或 SR_ss=1).
   和 SSAA 的区别: SSAA 靠多采样得到真实覆盖率; FXAA 是后处理, 只柔化锯齿,
   代价是细节略糊 —— 但对 2D 精灵的斜边确实有效, 且比 SSAA 便宜很多. */
#define O_LUMA(c) ( (float)(((c) >> 16) & 0xFF) * 0.299f \
                  + (float)(((c) >>  8) & 0xFF) * 0.587f \
                  + (float)( (c)        & 0xFF) * 0.114f )

static void O_FXAA(unsigned int *dst, const unsigned int *src, int w, int h, float strength)
{
    int x, y;
    const float k = (strength < 0.0f) ? 0.0f : (strength > 1.0f ? 1.0f : strength);

    if (!dst || !src || w < 3 || h < 3) return;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            unsigned int c = src[(size_t)y * w + x];

            /* 边界直接拷贝 */
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1) {
                dst[(size_t)y * w + x] = c;
                continue;
            }

            {
                const unsigned int *row = src + (size_t)y * w;
                float lm = O_LUMA(c);
                float ln = O_LUMA(row[-(int)w]);
                float ls = O_LUMA(row[ (int)w]);
                float le = O_LUMA(row[1]);
                float lw = O_LUMA(row[-1]);
                float lmin = lm, lmax = lm;
                float edgeH, edgeV, amt;
                unsigned int r, g, b, a;

                if (ln < lmin) lmin = ln;  if (ln > lmax) lmax = ln;
                if (ls < lmin) lmin = ls;  if (ls > lmax) lmax = ls;
                if (le < lmin) lmin = le;  if (le > lmax) lmax = le;
                if (lw < lmin) lmin = lw;  if (lw > lmax) lmax = lw;

                /* 对比度阈值: 平坦区域不动, 否则会把噪点也揉糊 */
                if (lmax - lmin < 12.0f) { dst[(size_t)y * w + x] = c; continue; }

                edgeH = (le > lw) ? (le - lw) : (lw - le);   /* 垂直边缘强度 */
                edgeV = (ln > ls) ? (ln - ls) : (ls - ln);   /* 水平边缘强度 */

                /* 边缘越强混得越多, 再乘用户强度 */
                amt = (lmax - lmin) * (1.0f / 255.0f);
                amt = amt * amt * k * 0.5f;

                if (edgeH > edgeV) {
                    unsigned int cl = row[-1], cr = row[1];
                    float f = 1.0f - amt;
                    r = (unsigned int)(((c >> 16) & 0xFF) * f + (((cl >> 16) & 0xFF) + ((cr >> 16) & 0xFF)) * 0.5f * amt);
                    g = (unsigned int)(((c >>  8) & 0xFF) * f + (((cl >>  8) & 0xFF) + ((cr >>  8) & 0xFF)) * 0.5f * amt);
                    b = (unsigned int)(( c        & 0xFF) * f + (( cl        & 0xFF) + ( cr        & 0xFF)) * 0.5f * amt);
                    a = (c >> 24) & 0xFF;
                } else {
                    unsigned int cu = row[-(int)w], cd = row[(int)w];
                    float f = 1.0f - amt;
                    r = (unsigned int)(((c >> 16) & 0xFF) * f + (((cu >> 16) & 0xFF) + ((cd >> 16) & 0xFF)) * 0.5f * amt);
                    g = (unsigned int)(((c >>  8) & 0xFF) * f + (((cu >>  8) & 0xFF) + ((cd >>  8) & 0xFF)) * 0.5f * amt);
                    b = (unsigned int)(( c        & 0xFF) * f + (( cu        & 0xFF) + ( cd        & 0xFF)) * 0.5f * amt);
                    a = (c >> 24) & 0xFF;
                }
                dst[(size_t)y * w + x] = (a << 24) | ((r & 0xFF) << 16)
                                       | ((g & 0xFF) << 8) | (b & 0xFF);
            }
        }
    }
}

/* ---- SSAA 盒式下行采样: SR_buf (sw x sh) -> dst (dw x dh) ----
   每输出像素 = ss x ss 个输入像素的通道均值.
   这就是真抗锯齿: 边缘按覆盖率自然过渡, 不是 FXAA 那种模糊. */
static void O_Downsample(unsigned int *dst, const unsigned int *src,
                         int sw, int sh, int dw, int dh, int ss)
{
    int x, y, kx, ky;
    int area = ss * ss;
    if (ss <= 1) { memcpy(dst, src, (size_t)dw * dh * 4); return; }

    for (y = 0; y < dh; y++) {
        int sy0 = y * ss;
        if (sy0 + ss > sh) sy0 = sh - ss;
        for (x = 0; x < dw; x++) {
            int sx0 = x * ss;
            unsigned int r = 0, g = 0, b = 0, a = 0;
            if (sx0 + ss > sw) sx0 = sw - ss;
            for (ky = 0; ky < ss; ky++) {
                const unsigned int *row = src + (size_t)(sy0 + ky) * sw + sx0;
                for (kx = 0; kx < ss; kx++) {
                    unsigned int c = row[kx];
                    r += (c >> 16) & 0xFF;
                    g += (c >>  8) & 0xFF;
                    b +=  c        & 0xFF;
                    a += (c >> 24) & 0xFF;
                }
            }
            dst[(size_t)y * dw + x] = (((a / area) & 0xFF) << 24)
                                    | (((r / area) & 0xFF) << 16)
                                    | (((g / area) & 0xFF) <<  8)
                                    |  ((b / area) & 0xFF);
        }
    }
}

/* 分配下行/FXAA 暂存缓冲 (O_w x O_h). 两个创建路径都调它. */
static void O_AllocAux(void)
{
    if (O_down) { free(O_down); O_down = NULL; }
    if (O_w <= 0 || O_h <= 0) return;

    if (O_aaMode == O_AA_FXAA || SR_ss > 1) {
        O_down = (unsigned int *)malloc((size_t)O_w * O_h * 4);
        if (!O_down) {
            LogF("[aa] 暂存缓冲分配失败 (%ux%u) -> 降级为 1:1 输出", O_w, O_h);
            if (O_aaMode == O_AA_FXAA) O_aaMode = O_AA_OFF;
            SR_SetSuperSample(1);
        }
    }
}

/* AA 调度: 按 O_aaMode 选路径, 最后统一交给 O_PresentSrc 上屏. */
static int O_PresentAA(void)
{
    /* --- FXAA: 在输出尺寸上做, SR_buf 必为 O_w x O_h (SR_ss 已强制 1) --- */
    if (O_aaMode == O_AA_FXAA && O_down && SR_buf &&
        SR_w == (int)O_w && SR_h == (int)O_h) {
        long long tA = QpcNow();
        O_FXAA(O_down, SR_buf, SR_w, SR_h, O_aaStrength);
        g_ticksConvert += QpcNow() - tA;
        return O_PresentSrc(O_down, (int)O_w, (int)O_h);
    }

    /* --- SSAA: 超采样缓冲盒式缩回 --- */
    if (SR_ss > 1 && SR_buf && O_w > 0 && O_h > 0 &&
        (UINT)SR_w > O_w && (UINT)SR_h > O_h) {
        /* O_down 在 O_AllocAux 里分配. 没拿到就不能 1:1 上交
           (SR_buf 是超采样尺寸, 直接上交会裁成左上角一块 = 放大错图) */
        if (!O_down) {
            static int warned = 0;
            if (!warned) { LogF("[ssaa] !!! 无下行采样缓冲, 跳过本帧输出"); warned = 1; }
            return 0;
        }
        {
            long long tA = QpcNow();
            O_Downsample(O_down, SR_buf, SR_w, SR_h, (int)O_w, (int)O_h, SR_ss);
            g_ticksConvert += QpcNow() - tA;
        }
        return O_PresentSrc(O_down, (int)O_w, (int)O_h);
    }

    /* --- OFF: 直出 --- */
    return O_PresentSrc(NULL, SR_w, SR_h);
}

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
    /* AA 暂存缓冲随尺寸重建 (在这里分配, 失败可在这里发现) */
    O_AllocAux();
    O_ready = 1;
    SR_Reset((int)O_w * SR_ss, (int)O_h * SR_ss);
    LogF("[out] *** Reset 完成: %ux%u fmt=%d windowed=%d (光栅 %dx%d, SSAA x%d) ***",
         O_w, O_h, (int)O_fmt, pp->Windowed ? 1 : 0, SR_w, SR_h, SR_ss);
    return 1;
}

#endif /* D3D9OUT_H */
