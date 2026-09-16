/*
 * soku_loader —— 非想天则模组加载器 (d3d9.dll 代理)
 *
 * 原理: 游戏 exe 目录优先于系统目录被搜索, 放个同名 d3d9.dll 即替代系统那个。
 *      我们把调用转发给真正的系统 d3d9, 顺便扫描 modules/ 加载模组。
 *
 * 目标: th123_beta.exe (x86 / 32位)
 * 依赖: 纯用户态, 无内核驱动, 无测试签名。
 *
 * 模组 ABI (从 modules/*.dll 导出表实测, 所有模组一致):
 *     bool CheckVersion(const unsigned char hash[16]);
 *     bool Initialize(HMODULE hMyModule, HMODULE hParentModule);
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#pragma comment(lib, "advapi32.lib")

/* ---------------- 日志 ---------------- */
static FILE *g_log = NULL;

static void LogF(const char *fmt, ...)
{
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* ---------------- 计算 exe 的 MD5 ---------------- */
static bool Md5OfFile(const char *path, unsigned char out[16])
{
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    HANDLE f = INVALID_HANDLE_VALUE;
    bool ok = false;
    unsigned char buf[65536];
    DWORD got = 0;
    BYTE digest[16];
    DWORD dlen = sizeof(digest);

    if (!CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        LogF("[!] CryptAcquireContext failed err=%lu", GetLastError());
        return false;
    }
    if (!CryptCreateHash(prov, CALG_MD5, 0, 0, &hash)) {
        LogF("[!] CryptCreateHash failed err=%lu", GetLastError());
        CryptReleaseContext(prov, 0);
        return false;
    }

    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        LogF("[!] open %s failed err=%lu", path, GetLastError());
        CryptDestroyHash(hash); CryptReleaseContext(prov, 0);
        return false;
    }
    while (ReadFile(f, buf, sizeof(buf), &got, NULL) && got > 0) {
        if (!CryptHashData(hash, buf, got, 0)) { LogF("[!] CryptHashData failed"); break; }
    }
    CloseHandle(f);

    if (CryptGetHashParam(hash, HP_HASHVAL, digest, &dlen, 0)) {
        memcpy(out, digest, 16);
        ok = true;
    }
    CryptDestroyHash(hash);
    CryptReleaseContext(prov, 0);
    return ok;
}

static void HashToStr(const unsigned char h[16], char out[33])
{
    for (int i = 0; i < 16; i++) sprintf_s(out + i * 2, 3, "%02x", h[i]);
    out[32] = 0;
}

/* ---------------- 真正的系统 d3d9 ---------------- */
static HMODULE g_real = NULL;
typedef void *(__stdcall *PFN_Create9)(unsigned int);
static PFN_Create9 p_Create9 = NULL;

/* D3D9On12: 微软官方的 D3D9 -> D3D12 翻译层 */
typedef void *(__stdcall *PFN_Create9On12)(unsigned int, void *, unsigned int);
static PFN_Create9On12 p_Create9On12 = NULL;

static FARPROC Real(const char *name)
{
    return g_real ? GetProcAddress(g_real, name) : NULL;
}

