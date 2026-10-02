/*
 * soku_config.exe —— 非想天则 配置器 (Win32 原生 GUI)
 *
 * 为什么用 Win32 原生而不是 Electron/WPF:
 *   目标机器是 Th123 的安装目录, 单文件 exe 丢进去就能跑。
 *   无运行库依赖 (静态链接), 无需 .NET, 无需 node。
 *
 * 管两套配置:
 *   左: SokuEngine.ini  [Modules]  —— 模组的开/关
 *   右: MemoryPatch.ini [Section]  —— 画面/特效补丁
 *   下: 渲染后端 / 抗锯齿 (写 soku_loader.ini + soku_shim.ini)
 *
 * 多语言:
 *   lang\zh-CN.ini / en-US.ini 放在 backends\lang\ 下。
 *   选择顺序: soku_loader.ini 的 Language= -> 系统 UI 语言 -> 内置中文。
 *   布局(控件坐标)也写在语言文件里, 换行/语言导致字长差异也不会挤爆。
 *
 * 用法: soku_config.exe [游戏目录]   默认 D:\th123
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
/* WIN32_LEAN_AND_MEAN makes windows.h skip objidl.h, but gdiplus.h needs
   IImageBytes / PROPID from it -- without this, GdiplusImaging.h explodes
   with "missing type specifier" / "syntax error: PROPID". */
#include <objidl.h>
#include <commctrl.h>
#include <uxtheme.h>
#pragma comment(lib, "uxtheme.lib")
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
/* d3d9.h 只用于类型/枚举（在 shared\ 下）。
   注意：**不链接 d3d9.lib** —— 本 exe 住在游戏目录，那里有我们自己的
   d3d9.dll 代理，导入表按裸名解析会命中代理而不是系统库。
   所以运行时用 GetSystemDirectory + LoadLibrary 绝对路径加载。 */
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "advapi32.lib")   /* RegGetValueW (读 CPU 型号) */

/* ---------------- 全局 ---------------- */
static wchar_t g_dir[MAX_PATH]      = L"D:\\th123";

/* Forward declarations: AboutWndProc (defined above the i18n helpers) calls
   L()/LF(), so they must be declared before it. */
static const wchar_t *L(const char *key);
static const wchar_t *LF(const char *key, ...);
static void UpdateSysInfo(void);   /* ApplyStrings 在它定义之前就要调 */
static wchar_t g_engineIni[MAX_PATH] = L"";
static wchar_t g_patchIni[MAX_PATH]  = L"";
static char    g_engineIniA[MAX_PATH] = "";   /* ANSI 缓存, 避免临时对象悬垂 */
static char    g_patchIniA[MAX_PATH]  = "";

static wchar_t g_loaderIni[MAX_PATH] = L"";
static char    g_loaderIniA[MAX_PATH] = "";
static wchar_t g_shimIni[MAX_PATH] = L"";      /* soku_shim.ini (AA 设置在这里) */
static char    g_shimIniA[MAX_PATH] = "";
static wchar_t g_langDir[MAX_PATH] = L"";      /* backends\lang */

static int     g_backend = 1;   /* 索引到 kBackends[] */
static int     g_aaMode  = 0;   /* 0=off 1=ssaa 2=fxaa */
static int     g_ssaaLevel = 2; /* 1..4 */
static int     g_fxaaStrength = 60;  /* 0..100 */

static HWND g_listModules = NULL;
static HWND g_listPatches = NULL;
static HWND g_lblStatus   = NULL;
static HWND g_tab = NULL;
static Gdiplus::Image *g_aboutImage = NULL;
static ULONG_PTR g_gdiplusToken = 0;
static HWND g_aboutWnd = NULL;
/* 关于页信息：
   你可以通过两种方式填写这些信息：
   1) 直接在这里修改下面三个字符串（编译时生效）。
   2) 在游戏目录下的 soku_loader.ini 中添加 [About] 段，设置 AppName/Version/Author（运行时读取）。

   示例 soku_loader.ini:
   [About]
   AppName=我的配置器
   Version=1.2.3
   Author=作者名
*/
static wchar_t g_aboutAppName[128] = L"soku_config";     /* 在此处修改应用名（或编辑 soku_loader.ini 的 [About]） */
static wchar_t g_aboutVersion[64] = L"0.8.4hotfix";      /* 管理器版本号 */
static wchar_t g_aboutAuthor[128] = L"Aya";              /* 在此处填写作者，留空则不显示 */
/* 管理器 / 本地化 / 加载器三者独立发版，所以版本号分开列。
   （合并成一个会出现「版本: 0.8.1: 0.8.1」这种重复显示。）
   加载器那行必须与 loader\src\d3d9_proxy.cpp 的 SOKU_LOADER_VERSION 一致，
   package.ps1 会核对。） */
static wchar_t g_aboutLocVer[64]  = L"0.8.1";            /* 本地化（语言包）版本 */
static wchar_t g_aboutLdrVer[64]  = L"0.1.2hotfix";      /* 加载器 d3d9.dll 版本 */

/* 从 soku_loader.ini 的 [About] 段读取关于信息，优先于源码默认值 */
static void LoadAboutInfoFromIni(void)
{
    if (!g_loaderIni[0]) return;
    wchar_t buf[128];
    if (GetPrivateProfileStringW(L"About", L"AppName", L"", buf, _countof(buf), g_loaderIni) && buf[0])
        wcsncpy_s(g_aboutAppName, _countof(g_aboutAppName), buf, _TRUNCATE);
    if (GetPrivateProfileStringW(L"About", L"Version", L"", buf, _countof(buf), g_loaderIni) && buf[0])
        wcsncpy_s(g_aboutVersion, _countof(g_aboutVersion), buf, _TRUNCATE);
    if (GetPrivateProfileStringW(L"About", L"Author", L"", buf, _countof(buf), g_loaderIni) && buf[0])
        wcsncpy_s(g_aboutAuthor, _countof(g_aboutAuthor), buf, _TRUNCATE);
    /* 本地化 / 加载器版本也允许 ini 覆盖 —— 打包脚本据此写入实际版本 */
    if (GetPrivateProfileStringW(L"About", L"LocVersion", L"", buf, _countof(buf), g_loaderIni) && buf[0])
        wcsncpy_s(g_aboutLocVer, _countof(g_aboutLocVer), buf, _TRUNCATE);
    if (GetPrivateProfileStringW(L"About", L"LoaderVersion", L"", buf, _countof(buf), g_loaderIni) && buf[0])
        wcsncpy_s(g_aboutLdrVer, _countof(g_aboutLdrVer), buf, _TRUNCATE);
}

