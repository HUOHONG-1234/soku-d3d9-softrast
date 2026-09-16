/*
 * d3dx9_proxy.cpp —— d3dx9_33.dll 代理 (只包 4 个函数)
 *
 * 为什么要它:
 *   th123.exe 导入表实测只用 4 个 d3dx9 函数 ——
 *     D3DXCreateTexture   ← 纹理正门
 *     D3DXCreateEffect    ← 着色器效果 (对应之前看到的 CreatePixelShader 调用)
 *     D3DXVec2Hermite / D3DXVec2Normalize  (纯数学, 不关心)
 *   但 device->CreateTexture 一次都没被调 -> 纹理在 D3DX 内部就断了。
 *   架这层网把入参和返回码记下来, 一次问死"断在哪"。
 *
 * 不用 .def 转发: MSVC 不支持 "名 = 其它dll.名" 那种跨模块转发,
 * 而且我们只需要 4 个, 直接手写转发更省事。
 *
 * 部署:
 *   d3dx9_33.dll   = 本代理
 *   d3dx9_real.dll = 系统 d3dx9_33.dll 的改名副本
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static FILE   *g_log  = NULL;
static HMODULE g_real = NULL;
static char    g_dir[MAX_PATH] = "";
static int     g_nTex = 0, g_nFx = 0;

static void LogF(const char *fmt, ...)
{
    va_list ap;
    if (!g_log) return;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* 载入真身。故意用【改名副本】+ 绝对路径:
   LoadLibraryA("d3dx9_33.dll") 会命中我们自己 (dll 代理经典陷阱)。 */
static HMODULE RealMod(void)
{
    if (g_real) return g_real;
    if (g_dir[0]) {
        char p[MAX_PATH];
        sprintf_s(p, MAX_PATH, "%sd3dx9_real.dll", g_dir);
        g_real = LoadLibraryA(p);
        LogF("[d3dx] 真身 <- %s  (h=%p)", p, (void *)g_real);
    }
    if (!g_real) {
        g_real = LoadLibraryA("d3dx9_real.dll");
        LogF("[d3dx] 真身 <- 搜索路径  (h=%p)", (void *)g_real);
    }
    return g_real;
}

static FARPROC Sym(const char *name)
{
    HMODULE m = RealMod();
    FARPROC p = m ? GetProcAddress(m, name) : NULL;
    if (!p) LogF("[d3dx] [!] 取不到 %s (err=%lu)", name, GetLastError());
    return p;
}

/* ---- D3DXCreateTexture ----
   真 D3DXCreateTexture 对所有尺寸都回 D3DERR_INVALIDCALL (实测 640x78/128x128/32x32 全挂),
   而 device->CreateTexture 一次没被调 —— 说明它卡在自己的前置校验里。
   那层校验对我们不透明, 且我们的纹理是 CPU 侧的, 根本不需要它。
   所以: 直接跳过 D3DX 校验, 自己调 device->CreateTexture; 失败再退回真 D3DX。 */
typedef HRESULT (WINAPI *PFN_CreateTexture)(IDirect3DDevice9 *, UINT, UINT, UINT,
                                            DWORD, D3DFORMAT, D3DPOOL,
                                            IDirect3DTexture9 **);