static void LoadReal(void)
{
    char path[MAX_PATH];
    UINT n;

    if (g_real) return;

    /* 链式代理: 优先加载同目录的链目标 (DXVK 等翻译层), 没有再用系统 d3d9。
       链目标名由 soku_loader.ini 的 ChainDll 指定, 默认 dxvk_d3d9.dll */
    {
        char exe[MAX_PATH], dir[MAX_PATH];
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        strncpy_s(dir, MAX_PATH, exe, _TRUNCATE);
        { char *s = strrchr(dir, '\\'); if (s) *s = 0; }

        char cfg[MAX_PATH], chain[64] = "dxvk_d3d9.dll";
        sprintf_s(cfg, MAX_PATH, "%s\\soku_loader.ini", dir);
        GetPrivateProfileStringA("soku_loader", "ChainDll", "dxvk_d3d9.dll", chain, sizeof(chain), cfg);

        if (chain[0]) {
            sprintf_s(path, MAX_PATH, "%s\\%s", dir, chain);
            g_real = LoadLibraryA(path);
            if (g_real) {
                p_Create9 = (PFN_Create9)GetProcAddress(g_real, "Direct3DCreate9");
                p_Create9On12 = (PFN_Create9On12)GetProcAddress(g_real, "Direct3DCreate9On12");
                LogF("[+] 链式代理已接上: %s", path);
                LogF("    Create9 = %p   Create9On12 = %p", (void *)p_Create9, (void *)p_Create9On12);
                return;
            }
            LogF("[i] 链目标不可用 (%s), 回退系统 d3d9", path);
        }
    }

    /* 回退: 系统 d3d9 */
    n = GetSystemDirectoryA(path, MAX_PATH);          /* 32位进程 -> SysWOW64 */
    if (n == 0 || n + 12 >= MAX_PATH) { LogF("[!] GetSystemDirectory failed"); return; }
    strcat_s(path, MAX_PATH, "\\d3d9.dll");

    g_real = LoadLibraryA(path);
    if (!g_real) { LogF("[!] LoadLibrary(%s) err=%lu", path, GetLastError()); return; }

    p_Create9 = (PFN_Create9)GetProcAddress(g_real, "Direct3DCreate9");
    p_Create9On12 = (PFN_Create9On12)GetProcAddress(g_real, "Direct3DCreate9On12");
    LogF("[+] 系统 d3d9 已加载: %s", path);
    LogF("    Direct3DCreate9 = %p   Direct3DCreate9On12 = %p", (void *)p_Create9, (void *)p_Create9On12);
}

/* ---------------- 配置 ---------------- */
static int  g_verbose = 0;
static int  g_force = 0;      /* 1 = 无视 CheckVersion 失败仍然 Initialize */
static int  g_loadModules = 1; /* 0 = 只做 d3d9 转发, 不加载任何模组 (排错用) */
static int  g_useOn12 = 0;     /* 1 = 走 D3D9On12 (D3D9 -> D3D12) */
static int  g_loadEngine = 0;   /* 0 = 不自动加载 SokuEngine.dll (独立模式) */
static int  g_discover = 1;     /* 1 = 自动扫描 modules/ 目录, 不依赖任何 ini */

/* 默认排除表: 这些不是普通模组, 当普通模组加载会崩游戏
   (th123e 是 SokuEngine 的引导器, SokuEngine 是框架本体) */
static const char *kDefaultExclude[] = { "th123e.dll", "SokuEngine.dll" };
static const int   kDefaultExcludeCount = 2;
static char g_exclude[512] = "";   /* 额外排除, 逗号分隔, 来自 ini 的 Exclude= */

/* 判断文件名是否在排除表里 */
static int IsExcluded(const char *fname)
{
    int i;
    for (i = 0; i < kDefaultExcludeCount; i++)
        if (lstrcmpiA(fname, kDefaultExclude[i]) == 0) return 1;

    if (g_exclude[0]) {
        char tmp[512];
        char *tok, *ctx = NULL;
        strncpy_s(tmp, sizeof(tmp), g_exclude, _TRUNCATE);
        tok = strtok_s(tmp, ",; ", &ctx);
        while (tok) {
            if (lstrcmpiA(fname, tok) == 0) return 1;
            tok = strtok_s(NULL, ",; ", &ctx);
        }
    }
    return 0;
}

static void ReadCfg(const char *dir)
{
    char ini[MAX_PATH];
    sprintf_s(ini, MAX_PATH, "%s\\soku_loader.ini", dir);
    g_verbose = GetPrivateProfileIntA("soku_loader", "Verbose", 0, ini);
    g_force   = GetPrivateProfileIntA("soku_loader", "ForceLoad", 0, ini);
    g_loadModules = GetPrivateProfileIntA("soku_loader", "LoadModules", 1, ini);
    g_useOn12 = GetPrivateProfileIntA("soku_loader", "UseOn12", 0, ini);
    g_loadEngine = GetPrivateProfileIntA("soku_loader", "LoadEngine", 0, ini);
    g_discover = GetPrivateProfileIntA("soku_loader", "DiscoverModules", 1, ini);
    GetPrivateProfileStringA("soku_loader", "Exclude", "", g_exclude, sizeof(g_exclude), ini);
    if (GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES)
        LogF("[i] 配置: %s  Verbose=%d ForceLoad=%d LoadModules=%d", ini, g_verbose, g_force, g_loadModules);
}

/* ---------------- 模组加载 ---------------- */
typedef bool (*PFN_CheckVersion)(const unsigned char *);
typedef bool (*PFN_Initialize)(HMODULE, HMODULE);