static LRESULT CALLBACK AboutWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        Gdiplus::Graphics g(hdc);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQuality);

        /* Layout: image on left (square), text on right */
        int padding = 8;
        int areaW = rc.right - rc.left;
        int areaH = rc.bottom - rc.top;
        int imgSize = min(180, areaH - padding * 2);
        int imgX = rc.left + padding;
        int imgY = rc.top + padding;

        if (g_aboutImage) {
            UINT iw = g_aboutImage->GetWidth();
            UINT ih = g_aboutImage->GetHeight();
            float scale = min((float)imgSize / iw, (float)imgSize / ih);
            int dw = (int)(iw * scale);
            int dh = (int)(ih * scale);
            int dx = imgX + (imgSize - dw) / 2;
            int dy = imgY + (imgSize - dh) / 2;
            Gdiplus::Rect dest(dx, dy, dw, dh);
            g.DrawImage(g_aboutImage, dest);
        } else {
            /* Draw placeholder rectangle */
            Gdiplus::Pen pen(Gdiplus::Color(120, 120, 120));
            /* Brush is abstract in GDI+; a concrete SolidBrush is required. */
            Gdiplus::SolidBrush brush(Gdiplus::Color(230, 230, 230));
            g.FillRectangle(&brush, imgX, imgY, imgSize, imgSize);
            g.DrawRectangle(&pen, imgX, imgY, imgSize, imgSize);
            Gdiplus::FontFamily ff(L"Segoe UI");
            Gdiplus::Font f(&ff, 10, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush tb(Gdiplus::Color(100,100,100));
            Gdiplus::StringFormat sf;
            sf.SetAlignment(Gdiplus::StringAlignmentCenter);
            sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            Gdiplus::RectF rf((Gdiplus::REAL)imgX, (Gdiplus::REAL)imgY, (Gdiplus::REAL)imgSize, (Gdiplus::REAL)imgSize);
            g.DrawString(L"about.png", -1, &f, rf, &sf, &tb);
        }

        /* Text area */
        int textX = imgX + imgSize + padding;
        int textW = areaW - (textX - rc.left) - padding;
        int y = rc.top + padding;

        Gdiplus::FontFamily ffTitle(L"Segoe UI");
        Gdiplus::Font titleFont(&ffTitle, 16, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font subFont(&ffTitle, 11, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush blackBrush(Gdiplus::Color(20,20,20));

        std::wstring name(g_aboutAppName);
        Gdiplus::RectF nameRect((Gdiplus::REAL)textX, (Gdiplus::REAL)y, (Gdiplus::REAL)textW, 24);
        g.DrawString(name.c_str(), -1, &titleFont, nameRect, NULL, &blackBrush);
        y += 28;

        /* 版本区：管理器 / 本地化 / 加载器 各一行，空的自动跳过。 */
        {
            struct { const wchar_t *label; const wchar_t *val; } rows[3];
            int nrows = 0;
            rows[nrows].label = L("LblVersion");      rows[nrows].val = g_aboutVersion;
            if (rows[nrows].val[0]) nrows++;
            rows[nrows].label = L("LblLocalization"); rows[nrows].val = g_aboutLocVer;
            if (rows[nrows].val[0]) nrows++;
            rows[nrows].label = L("LblLoader");       rows[nrows].val = g_aboutLdrVer;
            if (rows[nrows].val[0]) nrows++;
            for (int i = 0; i < nrows; i++) {
                std::wstring line = std::wstring(rows[i].label) + std::wstring(L": ") + rows[i].val;
                Gdiplus::RectF r((Gdiplus::REAL)textX, (Gdiplus::REAL)y, (Gdiplus::REAL)textW, 20);
                g.DrawString(line.c_str(), -1, &subFont, r, NULL, &blackBrush);
                y += 20;
            }
        }
        if (g_aboutAuthor[0]) {
            std::wstring authLabel = std::wstring(L("LblAuthor"));
            std::wstring auth = authLabel + std::wstring(L": ") + g_aboutAuthor;
            Gdiplus::RectF authRect((Gdiplus::REAL)textX, (Gdiplus::REAL)y, (Gdiplus::REAL)textW, 20);
            g.DrawString(auth.c_str(), -1, &subFont, authRect, NULL, &blackBrush);
            y += 20;
        }
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}
static HWND g_radBackend[6] = { NULL, NULL, NULL, NULL, NULL, NULL };
static HWND g_radAA[3] = { NULL, NULL, NULL };
static HWND g_cmbSSAA = NULL;
static HWND g_edStrength = NULL;
static HWND g_lblAA = NULL, g_lblSSAA = NULL, g_lblStrength = NULL;
/* 需要参与布局的控件（窗口缩放时要重新摆位）*/
static HWND g_lblMods = NULL, g_lblPats = NULL, g_lblBackend = NULL;
static HWND g_lblLangSel = NULL, g_cmbLangSel = NULL;
static HWND g_btnDeploy = NULL, g_btnRecall = NULL, g_btnLaunch = NULL;
static HWND g_btnSave = NULL, g_btnAllOn = NULL, g_btnAllOff = NULL, g_btnReload = NULL;
static HWND g_lblSysInfo = NULL;   /* 渲染页的「本机情况」面板 */

/* ---- 本机 CPU 特性探测 ----
   软光栅后端分 AVX2 / SSE2 两份, 选错会直接崩 (非法指令 0xC000001D)。
   这里读系统特性, 直接告诉用户该选哪个。 */
#ifndef PF_AVX_INSTRUCTIONS_AVAILABLE
#define PF_AVX_INSTRUCTIONS_AVAILABLE 39
#endif
#ifndef PF_AVX2_INSTRUCTIONS_AVAILABLE
#define PF_AVX2_INSTRUCTIONS_AVAILABLE 40
#endif
#ifndef PF_SSE2_INSTRUCTIONS_AVAILABLE
#define PF_SSE2_INSTRUCTIONS_AVAILABLE 10
#endif

static int CpuHasAvx2(void)
{
    return IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE) ? 1 : 0;
}

static int CpuHasSse2(void)
{
    return IsProcessorFeaturePresent(PF_SSE2_INSTRUCTIONS_AVAILABLE) ? 1 : 0;
}

/* ---- 显卡是否真的能做硬件 DX9 ----
   有独显却被推 CPU 软光栅是荒唐的，所以这里实测一遍。

   两个陷阱都要避：
   1) 本 exe 就住在游戏目录，那里有我们自己的 d3d9.dll 代理。
      裸名 LoadLibrary("d3d9.dll") 会拿到代理，所以必须用
      GetSystemDirectory 的绝对路径加载系统库。
   2) 只信 HAL：参考光栅器(REF)在没驱动时也能创建设备，
      但它跑在 CPU 上，当“硬件支持”用会误导。

   返回：1 = HAL 可用（硬件 DX9），0 = 不可用。
   adapterName 回填第一块适配器的名字（可传 NULL）。 */
static int GpuHasHardwareDx9(wchar_t *adapterName, int nameChars)
{
    typedef IDirect3D9 *(WINAPI *PFN_D3D9CREATE)(UINT);
    HMODULE mod = NULL;
    PFN_D3D9CREATE pCreate = NULL;
    IDirect3D9 *d3d = NULL;
    D3DADAPTER_IDENTIFIER9 id;
    UINT n, i;
    int ok = 0;
    wchar_t sysdir[MAX_PATH];
    wchar_t path[MAX_PATH];

    if (adapterName && nameChars > 0) adapterName[0] = 0;

    /* 系统目录下的 d3d9.dll（WOW64 进程下自动是 SysWOW64） */
    if (GetSystemDirectoryW(sysdir, MAX_PATH) == 0) return 0;
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\d3d9.dll", sysdir);
    mod = LoadLibraryW(path);
    if (!mod) return 0;

    pCreate = (PFN_D3D9CREATE)GetProcAddress(mod, "Direct3DCreate9");
    if (!pCreate) { FreeLibrary(mod); return 0; }

    d3d = pCreate(D3D_SDK_VERSION);
    if (!d3d) { FreeLibrary(mod); return 0; }

    n = d3d->GetAdapterCount();
    for (i = 0; i < n; i++) {
        D3DCAPS9 caps;
        ZeroMemory(&caps, sizeof(caps));
        /* 能报出 HAL 设备能力 = 这张卡有可用的 DX9 硬件驱动 */
        if (SUCCEEDED(d3d->GetDeviceCaps(i, D3DDEVTYPE_HAL, &caps))) {
            ZeroMemory(&id, sizeof(id));
            if (SUCCEEDED(d3d->GetAdapterIdentifier(i, 0, &id))) {
                if (adapterName && nameChars > 0) {
                    MultiByteToWideChar(CP_ACP, 0, id.Description, -1,
                                        adapterName, nameChars);
                }
            }
            ok = 1;
            break;
        }
    }

    d3d->Release();
    FreeLibrary(mod);
    return ok;
}

static HFONT g_font = NULL;      /* 现代字体 (Segoe UI / 微软雅黑) */

/* ============================================================================
 * i18n: 语言表
 * ==========================================================================*/
#define LK_MAX   128
#define LK_KEY   48
#define LK_VAL   512

struct LangKey {
    char    key[LK_KEY];
    wchar_t val[LK_VAL];
};
static LangKey g_lang[LK_MAX];
static int     g_langN = 0;
static int     g_langOverlay = 0;   /* 1 = 正在载入覆盖层（非底表） */
static char    g_langCode[32] = "zh-CN";
static char    g_langFile[MAX_PATH] = "";

static LangKey *LangFind(const char *key)
{
    for (int i = 0; i < g_langN; i++)
        if (strcmp(g_lang[i].key, key) == 0) return &g_lang[i];
    return NULL;
}

static std::vector<std::wstring> g_langFileList;
static std::vector<std::string> g_langCodes;

static bool ReadUtf8File(const wchar_t *path, std::string &text)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }

    text.resize((size_t)size.QuadPart);
    size_t offset = 0;
    while (offset < text.size()) {
        DWORD chunk = (DWORD)min(text.size() - offset, (size_t)MAXDWORD);
        DWORD bytesRead = 0;
        if (!ReadFile(file, &text[offset], chunk, &bytesRead, NULL) || bytesRead == 0) {
            CloseHandle(file);
            text.clear();
            return false;
        }
        offset += bytesRead;
    }
    CloseHandle(file);

    if (text.size() >= 3 &&
        (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        text.erase(0, 3);
    return true;
}

/* 读语言文件里 [section] 的某个键。
   Win32 Profile API 不按 UTF-8 解析文件，因此使用 Win32 文件 API 读取后自行解析。 */
static void ReadIniValueUtf8(const wchar_t *pathW, const char *section,
                             const char *key, wchar_t *out, int outChars)
{
    std::string raw;
    size_t klen;
    const char *sec, *end, *p;

    if (!out || outChars <= 0) return;
    out[0] = 0;

    if (!ReadUtf8File(pathW, raw)) return;
    raw.push_back('\0');
    /* 定位 [section] */
    sec = NULL;
    {
        char pat[128];
        _snprintf_s(pat, sizeof(pat), _TRUNCATE, "[%s]", section);
        sec = strstr(raw.c_str(), pat);
    }
    if (!sec) return;
    end = strstr(sec, "\n[");
    p = sec;
    klen = strlen(key);

    while (p && (!end || p < end)) {
        const char *e = strchr(p, '\n');
        const char *q;
        if (!e) e = p + strlen(p);
        q = p;
        while (q < e && (*q == ' ' || *q == '\t')) q++;
        if ((size_t)(e - q) > klen && _strnicmp(q, key, klen) == 0 && q[klen] == '=') {
            const char *v = q + klen + 1;
            const char *ve = e;
            int blen, wn;
            wchar_t wbuf[512];
            while (ve > v && (ve[-1] == '\r' || ve[-1] == '\n' || ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            blen = (int)(ve - v);
            if (blen > 0) {
                wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, v, blen, wbuf, 511);
                if (wn <= 0) return;
                wbuf[wn] = 0;
                wcsncpy_s(out, outChars, wbuf, _TRUNCATE);
            }
            return;
        }
        p = (*e == '\n') ? e + 1 : NULL;
    }
}

static void PopulateLanguageList(HWND h)
{
    if (!g_cmbLangSel) return;
    SendMessageW(g_cmbLangSel, CB_RESETCONTENT, 0, 0);
    g_langFileList.clear();
    wchar_t pattern[MAX_PATH];
    _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\*.ini", g_langDir);
    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileW(pattern, &fd);
    if (fh == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring full = std::wstring(g_langDir) + L"\\" + fd.cFileName;
        wchar_t name[128] = L"";
        wchar_t code[64] = L"";
        /* 本地化文件统一按 UTF-8 读取，避免 Profile API 使用系统代码页解析。 */
        ReadIniValueUtf8(full.c_str(), "meta", "Name", name, _countof(name));
        ReadIniValueUtf8(full.c_str(), "meta", "Code", code, _countof(code));
        const wchar_t *display = name[0] ? name : fd.cFileName;
        int idx = (int)SendMessageW(g_cmbLangSel, CB_ADDSTRING, 0, (LPARAM)display);
        g_langFileList.push_back(full);
        // store code (narrow) for this file (may be empty)
        char codeA[64] = "";
        if (code[0]) WideCharToMultiByte(CP_ACP, 0, code, -1, codeA, sizeof(codeA), NULL, NULL);
        g_langCodes.push_back(std::string(codeA));
        SendMessageW(g_cmbLangSel, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)g_langFileList.size() - 1);
        // Preselect current language if codes match
        if (codeA[0] && strcmp(codeA, g_langCode) == 0) SendMessageW(g_cmbLangSel, CB_SETCURSEL, idx, 0);
    } while (FindNextFileW(fh, &fd));
    FindClose(fh);
}

/* 取字符串。找不到返回空串（调用方应保证键都存在）。 */
static const wchar_t *L(const char *key)
{
    LangKey *p = LangFind(key);
    return p ? p->val : L"";
}

/* 带参数版本。用环形缓冲, 允许一行里嵌套调用几次。 */
static const wchar_t *LF(const char *key, ...)
{
    static wchar_t ring[6][LK_VAL];
    static int idx = 0;
    wchar_t *b = ring[idx = (idx + 1) % 6];
    const wchar_t *fmt = L(key);
    va_list ap;

    b[0] = 0;
    if (!fmt || !*fmt) return b;
    va_start(ap, key);
    _vsnwprintf_s(b, LK_VAL, _TRUNCATE, fmt, ap);
    va_end(ap);
    return b;
}

/* 把 wstring 里的字面 \n 换成真换行 (ini 里不能写多行) */
static void UnescapeNewlines(wchar_t *s)
{
    wchar_t *r = s, *w = s;
    while (*r) {
        if (r[0] == L'\\' && r[1] == L'n') { *w++ = L'\n'; r += 2; }
        else *w++ = *r++;
    }
    *w = 0;
}

/* 一行 "key=value"。语言文件固定使用 UTF-8。 */
/* 数一个模板里的格式占位符个数（%% 不算）。
   用途：翻译模板必须保留与底表相同数量的占位符，否则 _vsnwprintf_s
   会读到不存在的实参 —— 轻则乱码，重则崩（实测恶意 ini 的 %s%s%p 组合
   可直接把进程干成 0xC000041D）。 */
static int CountSpec(const wchar_t *s)
{
    int n = 0;
    while (s && (s = wcschr(s, L'%')) != NULL) {
        const wchar_t *q = s + 1;
        while (*q == L'#' || *q == L'0' || *q == L'-' || *q == L'+' ||
               *q == L' ' || *q == L'\'' || (*q >= L'0' && *q <= L'9') ||
               *q == L'*' || *q == L'.')
            q++;
        if (*q == L'\0') break;
        if (*q != L'%') n++;
        s++;
    }
    return n;
}

static void LangAddLine(const char *line, bool utf8)
{
    const char *eq;
    char key[LK_KEY];
    size_t klen;
    LangKey *p;
    wchar_t wbuf[LK_VAL];
    int n, blen;

    while (*line == ' ' || *line == '\t') line++;
    if (*line == 0 || *line == ';' || *line == '#') return;
    eq = strchr(line, '=');
    if (!eq) return;

    klen = (size_t)(eq - line);
    while (klen > 0 && (line[klen - 1] == ' ' || line[klen - 1] == '\t')) klen--;
    if (klen == 0 || klen >= LK_KEY) return;
    memcpy(key, line, klen);
    key[klen] = 0;

    {
        const char *v = eq + 1;
        blen = (int)strlen(v);
        if (utf8) {
            /* 语言文件固定 UTF-8：非法就丢这一行 */
            n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, v, blen, wbuf, LK_VAL - 1);
        } else {
            /* 内置兜底表是源码字符串常量。本工程用 /execution-charset:gbk 编译，
               所以它们是 GBK 字节。以前这里统一按 UTF-8 strict 解 —— GBK 字节
               必然返回 0 (err=1113)，导致整张中文兜底表被静默丢弃，
               语言文件缺失时界面全是空标签。现在按 ANSI 解。 */
            n = MultiByteToWideChar(CP_ACP, 0, v, blen, wbuf, LK_VAL - 1);
        }
        if (n <= 0) return;
        wbuf[n] = 0;
    }
    UnescapeNewlines(wbuf);

    /* ---- 格式串防护 ----
       语言值会被当成格式串交给 _vsnwprintf_s（翻译里确实要用 %d/%s），
       但 %n 是【任意写】原语 —— 实测恶意 ini 写 %n 能让进程
       0xC0000409。任何正当 UI 文案都不需要 %n，所以只杀它一个，
       不误伤 %d / %10d / %s 这些合法用法。
       （无参调用点的崩溃另由 SetStatus("%s", ...) 解决。） */
    {
        const wchar_t *scan = wbuf;
        while ((scan = wcschr(scan, L'%')) != NULL) {
            const wchar_t *q = scan + 1;
            while (*q == L'#' || *q == L'0' || *q == L'-' || *q == L'+' ||
                   *q == L' ' || *q == L'\'' || (*q >= L'0' && *q <= L'9') ||
                   *q == L'*' || *q == L'.')
                q++;
            if (*q == L'n') return;   /* %n / %hn / %ln → 整键丢弃 */
            scan++;
        }
    }

    p = LangFind(key);
    /* ---- 覆盖层形状校验 ----
       zh-CN 是随包发的可信样板。调用点的实参个数是固定的，所以：
         · 覆盖层占位符【多于】底表 → 会读到不存在的实参 → 崩/泄漏，必须拒
         · 覆盖层占位符【少于或等于】底表 → 多余实参被忽略，安全
           （en-MC 那种 hex 语言就是 0 个占位符，属于这种情况）
       超了才丢弃该键 → 保留中文底值。 */
    if (p && g_langOverlay) {
        if (CountSpec(wbuf) > CountSpec(p->val)) return;
    }
    if (!p) {
        if (g_langN >= LK_MAX) return;
        p = &g_lang[g_langN++];
        strncpy_s(p->key, LK_KEY, key, _TRUNCATE);
    }
    wcsncpy_s(p->val, LK_VAL, wbuf, _TRUNCATE);
}