HRESULT WINAPI D3DXCreateTexture(IDirect3DDevice9 *device, UINT width, UINT height,
                                 UINT miplevels, DWORD usage, D3DFORMAT format,
                                 D3DPOOL pool, IDirect3DTexture9 **texture)
{
    static PFN_CreateTexture pReal = NULL;
    HRESULT hr = E_FAIL;

    g_nTex++;
    if (g_nTex <= 20) {
        LogF("[d3dx] #%d CreateTexture(dev=%p %ux%u mip=%u usage=0x%X fmt=%d pool=%d)",
             g_nTex, (void *)device, width, height, miplevels, usage, (int)format, (int)pool);
    }
    if (!texture) return D3DERR_INVALIDCALL;
    *texture = NULL;
    if (!device)  return D3DERR_INVALIDCALL;

    /* ① 直连 device —— 绕开 D3DX 的前置校验 */
    __try {
        hr = device->CreateTexture(width, height, miplevels, usage, format, pool,
                                   texture, NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogF("[d3dx] #%d   直连 CreateTexture 异常", g_nTex);
        hr = E_FAIL;
    }
    if (SUCCEEDED(hr) && *texture) {
        static int n1 = 0;
        if (n1 < 6) { LogF("[d3dx] #%d   直接建纹理 OK  %ux%u -> %p",
                           g_nTex, width, height, (void *)*texture); n1++; }
        return hr;
    }

    /* ② 直连失败 -> 退回真 D3DX (保持原行为, 不做更坏) */
    if (!pReal) pReal = (PFN_CreateTexture)Sym("D3DXCreateTexture");
    if (pReal) {
        IDirect3DTexture9 *out = NULL;
        __try {
            hr = pReal(device, width, height, miplevels, usage, format, pool, &out);
        } __except (EXCEPTION_EXECUTE_HANDLER) { hr = E_FAIL; }
        *texture = out;
    }
    if (g_nTex <= 20)
        LogF("[d3dx] #%d   直连失败, 真 D3DX -> hr=0x%08X tex=%p",
             g_nTex, (unsigned)hr, (void *)*texture);
    return hr;
}

/* ---- D3DXCreateEffect ----
   HRESULT D3DXCreateEffect(LPDIRECT3DDEVICE9, const void *pSrcData, UINT len,
                            const D3DXMACRO*, LPD3DXINCLUDE, DWORD flags,
                            LPD3DXEFFECTPOOL, LPD3DXEFFECT*, LPD3DXBUFFER*) */
typedef HRESULT (WINAPI *PFN_CreateEffect)(IDirect3DDevice9 *, const void *, UINT,
                                           const void *, void *, DWORD, void *,
                                           void **, void **);

HRESULT WINAPI D3DXCreateEffect(IDirect3DDevice9 *device, const void *src, UINT len,
                                const void *defines, void *include, DWORD flags,
                                void *pool, void **effect, void **errors)
{
    static PFN_CreateEffect p = NULL;
    HRESULT hr;
    if (!p) p = (PFN_CreateEffect)Sym("D3DXCreateEffect");
    g_nFx++;
    if (g_nFx <= 10) {
        LogF("[d3dx] #%d CreateEffect(dev=%p len=%u flags=0x%X) 头部=\"%.48s\"",
             g_nFx, (void *)device, len, flags, src ? (const char *)src : "(null)");
    }
    if (!p) return E_FAIL;
    __try { hr = p(device, src, len, defines, include, flags, pool, effect, errors); }
    __except (EXCEPTION_EXECUTE_HANDLER) { LogF("[d3dx]   !! 异常"); return E_FAIL; }
    if (g_nFx <= 10) LogF("[d3dx] #%d   -> hr=0x%08X fx=%p", g_nFx, (unsigned)hr,
                          effect ? *effect : NULL);
    return hr;
}

/* ---- 纯数学, 只转发 ---- */
typedef void * (WINAPI *PFN_V2Hermite)(void *, const void *, const void *,
                                       const void *, const void *, float);
typedef void * (WINAPI *PFN_V2Normalize)(void *, const void *);

void * WINAPI D3DXVec2Hermite(void *pOut, const void *pV1, const void *pT1,
                              const void *pV2, const void *pT2, float s)
{
    static PFN_V2Hermite p = NULL;
    if (!p) p = (PFN_V2Hermite)Sym("D3DXVec2Hermite");
    return p ? p(pOut, pV1, pT1, pV2, pT2, s) : NULL;
}

void * WINAPI D3DXVec2Normalize(void *pOut, const void *pV)
{
    static PFN_V2Normalize p = NULL;
    if (!p) p = (PFN_V2Normalize)Sym("D3DXVec2Normalize");
    return p ? p(pOut, pV) : NULL;
}

BOOL WINAPI DllMain(HMODULE hSelf, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        char *q;
        DisableThreadLibraryCalls(hSelf);
        GetModuleFileNameA(hSelf, g_dir, MAX_PATH);
        q = strrchr(g_dir, '\\');
        if (q) q[1] = 0;
        {
            char p[MAX_PATH];
            sprintf_s(p, MAX_PATH, "%sd3dx_log.txt", g_dir);
            g_log = fopen(p, "w");
        }
        LogF("[d3dx] 代理已加载 (d3dx9_33 -> d3dx9_real)");
    }
    return TRUE;
}