static int g_loaded = 0;     /* 成功 Initialize */
static int g_failed = 0;     /* 加载或初始化真失败 */
static int g_missing = 0;    /* 文件不存在, 干净跳过 */
static int g_notmod = 0;     /* 缺 Initialize 导出, 不是模组 */
static bool g_ranOnce = false;   /* 防止游戏多次调用 Direct3DCreate9 时重复加载 */
static unsigned char g_exeMd5[16];   /* 供 JSON 路径复用 */

/* 把 Windows 错误码翻译成人话 (只处理常见那几个) */
static const char *ErrHint(DWORD e)
{
    switch (e) {
    case 126: return "找不到依赖模块 (缺 DLL 或文件不在)";
    case 193: return "不是有效的 Win32 DLL (位数不对?)";
    case 1260: return "被系统策略阻止加载";
    case 5:   return "拒绝访问";
    default:  return "";
    }
}

static void TryLoadModule(const char *dllRel, const unsigned char exeMd5[16])
{
    char fixed[MAX_PATH];
    HMODULE h;
    PFN_CheckVersion pCV;
    PFN_Initialize   pInit;

    strncpy_s(fixed, MAX_PATH, dllRel, _TRUNCATE);
    for (char *q = fixed; *q; ++q) if (*q == '/') *q = '\\';

    /* 1) 文件不存在 -> 干净跳过, 不当失败 (升级包常常只给了部分模块) */
    if (GetFileAttributesA(fixed) == INVALID_FILE_ATTRIBUTES) {
        LogF("  [跳过] %s  (文件不存在, 可能只装了升级包)", fixed);
        g_missing++;
        return;
    }

    LogF("  -> %s", fixed);

    h = LoadLibraryA(fixed);
    if (!h) {
        DWORD e = GetLastError();
        LogF("     [!] 加载失败 err=%lu  %s", e, ErrHint(e));
        g_failed++;
        return;
    }

    pCV   = (PFN_CheckVersion)GetProcAddress(h, "CheckVersion");
    pInit = (PFN_Initialize)  GetProcAddress(h, "Initialize");

    if (!pInit) {
        LogF("     [忽略] 无 Initialize 导出 —— 不是 SWRSToys 模组 (可能是引擎库)");
        g_notmod++;
        return;
    }

    if (pCV) {
        bool ok = false;
        __try { ok = pCV(exeMd5); }
        __except (EXCEPTION_EXECUTE_HANDLER) { LogF("     [!] CheckVersion 异常"); }

        char hs[33]; HashToStr(exeMd5, hs);
        LogF("     CheckVersion(md5=%s) -> %s", hs, ok ? "PASS" : "FAIL");
        if (!ok && !g_force) { LogF("     [!] 版本校验未通过, 跳过 (可设 ForceLoad=1 强制)"); g_failed++; return; }
        if (!ok && g_force)  { LogF("     [i] ForceLoad=1, 忽略校验结果继续"); }
    } else {
        LogF("     [i] 未导出 CheckVersion, 直接初始化");
    }

    {
        bool ok = false;
        __try { ok = pInit(h, NULL); }
        __except (EXCEPTION_EXECUTE_HANDLER) { LogF("     [!] Initialize 异常"); }
        LogF("     Initialize -> %s", ok ? "OK" : "FAIL");
        if (ok) g_loaded++; else g_failed++;
    }
}

/* ---- 自动发现模块 (独立模式, 不靠任何 ini) ----
   扫描 <dir>\modules\*\*.dll, 逐个试加载。
   TryLoadModule 内部会自己判断是不是合法模组。 */
static int DiscoverModules(const char *dir, const unsigned char exeMd5[16])
{
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd, fd2;
    HANDLE hf, hf2;
    int count = 0;

    sprintf_s(pat, MAX_PATH, "%s\\modules\\*", dir);
    hf = FindFirstFileA(pat, &fd);
    if (hf == INVALID_HANDLE_VALUE) return 0;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;

        sprintf_s(pat, MAX_PATH, "%s\\modules\\%s\\*.dll", dir, fd.cFileName);
        hf2 = FindFirstFileA(pat, &fd2);
        if (hf2 == INVALID_HANDLE_VALUE) continue;
        do {
            char rel[MAX_PATH];
            if (IsExcluded(fd2.cFileName)) {
                LogF("  [排除] %s  (框架/引导器, 不当模组加载)", fd2.cFileName);
                continue;
            }
            sprintf_s(rel, MAX_PATH, "modules/%s/%s", fd.cFileName, fd2.cFileName);
            TryLoadModule(rel, exeMd5);
            count++;
        } while (FindNextFileA(hf2, &fd2));
        FindClose(hf2);
    } while (FindNextFileA(hf, &fd));

    FindClose(hf);
    return count;
}