static void LangLoadFile(const wchar_t *path, const char *code, bool reset)
{
    std::string text;
    if (!ReadUtf8File(path, text)) return;
    /* reset=1: clear and reload (used for the base layer).
       reset=0: overlay -- only keys present in the file are replaced, the rest
       keep their previous value.  That is what makes "zh-CN as the template"
       work: load zh-CN first, then overlay the target language, so any key the
       target file omits falls back to Chinese instead of going blank. */
    if (reset) { g_langN = 0; g_langOverlay = 0; }
    else       { g_langOverlay = 1; }
    strncpy_s(g_langCode, 32, code, _TRUNCATE);
    WideCharToMultiByte(CP_ACP, 0, path, -1, g_langFile, MAX_PATH, NULL, NULL);
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find_first_of("\r\n", start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (start == 0 && line.size() >= 3 &&
            (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF)
            line.erase(0, 3);
        LangAddLine(line.c_str(), true);
        start = end;
        while (start < text.size() && (text[start] == '\r' || text[start] == '\n')) start++;
    }
}

/* 内置兜底：语言文件缺失时至少界面是中文 */
static void LangLoadBuiltin(void)
{
    static const char *kv[] = {
        "Title",      "非想天则 模组配置器",
        "ColItem",    "项目",
        "LblModules", "模组开关 (SokuEngine.ini)",
        "LblPatches", "画面/特效补丁 (MemoryPatch.ini)",
        "LblBackend", "渲染后端 (选中后点「部署」把零件放进根目录)",
        "LblAA",      "抗锯齿:",
        "LblAANeedCPU","抗锯齿 (需 CPU 软光栅):",
        "LblSSAA",    "SSAA 倍率:",
        "LblStrength","FXAA 强度:",
        "BtnDeploy",  "部署后端",
        "BtnRecall",  "全部回收",
        "BtnLaunch",  "启动 th123.exe",
        "BtnSave",    "保存",
        "BtnAllOn",   "左列全开",
        "BtnAllOff",  "左列全关",
        "BtnReload",  "重新读取",
        "AAOff",      "关 (最快)",
        "AASSAA",     "SSAA 超采样",
        "AAFXAA",     "FXAA 后处理",
        "AACombo2",   "2 (4x)",
        "AACombo3",   "3 (9x)",
        "AACombo4",   "4 (16x)",
        "AACombo1",   "1 (等价关闭)",
        "BackendNative","原生 D3D9",
        "BackendDXVK",  "DXVK (Vulkan)",
        "BackendOn12",  "D3D9On12 (D3D12)",
        "BackendDgV",   "dgVoodoo2 (D3D11)",
        "BackendCPUAVX2","CPU 软光栅 · AVX2 (Haswell+)",
        "BackendCPUSSE2","CPU 软光栅 · SSE2 (任意 x64)",
        "LblAbout","关于",
        "LblLanguage","语言",
        "LblVersion","版本",
        "LblLocalization","本地化",
        "LblLoader",  "加载器",
        "LblAuthor",  "作者",
        "StReady",    "就绪",
        "StLoaded",   "已载入 模组 %d 项 / 补丁 %d 项 / 后端: %s / 抗锯齿: %s",
        "StNoModules","读不到 SokuEngine.ini [Modules] 段",
        "StReadModules","已读取 %d 个模组",
        "StSaved",    "已保存 %d 项 (重启游戏生效)",
        "StBackendSet","渲染后端 -> %s",
        "StAASet",    "抗锯齿 -> %s",
        "StAASetSSAA","抗锯齿 -> SSAA (倍率 %d)",
        "StAASetFXAA","抗锯齿 -> FXAA (强度 %d)",
        "StSSAASet",  "SSAA 倍率 -> %d",
        "StStrengthSet","FXAA 强度 -> %d",
        "StDeployed", "后端 %s: %s",
        "StRecalled", "已回收根目录下的全部后端文件 (仅剩加载器 d3d9.dll)",
        "StAllOn",    "左列已全选 (记得点保存)",
        "StAllOff",   "左列已全不选 (记得点保存)",
        "StReloaded", "已重新读取 (后端: %s / 抗锯齿: %s)",
        "StNoExe",    "未在目录找到 th123.exe",
        "StLaunched", "已启动游戏",
        "StLaunchFail","启动 th123.exe 失败 (错误 %d)",
        "StAASuffixSSAA","SSAA",
        "StAASuffixFXAA","FXAA",
        "StAASuffixOff", "关",
        "MsgDeployOK",  "已部署 %d 个文件",
        "MsgDeployMiss","缺少 backends\\%s\\%s",
        "WarnAVX2Title","CPU 软光栅 · AVX2",
        "WarnAVX2Body", "需要 Haswell 代或更新的 CPU（2013 年及以后），\\n即支持 AVX2 + FMA3 的处理器。\\n\\ni7 三代及更早、Core2、赛扬 J/N 系（部分）不支持，\\n选了会在启动时直接崩溃（0xC000001D 非法指令）。\\n\\n不确定的话请改用「SSE2 (任意 x64)」——\\n它慢一些但任何 64 位 CPU 都能跑。\\n\\n继续部署吗？\\n\\n(仅限绝望者：如果你的显卡支持 DX9，请不要用 CPU 软光栅。)",
        "WarnSSE2Title","CPU 软光栅 · SSE2",
        "WarnSSE2Body", "兼容模式：任何 64 位 CPU 都能跑，不需要 AVX2。\\n\\n光栅速度比 AVX2 版慢（无 SIMD 批处理混合），\\n但老机器（Nehalem / Westmere / Core2 等）只能用这个。\\n\\n如果 CPU 支持 AVX2，建议改用 AVX2 版以获得更好帧率。\\n\\n继续部署吗？",
        NULL, NULL
    };
    int i;
    g_langN = 0;
    g_langOverlay = 0;      /* 内置中文表就是底表 */
    for (i = 0; kv[i]; i += 2) LangAddLine((std::string(kv[i]) + "=" + kv[i + 1]).c_str(), false);
}

/* 布局默认值（语言文件可覆盖） */
static int LInt(const char *key, int def)
{
    LangKey *p = LangFind(key);
    if (!p || !p->val[0]) return def;
    return _wtoi(p->val);
}

typedef struct { int x, w; } Pos;
/* 3x2 网格对齐：每列起始 0/275/550，列宽 270。
   之前手摆的坐标让第二行缩进不一致，中间留了个怪缝。 */
static Pos kPosBackend[6] = { {0,270},{275,270},{550,270}, {0,270},{275,270},{550,270} };
/* AA 行：标签 + 3 单选 + SSAA标签/下拉 + 强度标签/输入 */
static Pos kPosAA[8]      = { {0,80},{84,110},{198,110},{312,110},{430,90},{522,70},{604,110},{714,50} };

static void LoadLayoutFromLang(void)
{
    char k[16];
    int i;
    for (i = 0; i < 6; i++) {
        sprintf_s(k, sizeof(k), "Rb%dX", i); kPosBackend[i].x = LInt(k, kPosBackend[i].x);
        sprintf_s(k, sizeof(k), "Rb%dW", i); kPosBackend[i].w = LInt(k, kPosBackend[i].w);
    }
    for (i = 0; i < 8; i++) {
        sprintf_s(k, sizeof(k), "Aa%dX", i); kPosAA[i].x = LInt(k, kPosAA[i].x);
        sprintf_s(k, sizeof(k), "Aa%dW", i); kPosAA[i].w = LInt(k, kPosAA[i].w);
    }
}

/* 选语言并载入 */
/* 按语言代码载入。总是先拿 zh-CN 打底，再把目标语言覆盖上去，
   这样任何语言文件漏写的键都会回退到中文样板，不会变成空白。 */
/* 扫描 lang 目录，按 meta.Code 找对应文件（大小写不敏感）。
   回退：文件名主干（不含 .ini）与 code 相同也算命中。
   为什么要扫而不是直接拼 code.ini：文件名和 meta.Code 很容易不一致
   （实测 en-PT.ini 里写的是 en-PIRATE，拼接就找不到了）。 */
static int FindLangFileByCode(const char *code, wchar_t *outPath, int outChars)
{
    wchar_t pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE fh;
    char want[64];

    if (!code || !code[0]) return 0;
    strncpy_s(want, sizeof(want), code, _TRUNCATE);

    _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\*.ini", g_langDir);
    fh = FindFirstFileW(pattern, &fd);
    if (fh == INVALID_HANDLE_VALUE) return 0;

    do {
        wchar_t full[MAX_PATH];
        wchar_t wcode[64] = L"";
        char codeA[64] = "";

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        _snwprintf_s(full, MAX_PATH, _TRUNCATE, L"%s\\%s", g_langDir, fd.cFileName);

        /* 先按 meta.Code 比 */
        ReadIniValueUtf8(full, "meta", "Code", wcode, _countof(wcode));
        if (wcode[0])
            WideCharToMultiByte(CP_ACP, 0, wcode, -1, codeA, sizeof(codeA), NULL, NULL);
        if (codeA[0] && _stricmp(codeA, want) == 0) {
            wcsncpy_s(outPath, outChars, full, _TRUNCATE);
            FindClose(fh);
            return 1;
        }

        /* 再按文件名主干比 */
        {
            wchar_t stem[MAX_PATH];
            wchar_t *dot;
            wcsncpy_s(stem, MAX_PATH, fd.cFileName, _TRUNCATE);
            dot = wcsrchr(stem, L'.');
            if (dot) *dot = 0;
            WideCharToMultiByte(CP_ACP, 0, stem, -1, codeA, sizeof(codeA), NULL, NULL);
            if (_stricmp(codeA, want) == 0) {
                wcsncpy_s(outPath, outChars, full, _TRUNCATE);
                FindClose(fh);
                return 1;
            }
        }
    } while (FindNextFileW(fh, &fd));

    FindClose(fh);
    return 0;
}

/* 按【真实文件路径】载入。总是先拿 zh-CN 打底，再把目标语言覆盖上去，
   这样任何语言文件漏写的键都会回退到中文样板，不会变成空白。 */
static void LoadLanguageByFile(const wchar_t *fullPath, const char *code)
{
    wchar_t p[MAX_PATH];

    /* ---- 1) 基底: zh-CN.ini (没有则用内置中文表) ---- */
    _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%s\\zh-CN.ini", g_langDir);
    if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) LangLoadFile(p, "zh-CN", true);
    else                                                 LangLoadBuiltin();

    /* ---- 2) 覆盖: 选中的那个文件 ---- */
    if (fullPath && fullPath[0]) {
        if (GetFileAttributesW(fullPath) != INVALID_FILE_ATTRIBUTES)
            LangLoadFile(fullPath, code ? code : "", false);
    }

    if (g_langN == 0) LangLoadBuiltin();
    LoadLayoutFromLang();
}

