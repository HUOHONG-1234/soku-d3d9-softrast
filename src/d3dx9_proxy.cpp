/*
 * d3dx9_proxy.cpp -- d3dx9_33.dll proxy
 *
 * Why it exists:
 *   th123.exe imports exactly four d3dx9 functions:
 *     D3DXCreateTexture      texture creation
 *     D3DXCreateEffect       shader effect
 *     D3DXVec2Hermite / D3DXVec2Normalize   (plain math)
 *   The real D3DXCreateTexture never reaches device->CreateTexture, so the
 *   proxy intercepts it and calls the device directly.
 *
 * No .def re-export table: MSVC does not support cross-module forwarding
 * ("name = otherdll.name"), and only four functions need to be handled.
 *
 * Deployment:
 *   d3dx9_33.dll  = this proxy (MIT)
 *   The real D3DX is loaded from the system directory at runtime; it is
 *   NOT bundled.  Install the DirectX 9.0c runtime if it is missing.
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
static char    g_dllDir[MAX_PATH] = "";   /* directory containing this DLL */
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

/* Locate the real D3DX.  Must not be loaded by the name "d3dx9_33.dll":
   inside the game directory that name resolves to this proxy itself.
   The system copy (SysWOW64 for a 32-bit process) is used first, so the
   redistributable need not be shipped alongside this proxy.

   g_dllDir is an optional override directory (d3dx9_real.dll), kept for
   hosting scenarios where the runtime lives elsewhere. */
static HMODULE RealMod(void)
{
    if (g_real) return g_real;
    if (g_dllDir[0]) {
        char p[MAX_PATH];
        sprintf_s(p, MAX_PATH, "%sd3dx9_real.dll", g_dllDir);
        g_real = LoadLibraryA(p);
        if (g_real) LogF("[d3dx] real <- %s  (h=%p)", p, (void *)g_real);
    }
    if (!g_real) {
        char sys[MAX_PATH], p[MAX_PATH];
        if (GetSystemDirectoryA(sys, MAX_PATH)) {
            sprintf_s(p, MAX_PATH, "%s\\d3dx9_33.dll", sys);
            g_real = LoadLibraryA(p);
            LogF("[d3dx] real <- %s  (h=%p)", p, (void *)g_real);
        }
    }
    if (!g_real)
        LogF("[d3dx] [!] d3dx9_33.dll not found in the system directory. "
             "Install the DirectX 9.0c runtime.");
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
   The real D3DXCreateTexture returns D3DERR_INVALIDCALL for every size
   (measured at 640x78, 128x128 and 32x32) without ever reaching
   device->CreateTexture, so its own validation rejects the request before
   the device is consulted.  That validation is opaque to us and irrelevant
   here, since our textures live in CPU memory.
   So: skip the D3DX validation and call device->CreateTexture directly;
   fall back to the real D3DX only if that fails. */
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
        LogF("[d3dx] #%d   direct call failed, real D3DX -> hr=0x%08X tex=%p",
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
        GetModuleFileNameA(hSelf, g_dllDir, MAX_PATH);
        q = strrchr(g_dllDir, '\\');
        if (q) q[1] = 0;
        {
            char p[MAX_PATH];
            sprintf_s(p, MAX_PATH, "%sd3dx_log.txt", g_dllDir);
            g_log = fopen(p, "w");
        }
        LogF("[d3dx] proxy loaded (d3dx9_33 -> system D3DX)");
    }
    return TRUE;
}