/* ---- ModLoaderSettings.json 支持 ----
   SokuModLoader 用的新格式, 形如:
   {"modules":{"modules/Autopunch/Autopunch.dll":{"enabled":true}, ...}}
   只提取 enabled=true 的条目。 */
static int LoadFromJson(const char *jsonPath, const unsigned char exeMd5[16])
{
    FILE *f;
    long sz;
    char *buf;
    const char *p;
    int count = 0;

    f = fopen(jsonPath, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) { fclose(f); return -1; }

    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return -1; }
    buf[sz] = 0;
    fclose(f);

    p = buf;
    while ((p = strstr(p, "\"modules/")) != NULL) {
        const char *kstart = p + 1;
        const char *kend = strchr(kstart, '"');
        const char *nextKey;
        const char *en;

        if (!kend) break;

        /* 这个 key 之后、下一个 key 之前, 是否有 "enabled":true */
        nextKey = strstr(kend, "\"modules/");
        en = strstr(kend, "\"enabled\"");
        if (en && (!nextKey || en < nextKey)) {
            const char *colon = strchr(en, ':');
            if (colon) {
                const char *v = colon + 1;
                while (*v == ' ' || *v == '\t') v++;
                if (strncmp(v, "true", 4) == 0) {
                    size_t klen = (size_t)(kend - kstart);
                    if (klen > 0 && klen < MAX_PATH) {
                        char rel[MAX_PATH];
                        memcpy(rel, kstart, klen);
                        rel[klen] = 0;
                        TryLoadModule(rel, exeMd5);
                        count++;
                    }
                }
            }
        }
        p = kend;
    }
    free(buf);
    return count;
}