/* 按语言代码载入（启动时用）：先找文件，找到就按路径载。 */
static void LoadLanguage(const char *code)
{
    wchar_t full[MAX_PATH];

    if (FindLangFileByCode(code, full, MAX_PATH))
        LoadLanguageByFile(full, code);
    else
        LoadLanguageByFile(NULL, code);   /* 找不到就只载中文底 */
}

static void InitLang(void)
{
    char code[64] = "";
    LANGID lid;

    /* 1) 显式指定 */
    GetPrivateProfileStringA("soku_loader", "Language", "", code, sizeof(code), g_loaderIniA);

    /* 2) 系统语言 */
    if (!code[0]) {
        lid = GetUserDefaultUILanguage();
        if (PRIMARYLANGID(lid) == LANG_CHINESE) strcpy_s(code, sizeof(code), "zh-CN");
        else                                    strcpy_s(code, sizeof(code), "en-US");
    }

    LoadLanguage(code);
}

/* ============================================================================
 * 基础工具
 * ==========================================================================*/
static void SetStatus(const wchar_t *fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    if (!g_lblStatus) return;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, 512, _TRUNCATE, fmt, ap);
    va_end(ap);
    SetWindowTextW(g_lblStatus, buf);
}

static void InitPaths(const wchar_t *dir)
{
    wcsncpy_s(g_dir, MAX_PATH, dir, _TRUNCATE);
    _snwprintf_s(g_engineIni, MAX_PATH, _TRUNCATE, L"%s\\modules\\SokuEngine\\SokuEngine.ini", g_dir);
    _snwprintf_s(g_patchIni,  MAX_PATH, _TRUNCATE, L"%s\\modules\\MemoryPatch\\MemoryPatch.ini", g_dir);
    _snwprintf_s(g_loaderIni, MAX_PATH, _TRUNCATE, L"%s\\soku_loader.ini", g_dir);
    _snwprintf_s(g_shimIni,   MAX_PATH, _TRUNCATE, L"%s\\soku_shim.ini", g_dir);
    _snwprintf_s(g_langDir,   MAX_PATH, _TRUNCATE, L"%s\\backends\\lang", g_dir);
    WideCharToMultiByte(CP_ACP, 0, g_engineIni, -1, g_engineIniA, MAX_PATH, NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, g_patchIni,  -1, g_patchIniA,  MAX_PATH, NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, g_loaderIni, -1, g_loaderIniA, MAX_PATH, NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, g_shimIni,   -1, g_shimIniA,   MAX_PATH, NULL, NULL);
}

/* ---- 抗锯齿 (soku_shim.ini) ----
   下拉顺序固定: 0=2(4x) 1=3(9x) 2=4(16x) 3=1(等价关) */
static int SSAALevelToIndex(int lvl)
{
    if (lvl >= 2 && lvl <= 4) return lvl - 2;
    return 3;
}

static int SSAAIndexToLevel(int idx)
{
    if (idx >= 0 && idx <= 2) return idx + 2;
    return 1;
}

static void LoadAA(void)
{
    char v[32] = "";
    GetPrivateProfileStringA("soku_shim", "AAMode", "off", v, sizeof(v), g_shimIniA);
    if (_stricmp(v, "ssaa") == 0)      g_aaMode = 1;
    else if (_stricmp(v, "fxaa") == 0) g_aaMode = 2;
    else                                g_aaMode = 0;

    g_ssaaLevel = GetPrivateProfileIntA("soku_shim", "SuperSample", 2, g_shimIniA);
    if (g_ssaaLevel < 1) g_ssaaLevel = 1;
    if (g_ssaaLevel > 4) g_ssaaLevel = 4;

    g_fxaaStrength = GetPrivateProfileIntA("soku_shim", "FXAAStrength", 60, g_shimIniA);
    if (g_fxaaStrength < 0)   g_fxaaStrength = 0;
    if (g_fxaaStrength > 100) g_fxaaStrength = 100;
}

static void SaveAA(void)
{
    const char *mode = (g_aaMode == 1) ? "ssaa" : (g_aaMode == 2 ? "fxaa" : "off");
    char buf[16];
    WritePrivateProfileStringA("soku_shim", "AAMode", mode, g_shimIniA);
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", g_ssaaLevel);
    WritePrivateProfileStringA("soku_shim", "SuperSample", buf, g_shimIniA);
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", g_fxaaStrength);
    WritePrivateProfileStringA("soku_shim", "FXAAStrength", buf, g_shimIniA);
}

static const wchar_t *AAName(void)
{
    if (g_aaMode == 1) return L("StAASuffixSSAA");
    if (g_aaMode == 2) return L("StAASuffixFXAA");
    return L("StAASuffixOff");
}

/* ============================================================================
 * 数据表
 * ==========================================================================*/
struct Item {
    std::string  key;      /* ini 键名 */
    std::wstring label;    /* 界面显示 */
    bool         enabled;
};

static std::vector<Item> g_modules;
static std::vector<Item> g_patches;

/* MemoryPatch 的可用补丁 —— label 从语言文件取 */
static const char *kPatchKeys[] = {
    "16bitsColor", "DisableWeatherEffect", "HideHUD", "AllowMultiInstance",
    "AlwaysRandomMusic", "DefaultDenyWatch", "DefaultDenyBattle",
    "InputFreedom", "FilesystemFirst", "NativeDPadFix"
};
static const char *kPatchLabelKeys[] = {
    "Patch16bitsColor", "PatchDisableWeather", "PatchHideHUD", "PatchMultiInstance",
    "PatchRandomMusic", "PatchDenyWatch", "PatchDenyBattle",
    "PatchInputFreedom", "PatchFilesystemFirst", "PatchNativeDPad"
};
static const int kPatchCount = 10;

/* 若语言文件漏了补丁名, 回退到键名本身 */
static void EnsurePatchLabels(void)
{
    LangKey *p;
    if ((p = LangFind("Patch16bitsColor")) == NULL || !p->val[0])
        LangAddLine("Patch16bitsColor=16bits 色深 (复古画面)", false);
    if ((p = LangFind("PatchDisableWeather")) == NULL || !p->val[0])
        LangAddLine("PatchDisableWeather=关闭全部天气特效", false);
    if ((p = LangFind("PatchHideHUD")) == NULL || !p->val[0])
        LangAddLine("PatchHideHUD=隐藏游戏 HUD", false);
    if ((p = LangFind("PatchMultiInstance")) == NULL || !p->val[0])
        LangAddLine("PatchMultiInstance=允许多开游戏", false);
    if ((p = LangFind("PatchRandomMusic")) == NULL || !p->val[0])
        LangAddLine("PatchRandomMusic=永远随机关卡音乐", false);
    if ((p = LangFind("PatchDenyWatch")) == NULL || !p->val[0])
        LangAddLine("PatchDenyWatch=默认禁止观战", false);
    if ((p = LangFind("PatchDenyBattle")) == NULL || !p->val[0])
        LangAddLine("PatchDenyBattle=默认观战模式", false);
    if ((p = LangFind("PatchInputFreedom")) == NULL || !p->val[0])
        LangAddLine("PatchInputFreedom=无焦点也接收手柄输入", false);
    if ((p = LangFind("PatchFilesystemFirst")) == NULL || !p->val[0])
        LangAddLine("PatchFilesystemFirst=优先从文件系统读资源", false);
    if ((p = LangFind("PatchNativeDPad")) == NULL || !p->val[0])
        LangAddLine("PatchNativeDPad=用手柄十字键而非摇杆", false);
}

struct BFile { const char *src; const char *dst; };
struct BackendDef {
    const char *dir;       /* backends\ 下的子目录 */
    const char *labelKey;  /* 语言文件里的键 */
    const char *chain;     /* soku_loader.ini ChainDll */
    const char *on12;      /* soku_loader.ini UseOn12 */
    int         nFiles;
    BFile       files[6];
};