static void LoadAll(void)
{
    char exe[MAX_PATH], dir[MAX_PATH], ini[MAX_PATH], buf[16384];
    char *p;
    unsigned char md5[16];
    DWORD n;

    n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (!n) { LogF("[!] GetModuleFileName 失败"); return; }
    strncpy_s(dir, MAX_PATH, exe, _TRUNCATE);
    { char *s = strrchr(dir, '\\'); if (s) *s = 0; }

    LogF("[i] 游戏: %s", exe);
    LogF("[i] 目录: %s", dir);

    ReadCfg(dir);

    if (Md5OfFile(exe, md5)) {
        char hs[33]; HashToStr(md5, hs);
        LogF("[+] exe MD5 = %s", hs);
        memcpy(g_exeMd5, md5, 16);
    } else {
        memset(md5, 0, 16);
        LogF("[!] MD5 计算失败, 用全零代替");
    }

    sprintf_s(ini, MAX_PATH, "%s\\SWRSToys.ini", dir);
    SetCurrentDirectoryA(dir);

    /* 独立模式: 直接扫 modules/ 目录自动发现, 不依赖任何 ini */
    if (g_discover) {
        LogF("[+] DiscoverModules=1 —— 自动扫描 modules 目录");
        int c = DiscoverModules(dir, md5);
        LogF("[=] 扫描到 %d 个候选 dll", c);
        LogF("[=] 完成: 成功 %d / 真失败 %d / 文件缺失 %d / 非模组 %d",
             g_loaded, g_failed, g_missing, g_notmod);
        if (g_loaded > 0) return;
        LogF("[i] 扫描无可用模块, 回退配置文件");
    }

    if (!g_loadModules) { LogF("[i] LoadModules=0 —— 跳过基于 ini 的模组加载"); return; }

    /* 关键: SokuEngine 是新一代框架, 自带 DllMain 接管游戏。
       若它存在, 只加载它 —— 其余模组由它自己按 SokuEngine.ini 管理。
       否则退回 SWRSToys 那套逐个加载。 */
    {
        char snk[MAX_PATH];
        sprintf_s(snk, MAX_PATH, "%s\\modules\\SokuEngine\\SokuEngine.dll", dir);
        if (!g_loadEngine) {
            LogF("[i] LoadEngine=0 —— 跳过 SokuEngine.dll");
        } else if (GetFileAttributesA(snk) != INVALID_FILE_ATTRIBUTES) {
            LogF("[+] 检测到 SokuEngine 框架, 由它接管模块加载");
            HMODULE h = LoadLibraryA(snk);
            if (h) {
                LogF("    SokuEngine.dll 已载入 hmod=%p", (void *)h);
                g_loaded++;
                /* 关键: 绝不能再按 SWRSToys.ini 重复加载 ——
                   两套框架抢同一块钩子区会栈溢出 (实测 0xC00000FD)。
                   SokuEngine 自己按 SokuEngine.ini 管理模块。 */
                LogF("[=] SokuEngine 接管完成: 已载入 1 个框架");
                return;
            }
            LogF("    [!] 加载失败 err=%lu, 回退 SWRSToys 路径", GetLastError());
            g_failed++;
        } else {
            LogF("[i] 未发现 SokuEngine, 走 SWRSToys.ini 逐模块加载");
        }
    }

    /* 回退: SokuModLoader 的 JSON 格式 */
    {
        char js[MAX_PATH];
        sprintf_s(js, MAX_PATH, "%s\\ModLoaderSettings.json", dir);
        if (GetFileAttributesA(js) != INVALID_FILE_ATTRIBUTES) {
            LogF("[+] 使用 ModLoaderSettings.json (新格式)");
            int cnt = LoadFromJson(js, md5);
            LogF("[=] JSON 里 enabled=true 的模块: %d", cnt);
            LogF("[=] 总计: 成功 %d / 真失败 %d / 文件缺失 %d / 非模组 %d",
                 g_loaded, g_failed, g_missing, g_notmod);
            return;
        }
        LogF("[i] 无 ModLoaderSettings.json, 回退 SWRSToys.ini");
    }

    n = GetPrivateProfileSectionA("Module", buf, sizeof(buf), ini);
    if (n == 0) { LogF("[i] %s 没有 [Module] 段", ini); return; }

    LogF("[+] 开始加载模组 (%lu 字节配置)", n);
    p = buf;
    while (*p) {
        char *eq = strchr(p, '=');
        if (eq) {
            const char *val = eq + 1;
            while (*val == ' ' || *val == '\t') val++;
            if (*val) TryLoadModule(val, md5);
        }
        p += strlen(p) + 1;
    }
    LogF("[=] 完成: 成功 %d / 真失败 %d / 文件缺失 %d / 非模组 %d",
         g_loaded, g_failed, g_missing, g_notmod);
}

/* ---------------- 导出 ---------------- */
extern "C" __declspec(dllexport) void * __stdcall Direct3DCreate9(unsigned int sdk)
{
    LogF("[>] Direct3DCreate9(sdk=%u)", sdk);
    LoadReal();
    if (!g_ranOnce) { g_ranOnce = true; LoadAll(); }
    else { LogF("[i] 已加载过模组, 跳过"); }

    /* D3D9On12 路径: 绕开原生 D3D9 驱动, 把 D3D9 调用翻译成 D3D12 */
    if (g_useOn12) {
        LogF("[>] UseOn12=1, 改走 D3D9On12");
        if (p_Create9On12) {
            void *r = NULL;
            __try { r = p_Create9On12(sdk, NULL, 0); }
            __except (EXCEPTION_EXECUTE_HANDLER) { LogF("    [!] Direct3DCreate9On12 异常"); }
            LogF("    Direct3DCreate9On12 -> %p", r);
            if (r) return r;
            LogF("    [!] On12 返回空, 回退原生");
        } else {
            LogF("    [!] 当前链目标未导出 Direct3DCreate9On12 (改用 ChainDll 空 + 系统 d3d9)");
        }
    }

    return p_Create9 ? p_Create9(sdk) : NULL;
}