static const BackendDef kBackends[] = {
    { "native", "BackendNative", "", "0", 0, {} },
    { "dxvk", "BackendDXVK", "dxvk_d3d9.dll", "0", 4, {
        { "dxvk_d3d9.dll", "dxvk_d3d9.dll" },
        { "d3d11.dll",     "d3d11.dll" },
        { "dxgi.dll",      "dxgi.dll" },
        { "dxvk.conf",     "dxvk.conf" } } },
    { "on12", "BackendOn12", "", "1", 0, {} },
    { "dgvoodoo2", "BackendDgV", "dgvoodoo_d3d9.dll", "0", 2, {
        { "dgvoodoo_d3d9.dll", "dgvoodoo_d3d9.dll" },
        { "dgVoodoo.conf",     "dgVoodoo.conf" } } },
    /* 【正式】CPU 软光栅 —— 两个架构分开，用户按 CPU 选。
       Haswell(2013+) 用 AVX2；更老的（Nehalem/Westmere/Core2）必须用 SSE2，
       否则非法指令 0xC000001D（i7 初代实测）。 */
    { "cpu", "BackendCPUAVX2", "cpu_avx2.dll", "0", 2, {
        { "cpu_avx2.dll",  "cpu_avx2.dll" },
        { "d3dx9_33.dll",  "d3dx9_33.dll" } } },
    { "cpu", "BackendCPUSSE2", "cpu_sse2.dll", "0", 2, {
        { "cpu_sse2.dll",  "cpu_sse2.dll" },
        { "d3dx9_33.dll",  "d3dx9_33.dll" } } },
};
static const int kBackendCount = sizeof(kBackends) / sizeof(kBackends[0]);

/* ============================================================================
 * 后端
 * ==========================================================================*/
static void LoadBackend(void)
{
    char chain[128] = "";
    GetPrivateProfileStringA("soku_loader", "ChainDll", "", chain, sizeof(chain), g_loaderIniA);
    int on12 = GetPrivateProfileIntA("soku_loader", "UseOn12", 0, g_loaderIniA);
    int i, best = -1;

    g_backend = 0;
    if (on12) {
        for (i = 0; i < kBackendCount; i++)
            if (strcmp(kBackends[i].on12, "1") == 0) { g_backend = i; return; }
        return;
    }
    if (chain[0]) {
        /* 取【最长匹配】—— CPU 后端有两个条目共子目录 'cpu',
           用子串比对才能区分 cpu_avx2.dll / cpu_sse2.dll。 */
        for (i = 0; i < kBackendCount; i++) {
            if (!kBackends[i].chain[0]) continue;
            if (strstr(chain, kBackends[i].chain) == NULL) continue;
            if (best < 0 || strlen(kBackends[i].chain) > strlen(kBackends[best].chain))
                best = i;
        }
        if (best >= 0) g_backend = best;
    }
}

static void SaveBackend(void)
{
    if (g_backend < 0 || g_backend >= kBackendCount) g_backend = 0;
    const BackendDef *bd = &kBackends[g_backend];
    WritePrivateProfileStringA("soku_loader", "ChainDll", bd->chain, g_loaderIniA);
    WritePrivateProfileStringA("soku_loader", "UseOn12",  bd->on12,  g_loaderIniA);
}

static void CleanBackendFiles(void)
{
    wchar_t p[MAX_PATH];
    int b, f;
    for (b = 0; b < kBackendCount; b++) {
        for (f = 0; f < kBackends[b].nFiles; f++) {
            _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%s\\backends\\%S", g_dir, kBackends[b].files[f].dst);
            DeleteFileW(p);
        }
    }
}

static int DeployBackend(int idx, wchar_t *msg, size_t msgLen)
{
    wchar_t src[MAX_PATH], dst[MAX_PATH];
    int copied = 0, f;
    if (idx < 0 || idx >= kBackendCount) return 0;
    const BackendDef *bd = &kBackends[idx];

    CleanBackendFiles();
    for (f = 0; f < bd->nFiles; f++) {
        _snwprintf_s(src, MAX_PATH, _TRUNCATE, L"%s\\backends\\%S\\%S",
                     g_dir, bd->dir, bd->files[f].src);
        _snwprintf_s(dst, MAX_PATH, _TRUNCATE, L"%s\\%S", g_dir, bd->files[f].dst);
        if (CopyFileW(src, dst, FALSE)) copied++;
        else {
            _snwprintf_s(msg, msgLen, _TRUNCATE, LF("MsgDeployMiss"),
                         bd->dir, bd->files[f].src);
            return copied;
        }
    }
    _snwprintf_s(msg, msgLen, _TRUNCATE, LF("MsgDeployOK"), copied);
    return copied;
}

/* AA 的实现在 cpu_d3d9.dll 里 (操作 SR_buf)。选其他后端时那个 dll 根本不加载。 */
static int BackendIsCPU(int idx)
{
    if (idx < 0 || idx >= kBackendCount) return 0;
    return strstr(kBackends[idx].chain, "cpu_") != NULL;
}

static void SyncAAEnabled(void)
{
    BOOL en = BackendIsCPU(g_backend) ? TRUE : FALSE;
    int i;
    for (i = 0; i < 3; i++) if (g_radAA[i]) EnableWindow(g_radAA[i], en);
    if (g_cmbSSAA)     EnableWindow(g_cmbSSAA, en);
    if (g_edStrength)  EnableWindow(g_edStrength, en);
    if (g_lblSSAA)     EnableWindow(g_lblSSAA, en);
    if (g_lblStrength) EnableWindow(g_lblStrength, en);
    if (g_lblAA) {
        SetWindowTextW(g_lblAA, en ? L("LblAA") : L("LblAANeedCPU"));
        EnableWindow(g_lblAA, TRUE);
    }
}

/* ============================================================================
 * 配置读写
 * ==========================================================================*/
static void LoadModules(void)
{
    g_modules.clear();

    char buf[16384];
    DWORD n = GetPrivateProfileSectionA("Modules", buf, sizeof(buf), g_engineIniA);
    if (n == 0) { SetStatus(L("%s"), L("StNoModules")); return; }

    char *p = buf;
    while (*p) {
        char *eq = strchr(p, '=');
        if (eq) {
            Item it;
            std::string k(p, eq - p);
            while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
            it.key = k;
            const char *v = eq + 1;
            while (*v == ' ' || *v == '\t') v++;
            it.enabled = (atoi(v) != 0);
            {
                wchar_t w[256];
                MultiByteToWideChar(CP_ACP, 0, it.key.c_str(), -1, w, 256);
                it.label = w;
            }
            g_modules.push_back(it);
        }
        p += strlen(p) + 1;
    }
    SetStatus(LF("StReadModules"), (int)g_modules.size());
}

static void LoadPatches(void)
{
    g_patches.clear();

    for (int i = 0; i < kPatchCount; i++) {
        Item it;
        it.key = kPatchKeys[i];
        it.label = L(kPatchLabelKeys[i]);
        it.enabled = GetPrivateProfileIntA(it.key.c_str(), "Enabled", 0, g_patchIniA) != 0;
        g_patches.push_back(it);
    }
}

static void SaveAll(void)
{
    int changed = 0;

    for (size_t i = 0; i < g_modules.size(); i++) {
        const char *v = g_modules[i].enabled ? "1" : "0";
        WritePrivateProfileStringA("Modules", g_modules[i].key.c_str(), v, g_engineIniA);
        changed++;
    }
    for (size_t i = 0; i < g_patches.size(); i++) {
        const char *v = g_patches[i].enabled ? "1" : "0";
        WritePrivateProfileStringA(g_patches[i].key.c_str(), "Enabled", v, g_patchIniA);
        changed++;
    }
    SaveBackend();
    changed++;
    SetStatus(LF("StSaved"), changed);
}

static void FillList(HWND lv, const std::vector<Item> &items)
{
    ListView_DeleteAllItems(lv);
    for (size_t i = 0; i < items.size(); i++) {
        LVITEMW it = {0};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = (LPWSTR)items[i].label.c_str();
        int idx = (int)SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
        ListView_SetCheckState(lv, idx, items[i].enabled ? TRUE : FALSE);
    }
}

static void SyncFromList(HWND lv, std::vector<Item> &items)
{
    for (size_t i = 0; i < items.size(); i++)
        items[i].enabled = ListView_GetCheckState(lv, (int)i) ? true : false;
}

/* ============================================================================
 * 控件 ID
 * ==========================================================================*/
#define IDC_MODLIST   1001
#define IDC_PATLIST   1002
#define IDC_TAB       1000
#define IDC_SAVE      1003
#define IDC_ALLON     1004
#define IDC_ALLOFF    1005
#define IDC_RELOAD    1006
#define IDC_STATUS    1007
#define IDC_LBLM      1008
#define IDC_LBLP      1009
#define IDC_LBLB      1010
/* 单选按钮 ID 必须【连续】—— CheckRadioButton 靠范围扫。
   6 个后端: NATIVE..CPU_SSE2 = 1011..1016 */
#define IDC_RB_NATIVE   1011
#define IDC_RB_DXVK     1012
#define IDC_RB_ON12     1013
#define IDC_RB_DGV      1014
#define IDC_RB_CPU      1015
#define IDC_RB_CPU_SSE2 1016
#define IDC_DEPLOY      1017
#define IDC_RECALL      1018
#define IDC_LAUNCH      1019
/* 抗锯齿单选 (另一个连续范围) */
#define IDC_RB_AA_OFF   1021
#define IDC_RB_AA_SSAA  1022
#define IDC_RB_AA_FXAA  1023
#define IDC_CMB_SSAA    1024
#define IDC_ED_STRENGTH 1025
#define IDC_LBL_AA      1026
#define IDC_LBL_SSAA    1027
#define IDC_LBL_STR     1028
#define IDC_CMB_LANG    1029   /* language picker (AboutWndProc + main window) */

/* ============================================================================
 * 布局
 * ==========================================================================*/
static void SetupListView(HWND lv)
{
    SendMessageW(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    LVCOLUMNW col = {0};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = (LPWSTR)L("ColItem");
    col.cx = 300;
    SendMessageW(lv, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
}

/* 统一布局：列表随窗口伸缩，底栏锚底；底栏内的两行控件坐标来自语言文件。 */
static void LayoutChildren(HWND h)
{
    RECT rc;
    int W, H, margin = 10, top, rowH = 30, bottomH, listH, colW, y;
    int i;
    const int TAB_Y = 6, TAB_H = 26;      /* 选项卡占 6..32 */
    const int LBL_Y = TAB_Y + TAB_H + 4;  /* 标签占 36..56，不再与 Tab 重叠 */

    GetClientRect(h, &rc);
    W = rc.right; H = rc.bottom;
    if (W < 640) W = 640;

    /* 选项卡固定在顶部；列表从其【下方】开始。
       旧代码把标签放在 y=8 而 Tab 在 y=10，两者压在一起，
       所以列表顶部的行被标签糊住、Tab 也被遮。 */
    top = LBL_Y + 24;                     /* 列表从 60 开始 */

    /* 底栏预留高度。这个值必须 >= 底部实际的控件堆叠高度，否则
       保存/重载那一行会被推到客户区外面（实测 944x700 时曾被推到 y=690）。
       实测累积量：列表间隙 8 + 后端标签 22 + 后端两行 48 + 到AA的间隔 84
                   + AA 行 30 + 按钮行 30 = 222
       所以这里给 176 仍会溢出，留足 200。 */
    bottomH = 200;
    listH = H - top - bottomH - margin;
    if (listH < 100) listH = 100;
    colW = (W - margin * 3) / 2;
    if (colW < 240) colW = 240;

    /* 选项卡 */
    if (g_tab) MoveWindow(g_tab, margin, TAB_Y, W - margin * 2, TAB_H, TRUE);

    MoveWindow(g_listModules, margin, top, colW, listH, TRUE);
    MoveWindow(g_listPatches, margin * 2 + colW, top, colW, listH, TRUE);

    MoveWindow(g_lblMods, margin, LBL_Y, colW, 20, TRUE);
    MoveWindow(g_lblPats, margin * 2 + colW, LBL_Y, colW, 20, TRUE);

    /* 「本机情况」面板：正好占用渲染页空出来的列表区 */
    if (g_lblSysInfo)
        MoveWindow(g_lblSysInfo, margin, top, W - margin * 2, listH, TRUE);

    y = top + listH + 8;

    MoveWindow(g_lblBackend, margin, y, W - margin * 2, 18, TRUE);
    y += 22;

    /* 后端两行 */
    for (i = 0; i < 3; i++)
        MoveWindow(g_radBackend[i], margin + kPosBackend[i].x, y,
                   kPosBackend[i].w, 22, TRUE);
    for (i = 3; i < 6; i++)
        MoveWindow(g_radBackend[i], margin + kPosBackend[i].x, y + 24,
                   kPosBackend[i].w, 22, TRUE);

    MoveWindow(g_btnDeploy, W - margin - 100, y,      100, 26, TRUE);
    MoveWindow(g_btnRecall, W - margin - 100, y + 28, 100, 26, TRUE);
    MoveWindow(g_btnLaunch, W - margin - 100, y + 56, 100, 26, TRUE);
    y += 84;

    /* AA 行 */
    MoveWindow(g_lblAA,     margin + kPosAA[0].x, y, kPosAA[0].w, 22, TRUE);
    MoveWindow(g_radAA[0],  margin + kPosAA[1].x, y, kPosAA[1].w, 22, TRUE);
    MoveWindow(g_radAA[1],  margin + kPosAA[2].x, y, kPosAA[2].w, 22, TRUE);
    MoveWindow(g_radAA[2],  margin + kPosAA[3].x, y, kPosAA[3].w, 22, TRUE);
    MoveWindow(g_lblSSAA,   margin + kPosAA[4].x, y, kPosAA[4].w, 22, TRUE);
    MoveWindow(g_cmbSSAA,   margin + kPosAA[5].x, y, kPosAA[5].w, 140, TRUE);
    MoveWindow(g_lblStrength, margin + kPosAA[6].x, y, kPosAA[6].w, 22, TRUE);
    MoveWindow(g_edStrength,  margin + kPosAA[7].x, y, kPosAA[7].w, 22, TRUE);
    y += rowH;

    MoveWindow(g_btnSave,   margin,        y, 90, 30, TRUE);
    MoveWindow(g_btnAllOn,  margin + 100,  y, 90, 30, TRUE);
    MoveWindow(g_btnAllOff, margin + 200,  y, 90, 30, TRUE);
    MoveWindow(g_btnReload, margin + 300,  y, 90, 30, TRUE);

    if (g_lblStatus)
        MoveWindow(g_lblStatus, margin + 400, y + 4, W - margin * 2 - 400, 24, TRUE);

    /* Language picker: lives on the About tab, anchored bottom-right.
       It used to be created at a fixed y and never laid out again, so it
       floated over every tab. */
    if (g_lblLangSel)
        MoveWindow(g_lblLangSel, W - margin - 320, H - margin - 30, 110, 22, TRUE);
    if (g_cmbLangSel)
        MoveWindow(g_cmbLangSel, W - margin - 205, H - margin - 34, 205, 200, TRUE);

    /* About pane: fill the content area above the status line. */
    if (g_aboutWnd) {
        int aboutH = H - top - 44;
        if (aboutH < 120) aboutH = 120;
        MoveWindow(g_aboutWnd, margin, top, W - margin * 2, aboutH, TRUE);
    }
}

static void UpdateTabVisibility(HWND h)
{
    if (!g_tab) return;
    int sel = (int)SendMessageW(g_tab, TCM_GETCURSEL, 0, 0);
    int i;

    /* Tabs: 0=Modules+Patches (merged) 1=Render backend 2=About

       Strategy: hide EVERY managed control first, then show only the set the
       active tab needs.  The old version listed show/hide per branch and kept
       forgetting controls -- notably the language picker, which stayed on
       screen on every tab. */

    /* --- hide everything --- */
    HWND all[] = {
        g_listModules, g_lblMods, g_listPatches, g_lblPats,
        g_lblBackend, g_lblAA, g_lblSSAA, g_lblStrength, g_lblSysInfo,
        g_cmbSSAA, g_edStrength, g_cmbLangSel, g_lblLangSel,
        g_btnDeploy, g_btnRecall, g_btnLaunch,
        g_btnSave, g_btnAllOn, g_btnAllOff, g_btnReload
    };
    for (i = 0; i < (int)(sizeof(all) / sizeof(all[0])); i++)
        if (all[i]) ShowWindow(all[i], SW_HIDE);
    for (i = 0; i < 6; i++) if (g_radBackend[i]) ShowWindow(g_radBackend[i], SW_HIDE);
    for (i = 0; i < 3; i++) if (g_radAA[i]) ShowWindow(g_radAA[i], SW_HIDE);
    if (g_aboutWnd) ShowWindow(g_aboutWnd, SW_HIDE);

    /* --- show the active tab's set --- */
    if (sel == 0) {
        /* Modules + Patches share one tab: the two lists sit side by side,
           which also fills what used to be a half-empty single-column page. */
        ShowWindow(g_listModules, SW_SHOW);
        ShowWindow(g_lblMods, SW_SHOW);
        ShowWindow(g_listPatches, SW_SHOW);
        ShowWindow(g_lblPats, SW_SHOW);
        ShowWindow(g_btnAllOn, SW_SHOW);
        ShowWindow(g_btnAllOff, SW_SHOW);
        ShowWindow(g_btnSave, SW_SHOW);
        ShowWindow(g_btnReload, SW_SHOW);
    } else if (sel == 1) {
        ShowWindow(g_lblBackend, SW_SHOW);
        ShowWindow(g_lblSysInfo, SW_SHOW);
        for (i = 0; i < 6; i++) if (g_radBackend[i]) ShowWindow(g_radBackend[i], SW_SHOW);
        ShowWindow(g_btnDeploy, SW_SHOW);
        ShowWindow(g_btnRecall, SW_SHOW);
        ShowWindow(g_btnLaunch, SW_SHOW);
        ShowWindow(g_lblAA, SW_SHOW);
        for (i = 0; i < 3; i++) if (g_radAA[i]) ShowWindow(g_radAA[i], SW_SHOW);
        ShowWindow(g_lblSSAA, SW_SHOW);
        ShowWindow(g_cmbSSAA, SW_SHOW);
        ShowWindow(g_lblStrength, SW_SHOW);
        ShowWindow(g_edStrength, SW_SHOW);
        ShowWindow(g_btnSave, SW_SHOW);      /* saving persists backend+AA */
        ShowWindow(g_btnReload, SW_SHOW);
        SyncAAEnabled();
    } else {
        /* About: the painter window fills the content area, and the language
           picker lives here (it used to float over every tab). */
        if (g_aboutWnd) ShowWindow(g_aboutWnd, SW_SHOW);
        ShowWindow(g_lblLangSel, SW_SHOW);
        ShowWindow(g_cmbLangSel, SW_SHOW);
    }

    if (g_lblStatus) ShowWindow(g_lblStatus, SW_SHOW);
    LayoutChildren(h);
}

/* 把所有文案刷到控件上（切语言/重读时调用） */
static void ApplyStrings(HWND h)
{
    wchar_t title[MAX_PATH + 64];

    SetWindowTextW(h, LF("Title"));
    if (g_lblMods)  SetWindowTextW(g_lblMods,  L("LblModules"));
    if (g_lblPats)  SetWindowTextW(g_lblPats,  L("LblPatches"));
    if (g_lblBackend) SetWindowTextW(g_lblBackend, L("LblBackend"));
    if (g_lblSysInfo) SetWindowTextW(g_lblSysInfo, L(""));

    for (int i = 0; i < kBackendCount; i++)
        if (g_radBackend[i]) SetWindowTextW(g_radBackend[i], L(kBackends[i].labelKey));

    if (g_radAA[0]) SetWindowTextW(g_radAA[0], L("AAOff"));
    if (g_radAA[1]) SetWindowTextW(g_radAA[1], L("AASSAA"));
    if (g_radAA[2]) SetWindowTextW(g_radAA[2], L("AAFXAA"));
    if (g_lblSSAA)     SetWindowTextW(g_lblSSAA,     L("LblSSAA"));
    if (g_lblStrength) SetWindowTextW(g_lblStrength, L("LblStrength"));

    if (g_cmbSSAA) {
        int sel = (int)SendMessageW(g_cmbSSAA, CB_GETCURSEL, 0, 0);
        SendMessageW(g_cmbSSAA, CB_RESETCONTENT, 0, 0);
        SendMessageW(g_cmbSSAA, CB_ADDSTRING, 0, (LPARAM)L("AACombo2"));
        SendMessageW(g_cmbSSAA, CB_ADDSTRING, 0, (LPARAM)L("AACombo3"));
        SendMessageW(g_cmbSSAA, CB_ADDSTRING, 0, (LPARAM)L("AACombo4"));
        SendMessageW(g_cmbSSAA, CB_ADDSTRING, 0, (LPARAM)L("AACombo1"));
        SendMessageW(g_cmbSSAA, CB_SETCURSEL, (WPARAM)(sel >= 0 ? sel : 0), 0);
    }

    if (g_btnDeploy) SetWindowTextW(g_btnDeploy, L("BtnDeploy"));
    if (g_btnRecall) SetWindowTextW(g_btnRecall, L("BtnRecall"));
    if (g_btnLaunch) SetWindowTextW(g_btnLaunch, L("BtnLaunch"));
    if (g_btnSave)   SetWindowTextW(g_btnSave,   L("BtnSave"));
    if (g_btnAllOn)  SetWindowTextW(g_btnAllOn,  L("BtnAllOn"));
    if (g_btnAllOff) SetWindowTextW(g_btnAllOff, L("BtnAllOff"));
    if (g_btnReload) SetWindowTextW(g_btnReload, L("BtnReload"));

    if (g_lblStatus) SetWindowTextW(g_lblStatus, L("StReady"));

    /* 列表列头 */
    {
        LVCOLUMNW col = {0};
        col.mask = LVCF_TEXT;
        col.pszText = (LPWSTR)L("ColItem");
        if (g_listModules) SendMessageW(g_listModules, LVM_SETCOLUMNW, 0, (LPARAM)&col);
        if (g_listPatches) SendMessageW(g_listPatches, LVM_SETCOLUMNW, 0, (LPARAM)&col);
    }

    /* 窗口标题 */
    _snwprintf_s(title, _countof(title), _TRUNCATE, L"%s   -   %s", L("Title"), g_dir);
    SetWindowTextW(h, title);

    SyncAAEnabled();

    /* 更新选项卡文本（切语言时刷新）
       注意：不能用 TabCtrl_SetItem 宏 —— 它是按 UNICODE 宏切 A/W 的，
       本工程未定义 UNICODE，宏会退化成 ANSI 版，把宽字符串当单字节读
       → 标签变乱码（|j鵠 / e?N）。直接发 W 消息最稳。 */
    if (g_tab) {
        TCITEMW ti = {0};
        ti.mask = TCIF_TEXT;
        ti.pszText = (LPWSTR)L("TabModules"); SendMessageW(g_tab, TCM_SETITEMW, 0, (LPARAM)&ti);
        ti.pszText = (LPWSTR)L("TabBackend"); SendMessageW(g_tab, TCM_SETITEMW, 1, (LPARAM)&ti);
        ti.pszText = (LPWSTR)L("TabAbout");   SendMessageW(g_tab, TCM_SETITEMW, 2, (LPARAM)&ti);
    }

    UpdateSysInfo();
}

/* 刷新渲染页的「本机情况」面板。
   软光栅后端分 AVX2 / SSE2 两份, 选错会直接崩（0xC000001D 非法指令），
   所以直接把探测结果和推荐项摆出来。 */
static void UpdateSysInfo(void)
{
    wchar_t buf[1400];
    wchar_t cpu[128] = L"?";
    wchar_t gpu[160] = L"";
    char chain[128] = "";
    int avx2, sse2, hwdx9;
    const wchar_t *reco;
    SYSTEM_INFO si;
    MEMORYSTATUSEX ms;
    DWORD cpuLen = sizeof(cpu);

    if (!g_lblSysInfo) return;
    avx2 = CpuHasAvx2();
    sse2 = CpuHasSse2();
    hwdx9 = GpuHasHardwareDx9(gpu, 160);

    /* CPU 型号（注册表） */
    RegGetValueW(HKEY_LOCAL_MACHINE,
                 L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                 L"ProcessorNameString", RRF_RT_REG_SZ, NULL, cpu, &cpuLen);

    GetNativeSystemInfo(&si);
    ZeroMemory(&ms, sizeof(ms));
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);

    if (g_loaderIniA[0])
        GetPrivateProfileStringA("soku_loader", "ChainDll", "", chain, sizeof(chain), g_loaderIniA);

    /* 推荐顺序：有硬件 DX9 就别用 CPU 软光栅。
       只有显卡真做不了 DX9 时，才退而选 CPU，并按指令集挑 AVX2/SSE2。 */
    if (hwdx9)                      reco = L("BackendNative");
    else if (avx2)                  reco = L("BackendCPUAVX2");
    else                            reco = L("BackendCPUSSE2");

    _snwprintf_s(buf, _countof(buf), _TRUNCATE,
        L"%s\n"
        L"  CPU    : %s\n"
        L"  核心   : %u   内存: %.1f GB\n"
        L"  %s: %s\n"
        L"  AVX2   : %s    SSE2: %s\n\n"
        L"  %s : %s\n"
        L"  当前链目标 : %S\n"
        L"  游戏目录   : %s",
        L("LblSysInfo"),
        cpu,
        (unsigned)si.dwNumberOfProcessors,
        (double)ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0),
        L("LblGpu"),
        hwdx9 ? (gpu[0] ? gpu : L("StYes")) : L("StNoHwDx9"),
        avx2 ? L("StYes") : L("StNo"),
        sse2 ? L("StYes") : L("StNo"),
        L("LblRecommended"), reco,
        chain[0] ? chain : "(auto)",
        g_dir);
    SetWindowTextW(g_lblSysInfo, buf);
}