/* 其余导出: 转发给系统 d3d9 */
extern "C" __declspec(dllexport) int __stdcall D3DPERF_BeginEvent(unsigned int a, const wchar_t *b)
{ LoadReal(); typedef int(__stdcall*F)(unsigned int,const wchar_t*); F f=(F)Real("D3DPERF_BeginEvent"); return f?f(a,b):-1; }
extern "C" __declspec(dllexport) int __stdcall D3DPERF_EndEvent(void)
{ LoadReal(); typedef int(__stdcall*F)(void); F f=(F)Real("D3DPERF_EndEvent"); return f?f():-1; }
extern "C" __declspec(dllexport) unsigned int __stdcall D3DPERF_GetStatus(void)
{ LoadReal(); typedef unsigned int(__stdcall*F)(void); F f=(F)Real("D3DPERF_GetStatus"); return f?f():0; }
extern "C" __declspec(dllexport) int __stdcall D3DPERF_QueryRepeatFrame(void)
{ LoadReal(); typedef int(__stdcall*F)(void); F f=(F)Real("D3DPERF_QueryRepeatFrame"); return f?f():0; }
extern "C" __declspec(dllexport) void __stdcall D3DPERF_SetMarker(unsigned int a, const wchar_t *b)
{ LoadReal(); typedef void(__stdcall*F)(unsigned int,const wchar_t*); F f=(F)Real("D3DPERF_SetMarker"); if(f)f(a,b); }
extern "C" __declspec(dllexport) void __stdcall D3DPERF_SetOptions(unsigned int a)
{ LoadReal(); typedef void(__stdcall*F)(unsigned int); F f=(F)Real("D3DPERF_SetOptions"); if(f)f(a); }
extern "C" __declspec(dllexport) void __stdcall D3DPERF_SetRegion(unsigned int a, const wchar_t *b)
{ LoadReal(); typedef void(__stdcall*F)(unsigned int,const wchar_t*); F f=(F)Real("D3DPERF_SetRegion"); if(f)f(a,b); }
extern "C" __declspec(dllexport) void __stdcall DebugSetLevel(int a)
{ LoadReal(); typedef void(__stdcall*F)(int); F f=(F)Real("DebugSetLevel"); if(f)f(a); }
extern "C" __declspec(dllexport) void __stdcall DebugSetMute(void) {}

/* Direct3D9Ex 等扩展接口 —— 同样转发给系统 d3d9 */
extern "C" __declspec(dllexport) long __stdcall Direct3DCreate9Ex(unsigned int sdk, void **ppOut)
{ LoadReal(); typedef long(__stdcall*F)(unsigned int, void**); F f=(F)Real("Direct3DCreate9Ex"); return f?f(sdk,ppOut):-1; }

extern "C" __declspec(dllexport) void * __stdcall Direct3DCreate9On12(unsigned int sdk, void *ovr, unsigned int n)
{ LoadReal(); typedef void*(__stdcall*F)(unsigned int, void*, unsigned int); F f=(F)Real("Direct3DCreate9On12"); return f?f(sdk,ovr,n):NULL; }

extern "C" __declspec(dllexport) long __stdcall Direct3DCreate9On12Ex(unsigned int sdk, void *ovr, unsigned int n, void **ppOut)
{ LoadReal(); typedef long(__stdcall*F)(unsigned int, void*, unsigned int, void**); F f=(F)Real("Direct3DCreate9On12Ex"); return f?f(sdk,ovr,n,ppOut):-1; }

extern "C" __declspec(dllexport) void * __stdcall Direct3DShaderValidatorCreate9(void)
{ LoadReal(); typedef void*(__stdcall*F)(void); F f=(F)Real("Direct3DShaderValidatorCreate9"); return f?f():NULL; }

extern "C" __declspec(dllexport) void __stdcall PSGPError(void) {}
extern "C" __declspec(dllexport) void __stdcall PSGPSampleTexture(void) {}

extern "C" __declspec(dllexport) int __stdcall Direct3D9EnableMaximizedWindowedModeShim(int a)
{ LoadReal(); typedef int(__stdcall*F)(int); F f=(F)Real("Direct3D9EnableMaximizedWindowedModeShim"); return f?f(a):0; }

/* ---------------- DllMain ---------------- */
BOOL WINAPI DllMain(HMODULE hSelf, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hSelf);
        g_log = fopen("soku_loader.log", "w");
        LogF("[i] soku_loader 注入成功 (d3d9 proxy)");
    } else if (reason == DLL_PROCESS_DETACH) {
        LogF("[i] 进程退出");
        if (g_log) { fclose(g_log); g_log = NULL; }
    }
    return TRUE;
}