/* ============================================================================
 * 窗口过程
 * ==========================================================================*/
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HWND lm = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            10, 30, 400, 420, h, (HMENU)IDC_MODLIST, NULL, NULL);
        HWND lpl = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            425, 30, 400, 420, h, (HMENU)IDC_PATLIST, NULL, NULL);
        g_listModules = lm;
        g_listPatches = lpl;
        SetupListView(lm);
        SetupListView(lpl);
        /* Use native Explorer theming for listviews for a modern appearance */
        SetWindowTheme(lm, L"Explorer", NULL);
        SetWindowTheme(lpl, L"Explorer", NULL);

        g_lblMods = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            10, 8, 300, 20, h, (HMENU)IDC_LBLM, NULL, NULL);
        g_lblPats = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            425, 8, 300, 20, h, (HMENU)IDC_LBLP, NULL, NULL);
        g_lblBackend = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            10, 448, 420, 18, h, (HMENU)IDC_LBLB, NULL, NULL);

        g_lblLangSel = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            425, 448, 120, 18, h, NULL, NULL, NULL);
        g_cmbLangSel = CreateWindowW(L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 550, 444, 200, 120,
            h, (HMENU)IDC_CMB_LANG, NULL, NULL);

        g_radBackend[0] = CreateWindowW(L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP, 10, 468, 110, 22,
            h, (HMENU)IDC_RB_NATIVE, NULL, NULL);
        g_radBackend[1] = CreateWindowW(L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 122, 468, 130, 22,
            h, (HMENU)IDC_RB_DXVK, NULL, NULL);
        g_radBackend[2] = CreateWindowW(L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 254, 468, 145, 22,
            h, (HMENU)IDC_RB_ON12, NULL, NULL);
        g_radBackend[3] = CreateWindowW(L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 10, 492, 185, 22,
            h, (HMENU)IDC_RB_DGV, NULL, NULL);
        g_radBackend[4] = CreateWindowW(L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 197, 492, 190, 22,
            h, (HMENU)IDC_RB_CPU, NULL, NULL);
        g_radBackend[5] = CreateWindowW(L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 389, 492, 190, 22,
            h, (HMENU)IDC_RB_CPU_SSE2, NULL, NULL);

        g_lblAA = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            10, 516, 90, 22, h, (HMENU)IDC_LBL_AA, NULL, NULL);
        g_radAA[0] = CreateWindowW(L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP, 102, 516, 90, 22,
            h, (HMENU)IDC_RB_AA_OFF, NULL, NULL);
        g_radAA[1] = CreateWindowW(L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 194, 516, 90, 22,
            h, (HMENU)IDC_RB_AA_SSAA, NULL, NULL);
        g_radAA[2] = CreateWindowW(L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 286, 516, 90, 22,
            h, (HMENU)IDC_RB_AA_FXAA, NULL, NULL);
        g_lblSSAA = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            382, 516, 100, 22, h, (HMENU)IDC_LBL_SSAA, NULL, NULL);
        g_cmbSSAA = CreateWindowW(L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 484, 516, 70, 140,
            h, (HMENU)IDC_CMB_SSAA, NULL, NULL);
        g_lblStrength = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            562, 516, 110, 22, h, (HMENU)IDC_LBL_STR, NULL, NULL);
        g_edStrength = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"60",
            WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_RIGHT, 674, 516, 46, 22,
            h, (HMENU)IDC_ED_STRENGTH, NULL, NULL);

        g_btnDeploy = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            728, 466, 100, 26, h, (HMENU)IDC_DEPLOY, NULL, NULL);
        g_btnRecall = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            728, 496, 100, 26, h, (HMENU)IDC_RECALL, NULL, NULL);
        g_btnLaunch = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            728, 526, 100, 26, h, (HMENU)IDC_LAUNCH, NULL, NULL);

        g_btnSave = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            10, 546, 90, 30, h, (HMENU)IDC_SAVE, NULL, NULL);
        g_btnAllOn = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            110, 546, 90, 30, h, (HMENU)IDC_ALLON, NULL, NULL);
        g_btnAllOff = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            210, 546, 90, 30, h, (HMENU)IDC_ALLOFF, NULL, NULL);
        g_btnReload = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            310, 546, 90, 30, h, (HMENU)IDC_RELOAD, NULL, NULL);

        g_lblStatus = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
            425, 550, 400, 24, h, (HMENU)IDC_STATUS, NULL, NULL);

        /* 渲染页的「本机情况」面板 —— 多行只读文本, 显示 CPU 特性与推荐后端。
           软光栅分 AVX2/SSE2 两份, 选错会直接崩, 所以把真相摆出来。 */
        g_lblSysInfo = CreateWindowW(L"STATIC", L"", WS_CHILD | SS_LEFT,
            10, 100, 400, 120, h, NULL, NULL, NULL);

        /* 统一现代字体 */
        {
            HWND all[] = { lm, lpl, g_lblMods, g_lblPats, g_lblBackend,
                           g_radBackend[0], g_radBackend[1], g_radBackend[2],
                           g_radBackend[3], g_radBackend[4], g_radBackend[5],
                           g_lblAA, g_radAA[0], g_radAA[1], g_radAA[2],
                           g_lblSSAA, g_cmbSSAA, g_lblStrength, g_edStrength,
                           g_btnDeploy, g_btnRecall, g_btnLaunch,
                           g_btnSave, g_btnAllOn, g_btnAllOff, g_btnReload,
                           g_lblStatus, g_lblLangSel, g_cmbLangSel, g_lblSysInfo, g_tab };
            for (int i = 0; i < (int)(sizeof(all) / sizeof(all[0])); i++)
                if (all[i]) SendMessageW(all[i], WM_SETFONT, (WPARAM)g_font, TRUE);
        }

        /* Create tab control */
        g_tab = CreateWindowExW(0, WC_TABCONTROLW, L"",
            WS_CHILD | WS_VISIBLE | TCS_HOTTRACK, 10, 10, 800, 30,
            h, (HMENU)IDC_TAB, NULL, NULL);
        if (g_tab) {
            /* Short labels: the long section titles were truncated to a couple of
               characters in the tab strip.
               Send TCM_INSERTITEMW directly -- the TabCtrl_InsertItem macro is
               UNICODE-gated and degrades to the ANSI variant here, which
               mangled the wide strings into mojibake. */
            TCITEMW ti = {0};
            ti.mask = TCIF_TEXT;
            ti.pszText = (LPWSTR)L("TabModules"); SendMessageW(g_tab, TCM_INSERTITEMW, 0, (LPARAM)&ti);
            ti.pszText = (LPWSTR)L("TabBackend"); SendMessageW(g_tab, TCM_INSERTITEMW, 1, (LPARAM)&ti);
            ti.pszText = (LPWSTR)L("TabAbout");   SendMessageW(g_tab, TCM_INSERTITEMW, 2, (LPARAM)&ti);
        }

        ApplyStrings(h);
        UpdateTabVisibility(h);
        /* Populate language list after language dir is known */
        PopulateLanguageList(h);

        LoadModules();
        LoadPatches();
        LoadBackend();
        LoadAA();
        CheckRadioButton(h, IDC_RB_NATIVE, IDC_RB_CPU_SSE2, IDC_RB_NATIVE + g_backend);
        CheckRadioButton(h, IDC_RB_AA_OFF, IDC_RB_AA_FXAA, IDC_RB_AA_OFF + g_aaMode);
        SendMessageW(g_cmbSSAA, CB_SETCURSEL, (WPARAM)SSAALevelToIndex(g_ssaaLevel), 0);
        { wchar_t wb[16]; _snwprintf_s(wb, 16, _TRUNCATE, L"%d", g_fxaaStrength);
          SetWindowTextW(g_edStrength, wb); }
        SyncAAEnabled();
        FillList(lm, g_modules);
        FillList(lpl, g_patches);
        SetStatus(LF("StLoaded"), (int)g_modules.size(), (int)g_patches.size(),
                  L(kBackends[g_backend].labelKey), AAName());
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_CMB_LANG:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(g_cmbLangSel, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < (int)g_langFileList.size()) {
                    /* Load the chosen file BY ITS REAL PATH.  Do not build a
                       name from the code: filename and meta.Code drift apart
                       (en-PT.ini declares Code=en-PIRATE), and a拼出来
                       的路径会直接找不到文件 —— 这就是本地化"用不了"的原因。 */
                    const std::wstring &full = g_langFileList[sel];
                    const std::string code = (sel < (int)g_langCodes.size()) ? g_langCodes[sel] : std::string();
                    LoadLanguageByFile(full.c_str(), code.c_str());
                    /* write Language=code back to soku_loader.ini to persist choice */
                    if (code.size() > 0) {
                        WritePrivateProfileStringA("soku_loader", "Language", code.c_str(), g_loaderIniA);
                    }
                    /* After loading language, refresh strings */
                    ApplyStrings(h);
                    UpdateTabVisibility(h);
                }
            }
            break;

        case IDC_RB_NATIVE:   g_backend = 0; SyncAAEnabled(); SetStatus(LF("StBackendSet"), L(kBackends[0].labelKey)); break;
        case IDC_RB_DXVK:     g_backend = 1; SyncAAEnabled(); SetStatus(LF("StBackendSet"), L(kBackends[1].labelKey)); break;
        case IDC_RB_ON12:     g_backend = 2; SyncAAEnabled(); SetStatus(LF("StBackendSet"), L(kBackends[2].labelKey)); break;
        case IDC_RB_DGV:      g_backend = 3; SyncAAEnabled(); SetStatus(LF("StBackendSet"), L(kBackends[3].labelKey)); break;
        case IDC_RB_CPU:      g_backend = 4; SyncAAEnabled(); SetStatus(LF("StBackendSet"), L(kBackends[4].labelKey)); break;
        case IDC_RB_CPU_SSE2: g_backend = 5; SyncAAEnabled(); SetStatus(LF("StBackendSet"), L(kBackends[5].labelKey)); break;

        case IDC_RB_AA_OFF:  g_aaMode = 0; SaveAA(); SetStatus(LF("StAASet"), AAName()); break;
        case IDC_RB_AA_SSAA: g_aaMode = 1; SaveAA(); SetStatus(LF("StAASetSSAA"), g_ssaaLevel); break;
        case IDC_RB_AA_FXAA: g_aaMode = 2; SaveAA(); SetStatus(LF("StAASetFXAA"), g_fxaaStrength); break;

        case IDC_CMB_SSAA:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(g_cmbSSAA, CB_GETCURSEL, 0, 0);
                g_ssaaLevel = SSAAIndexToLevel(sel);
                SaveAA();
                SetStatus(LF("StSSAASet"), g_ssaaLevel);
            }
            break;

        case IDC_ED_STRENGTH:
            if (HIWORD(wp) == EN_KILLFOCUS) {
                wchar_t wb[16] = L"";
                GetWindowTextW(g_edStrength, wb, 16);
                int v = _wtoi(wb);
                if (v < 0) v = 0; if (v > 100) v = 100;
                g_fxaaStrength = v;
                _snwprintf_s(wb, 16, _TRUNCATE, L"%d", v);
                SetWindowTextW(g_edStrength, wb);
                SaveAA();
                SetStatus(LF("StStrengthSet"), v);
            }
            break;

        case IDC_DEPLOY: {
            wchar_t msg[256] = L"";
            const BackendDef *bd = &kBackends[g_backend];

            /* 部署前提示 —— 两个 CPU 后端对 CPU 指令集的要求不同，选错会直接崩。 */
            if (BackendIsCPU(g_backend)) {
                const wchar_t *head = (strstr(bd->chain, "avx2") != NULL)
                                    ? L("WarnAVX2Title") : L("WarnSSE2Title");
                const wchar_t *body = (strstr(bd->chain, "avx2") != NULL)
                                    ? L("WarnAVX2Body")  : L("WarnSSE2Body");
                if (MessageBoxW(h, body, head,
                                MB_ICONINFORMATION | MB_OKCANCEL | MB_DEFBUTTON1) != IDOK)
                    break;
            }

            SaveBackend();
            int n = DeployBackend(g_backend, msg, sizeof(msg) / sizeof(msg[0]));
            SetStatus(LF("StDeployed"), L(bd->labelKey), msg);
            (void)n;
            break;
        }

        case IDC_RECALL:
            CleanBackendFiles();
            SetStatus(L("%s"), L("StRecalled"));
            break;

        case IDC_LAUNCH: {
            wchar_t exe[MAX_PATH];
            _snwprintf_s(exe, MAX_PATH, _TRUNCATE, L"%s\\th123.exe", g_dir);
            if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
                SetStatus(L("%s"), L("StNoExe"));
            } else {
                STARTUPINFOW si = {0}; si.cb = sizeof(si);
                PROCESS_INFORMATION pi = {0};
                if (CreateProcessW(exe, NULL, NULL, NULL, FALSE, 0, NULL, g_dir, &si, &pi)) {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    SetStatus(L("%s"), L("StLaunched"));
                } else {
                    SetStatus(LF("StLaunchFail"), (int)GetLastError());
                }
            }
            break;
        }

        case IDC_SAVE:
            SyncFromList(g_listModules, g_modules);
            SyncFromList(g_listPatches, g_patches);
            SaveAll();
            SaveBackend();
            SaveAA();
            break;

        case IDC_RELOAD:
            LoadModules(); LoadPatches(); LoadBackend(); LoadAA();
            CheckRadioButton(h, IDC_RB_NATIVE, IDC_RB_CPU_SSE2, IDC_RB_NATIVE + g_backend);
            CheckRadioButton(h, IDC_RB_AA_OFF, IDC_RB_AA_FXAA, IDC_RB_AA_OFF + g_aaMode);
            SendMessageW(g_cmbSSAA, CB_SETCURSEL, (WPARAM)SSAALevelToIndex(g_ssaaLevel), 0);
            { wchar_t wb[16]; _snwprintf_s(wb, 16, _TRUNCATE, L"%d", g_fxaaStrength);
              SetWindowTextW(g_edStrength, wb); }
            SyncAAEnabled();
            FillList(g_listModules, g_modules);
            FillList(g_listPatches, g_patches);
            SetStatus(LF("StReloaded"), L(kBackends[g_backend].labelKey), AAName());
            break;

        case IDC_ALLON:
            for (int i = 0; i < ListView_GetItemCount(g_listModules); i++)
                ListView_SetCheckState(g_listModules, i, TRUE);
            SetStatus(L("%s"), L("StAllOn"));
            break;

        case IDC_ALLOFF:
            for (int i = 0; i < ListView_GetItemCount(g_listModules); i++)
                ListView_SetCheckState(g_listModules, i, FALSE);
            SetStatus(L("%s"), L("StAllOff"));
            break;
        }
        return 0;

    case WM_NOTIFY:
    {
        LPNMHDR nh = (LPNMHDR)lp;
        if (nh->hwndFrom == g_tab && nh->code == TCN_SELCHANGE) {
            UpdateTabVisibility(h);
            return 0;
        }
        break;
    }

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        RECT r = { 0, 0, 800, 700 };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        mmi->ptMinTrackSize.x = r.right - r.left;
        mmi->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) LayoutChildren(h);
        return 0;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
        /* 让静态文本用透明背景, 视觉上更干净 (配合对话框底色) */
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* 现代字体：优先 Segoe UI（并让雅黑兜底中文），退化到系统消息字体 */
static HFONT MakeUiFont(void)
{
    NONCLIENTMETRICSW ncm;
    HFONT f = NULL;

    ZeroMemory(&ncm, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        /* 系统消息字体本身就是 Segoe UI / 微软雅黑，跟着系统走最稳 */
        f = CreateFontIndirectW(&ncm.lfMessageFont);
    }
    if (!f) {
        f = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                        L"Segoe UI");
    }
    return f;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR cmdLine, int)
{
    /* Improve DPI handling on modern displays */
    SetProcessDPIAware();

    INITCOMMONCONTROLSEX icc = {0};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);

    /* 默认游戏目录 = exe 自己所在的目录。
       管理器就住在游戏根目录里，自己定位自己即可 —— 这样整包解压到
       任何位置都能直接用，不需要安装器，也不再依赖 D:\th123 这个
       开发机遗留的写死默认值。命令行参数仍可覆盖。 */
    {
        wchar_t self[MAX_PATH];
        if (GetModuleFileNameW(NULL, self, MAX_PATH)) {
            wchar_t *slash = wcsrchr(self, L'\\');
            if (slash) {
                *slash = 0;
                wcsncpy_s(g_dir, MAX_PATH, self, _TRUNCATE);
            }
        }
    }

    /* 命令行可指定游戏目录（覆盖上面的自定位） */
    if (cmdLine && *cmdLine) {
        wchar_t w[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, cmdLine, -1, w, MAX_PATH);
        if (w[0] == L'"') {
            memmove(w, w + 1, sizeof(w) - sizeof(wchar_t));
            size_t n = wcslen(w);
            if (n && w[n - 1] == L'"') w[n - 1] = 0;
        }
        if (GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES)
            wcsncpy_s(g_dir, MAX_PATH, w, _TRUNCATE);
    }
    InitPaths(g_dir);
    InitLang();
    EnsurePatchLabels();
    g_font = MakeUiFont();

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"SokuConfigWnd";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);

    /* Register About pane class for showing the image */
    WNDCLASSEXW awc = {0};
    awc.cbSize = sizeof(awc);
    awc.lpfnWndProc = AboutWndProc;
    awc.hInstance = hInst;
    awc.hCursor = LoadCursor(NULL, IDC_ARROW);
    awc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    awc.lpszClassName = L"AboutPane";
    RegisterClassExW(&awc);

    HWND h = CreateWindowExW(0, L"SokuConfigWnd", L"",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 960, 720,
        NULL, NULL, hInst, NULL);
    if (!h) return 1;

    /* Load About info from soku_loader.ini if present */
    LoadAboutInfoFromIni();

    /* Initialize GDI+ and try to load about image from backends\About.* (jpeg preferred) */
    Gdiplus::GdiplusStartupInput gsi;
    if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &gsi, NULL) == Gdiplus::Ok) {
        const wchar_t *candidates[] = { L"About.jpeg", L"About.jpg", L"About.png", L"About.bmp" };
        wchar_t imgPath[MAX_PATH];
        for (int ci = 0; ci < (int)(sizeof(candidates)/sizeof(candidates[0])); ci++) {
            _snwprintf_s(imgPath, MAX_PATH, _TRUNCATE, L"%s\\backends\\%s", g_dir, candidates[ci]);
            if (GetFileAttributesW(imgPath) == INVALID_FILE_ATTRIBUTES) continue;
            Gdiplus::Image *img = Gdiplus::Image::FromFile(imgPath);
            if (img && img->GetLastStatus() == Gdiplus::Ok) { g_aboutImage = img; break; }
            if (img) { delete img; }
        }
    }

    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);

    /* Create About pane window but keep it hidden; parent it to main window so it participates in layout */
    g_aboutWnd = CreateWindowExW(0, L"AboutPane", L"",
        WS_CHILD | WS_VISIBLE,
        10, 60, 300, 200, h, NULL, hInst, NULL);
    if (g_aboutWnd) ShowWindow(g_aboutWnd, SW_HIDE);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_font) DeleteObject(g_font);
    if (g_aboutImage) { delete g_aboutImage; g_aboutImage = NULL; }
    if (g_gdiplusToken) Gdiplus::GdiplusShutdown(g_gdiplusToken);
    return 0;
}
