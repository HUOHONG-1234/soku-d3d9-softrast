/*
 * soku_config.exe —— 非想天则模组配置器 (Win32 原生 GUI)
 *
 * 为什么用 Win32 原生而不是 Electron/WPF:
 *   目标机器是 Th123 的安装目录, 单文件 exe 丢进去就能跑。
 *   无运行库依赖 (静态链接), 无需 .NET, 无需 node。
 *
 * 管两套配置:
 *   左: SokuEngine.ini  [Modules]  —— 36 个模组的开/关
 *   右: MemoryPatch.ini [Section]Enabled —— 画面/特效补丁
 *
 * 用法: soku_config.exe [游戏目录]   默认 D:\th123
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")

/* ---------------- 全局 ---------------- */
static wchar_t g_dir[MAX_PATH]      = L"D:\\th123";
static wchar_t g_engineIni[MAX_PATH] = L"";
static wchar_t g_patchIni[MAX_PATH]  = L"";
static char    g_engineIniA[MAX_PATH] = "";   /* ANSI 缓存, 避免临时对象悬垂 */
static char    g_patchIniA[MAX_PATH]  = "";

static wchar_t g_loaderIni[MAX_PATH] = L"";
static char    g_loaderIniA[MAX_PATH] = "";
static int     g_backend = 1;   /* 索引到 kBackends[] */
static HWND g_listModules = NULL;
static HWND g_listPatches = NULL;
static HWND g_lblStatus   = NULL;
static HWND g_radBackend[5] = { NULL, NULL, NULL, NULL, NULL };
/* 需要参与布局的控件（窗口缩放时要重新摆位）*/
static HWND g_lblMods = NULL, g_lblPats = NULL, g_lblBackend = NULL;
static HWND g_btnDeploy = NULL, g_btnRecall = NULL;
static HWND g_btnSave = NULL, g_btnAllOn = NULL, g_btnAllOff = NULL, g_btnReload = NULL;

struct Item {
    std::string key;      // ini 里的键名 / 节名
    std::string label;    // 界面显示
    bool        enabled;
};

static std::vector<Item> g_modules;
static std::vector<Item> g_patches;

/* 固定表: MemoryPatch 的可用补丁 (从 MemoryPatch.ini 实测) */
static const char *kPatchKeys[] = {
    "16bitsColor", "DisableWeatherEffect", "HideHUD", "AllowMultiInstance",
    "AlwaysRandomMusic", "DefaultDenyWatch", "DefaultDenyBattle",
    "InputFreedom", "FilesystemFirst", "NativeDPadFix"
};
static const char *kPatchLabels[] = {
    "16bits 色深 (复古画面)", "关闭全部天气特效", "隐藏游戏 HUD", "允许多开游戏",
    "永远随机关卡音乐", "默认禁止观战", "默认观战模式",
    "无焦点也接收手柄输入", "优先从文件系统读资源", "用手柄十字键而非摇杆"
};
static const int kPatchCount = 10;

/* ---------------- backend file manifest ----------------
   Each backend's DLLs/conf live under backends\<dir>\; only the selected
   set is copied next to the game executable, so multiple d3d9-class DLLs
   never conflict in the root directory. */

struct BFile { const char *src; const char *dst; };
struct BackendDef {
    const char *dir;      /* backends\ 下的子目录 */
    const char *label;    /* 界面显示 */
    const char *chain;    /* soku_loader.ini ChainDll */
    const char *on12;     /* soku_loader.ini UseOn12 */
    int         nFiles;
    BFile       files[6];
};

static const BackendDef kBackends[] = {
    { "native", "原生 D3D9", "", "0", 0, {} },
    { "dxvk", "DXVK (Vulkan)", "dxvk_d3d9.dll", "0", 4, {
        { "dxvk_d3d9.dll", "dxvk_d3d9.dll" },
        { "d3d11.dll",     "d3d11.dll" },
        { "dxgi.dll",      "dxgi.dll" },
        { "dxvk.conf",     "dxvk.conf" } } },
    { "on12", "D3D9On12 (D3D12)", "", "1", 0, {} },
    { "dgvoodoo2", "dgVoodoo2 (D3D11)", "dgvoodoo_d3d9.dll", "0", 2, {
        { "dgvoodoo_d3d9.dll", "dgvoodoo_d3d9.dll" },
        { "dgVoodoo.conf",     "dgVoodoo.conf" } } },
    /* 【实验性】CPU 软光栅。已知问题(2026-09-16):
       1. 未抠干净黑底的素材仍会露黑边 (灵乌路空等)
       2. 不支持全屏模式
       3. 帧率: 轻场景 60 / 重场景 35
       性能本身没问题(光栅 2.08ms/帧, 6线程); 是渲染正确性未收敛。 */
    { "cpu", "[实验] CPU 渲染 (软光栅)", "cpu_d3d9.dll", "0", 3, {
        { "cpu_d3d9.dll",  "cpu_d3d9.dll" },
        { "d3dx9_33.dll",  "d3dx9_33.dll" },
        { "d3dx9_real.dll","d3dx9_real.dll" } } },
};
/* Table length is derived, never hardcoded.
   A previous version kept a stale constant after adding a backend, which
   made kBackends[n] read out of bounds and crashed on selection. */
static const int kBackendCount = sizeof(kBackends) / sizeof(kBackends[0]);

/* ---------------- 工具 ---------------- */
static void SetStatus(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    wchar_t wbuf[512];
    MultiByteToWideChar(CP_ACP, 0, buf, -1, wbuf, 512);
    SetWindowTextW(g_lblStatus, wbuf);
}

static void InitPaths(const wchar_t *dir)
{
    wcsncpy_s(g_dir, MAX_PATH, dir, _TRUNCATE);
    _snwprintf_s(g_engineIni, MAX_PATH, _TRUNCATE, L"%s\\modules\\SokuEngine\\SokuEngine.ini", g_dir);
    _snwprintf_s(g_patchIni,  MAX_PATH, _TRUNCATE, L"%s\\modules\\MemoryPatch\\MemoryPatch.ini", g_dir);
    _snwprintf_s(g_loaderIni, MAX_PATH, _TRUNCATE, L"%s\\soku_loader.ini", g_dir);
    WideCharToMultiByte(CP_ACP, 0, g_engineIni, -1, g_engineIniA, MAX_PATH, NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, g_patchIni,  -1, g_patchIniA,  MAX_PATH, NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, g_loaderIni, -1, g_loaderIniA, MAX_PATH, NULL, NULL);
}

/* 读渲染后端（按零件清单反查索引）*/
static void LoadBackend(void)
{
    char chain[128] = "";
    GetPrivateProfileStringA("soku_loader", "ChainDll", "", chain, sizeof(chain), g_loaderIniA);
    int on12 = GetPrivateProfileIntA("soku_loader", "UseOn12", 0, g_loaderIniA);
    int i;

    g_backend = 0;
    if (on12) {
        for (i = 0; i < kBackendCount; i++)
            if (strcmp(kBackends[i].on12, "1") == 0) { g_backend = i; return; }
        return;
    }
    if (chain[0]) {
        for (i = 0; i < kBackendCount; i++)
            if (kBackends[i].chain[0] && strstr(chain, kBackends[i].chain)) { g_backend = i; return; }
    }
}

/* 写渲染后端配置（不搬文件）*/
static void SaveBackend(void)
{
    if (g_backend < 0 || g_backend >= kBackendCount) g_backend = 0;
    const BackendDef *bd = &kBackends[g_backend];
    WritePrivateProfileStringA("soku_loader", "ChainDll", bd->chain, g_loaderIniA);
    WritePrivateProfileStringA("soku_loader", "UseOn12",  bd->on12,  g_loaderIniA);
}

/* 清掉根目录里所有已知后端文件（不动 loader 自己的 d3d9.dll）*/
static void CleanBackendFiles(void)
{
    wchar_t p[MAX_PATH];
    int b, f;
    for (b = 0; b < kBackendCount; b++) {
        for (f = 0; f < kBackends[b].nFiles; f++) {
            _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%s\\%S", g_dir, kBackends[b].files[f].dst);
            DeleteFileW(p);
        }
    }
}

/* 把选中的后端从 backends\<dir> 部署到游戏根目录 */
static int DeployBackend(int idx, char *msg, size_t msgLen)
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
            _snprintf_s(msg, msgLen, _TRUNCATE, "缺少 backends\\%S\\%S",
                        bd->dir, bd->files[f].src);
            return copied;
        }
    }
    _snprintf_s(msg, msgLen, _TRUNCATE, "已部署 %d 个文件", copied);
    return copied;
}

/* ---------------- 读配置 ---------------- */
static void LoadModules(void)
{
    g_modules.clear();

    char buf[16384];
    DWORD n = GetPrivateProfileSectionA("Modules", buf, sizeof(buf), g_engineIniA);
    if (n == 0) { SetStatus("读不到 SokuEngine.ini [Modules] 段"); return; }

    char *p = buf;
    while (*p) {
        char *eq = strchr(p, '=');
        if (eq) {
            Item it;
            it.key.assign(p, eq - p);
            while (!it.key.empty() && (it.key.back() == ' ' || it.key.back() == '\t')) it.key.pop_back();
            const char *v = eq + 1;
            while (*v == ' ' || *v == '\t') v++;
            it.enabled = (atoi(v) != 0);
            it.label = it.key;
            g_modules.push_back(it);
        }
        p += strlen(p) + 1;
    }
    SetStatus("已读取 %d 个模组", (int)g_modules.size());
}

static void LoadPatches(void)
{
    g_patches.clear();

    for (int i = 0; i < kPatchCount; i++) {
        Item it;
        it.key = kPatchKeys[i];
        it.label = kPatchLabels[i];
        it.enabled = GetPrivateProfileIntA(it.key.c_str(), "Enabled", 0, g_patchIniA) != 0;
        g_patches.push_back(it);
    }
}

/* ---------------- 写配置 ---------------- */
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
    SetStatus("已保存 %d 项 (重启游戏生效)", changed);
}

/* ---------------- 列表填充 ---------------- */
static void FillList(HWND lv, const std::vector<Item> &items)
{
    ListView_DeleteAllItems(lv);
    for (size_t i = 0; i < items.size(); i++) {
        LVITEMA it = {0};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = (LPSTR)items[i].label.c_str();
        int idx = (int)SendMessageA(lv, LVM_INSERTITEMA, 0, (LPARAM)&it);
        ListView_SetCheckState(lv, idx, items[i].enabled ? TRUE : FALSE);
    }
}

static void SyncFromList(HWND lv, std::vector<Item> &items)
{
    for (size_t i = 0; i < items.size(); i++) {
        items[i].enabled = ListView_GetCheckState(lv, (int)i) ? true : false;
    }
}

/* ---------------- 窗口过程 ---------------- */
#define IDC_MODLIST   1001
#define IDC_PATLIST   1002
#define IDC_SAVE      1003
#define IDC_ALLON     1004
#define IDC_ALLOFF    1005
#define IDC_RELOAD    1006
#define IDC_STATUS    1007
#define IDC_LBLM      1008
#define IDC_LBLP      1009
#define IDC_LBLB      1010
/* 单选按钮 ID 必须【连续】—— CheckRadioButton(h, first, last, check) 靠范围扫。
   之前 DEPLOY/RECALL 夹在中间, 导致 IDC_RB_NATIVE+5 算出来指向 DEPLOY。 */
#define IDC_RB_NATIVE 1011
#define IDC_RB_DXVK   1012
#define IDC_RB_ON12   1013
#define IDC_RB_DGV    1014
#define IDC_RB_CPU    1015
#define IDC_DEPLOY    1017
#define IDC_RECALL    1018

static void SetupListView(HWND lv)
{
    SendMessageW(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    LVCOLUMNW col = {0};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = (LPWSTR)L"项目";
    col.cx = 300;
    SendMessageW(lv, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
}

/* 统一布局：列表随窗口伸缩，底栏锚在底部。
   之前的写死坐标在 840x600 下把后端按钮挤出了客户区。 */
static void LayoutChildren(HWND h)
{
    RECT rc;
    int W, H, margin = 10, top, bottomH = 170, listH, colW, y;

    GetClientRect(h, &rc);
    W = rc.right; H = rc.bottom;
    if (W < 620) W = 620;

    top = 30;
    listH = H - top - bottomH - margin;
    if (listH < 120) listH = 120;
    colW = (W - margin * 3) / 2;
    if (colW < 240) colW = 240;

    MoveWindow(g_listModules, margin, top, colW, listH, TRUE);
    MoveWindow(g_listPatches, margin * 2 + colW, top, colW, listH, TRUE);

    MoveWindow(g_lblMods, margin, 8, colW, 20, TRUE);
    MoveWindow(g_lblPats, margin * 2 + colW, 8, colW, 20, TRUE);

    y = top + listH + 8;

    MoveWindow(g_lblBackend, margin, y, W - margin * 2, 18, TRUE);
    y += 22;

    /* 五个后端分两行，不再横向挤 */
    MoveWindow(g_radBackend[0], margin,        y,      110, 22, TRUE);
    MoveWindow(g_radBackend[1], margin + 112,  y,      130, 22, TRUE);
    MoveWindow(g_radBackend[2], margin + 244,  y,      145, 22, TRUE);
    MoveWindow(g_radBackend[3], margin,        y + 24, 185, 22, TRUE);
    MoveWindow(g_radBackend[4], margin + 187,  y + 24, 180, 22, TRUE);

    MoveWindow(g_btnDeploy, W - margin - 100, y,      100, 26, TRUE);
    MoveWindow(g_btnRecall, W - margin - 100, y + 28, 100, 26, TRUE);
    y += 56;

    MoveWindow(g_btnSave,   margin,        y, 90, 30, TRUE);
    MoveWindow(g_btnAllOn,  margin + 100,  y, 90, 30, TRUE);
    MoveWindow(g_btnAllOff, margin + 200,  y, 90, 30, TRUE);
    MoveWindow(g_btnReload, margin + 300,  y, 90, 30, TRUE);

    if (g_lblStatus)
        MoveWindow(g_lblStatus, margin + 400, y + 4, W - margin * 2 - 400, 24, TRUE);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

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

        HWND a = CreateWindowW(L"STATIC", L"模组开关 (SokuEngine.ini)",
            WS_CHILD | WS_VISIBLE, 10, 8, 300, 20, h, (HMENU)IDC_LBLM, NULL, NULL);
        HWND b = CreateWindowW(L"STATIC", L"画面/特效补丁 (MemoryPatch.ini)",
            WS_CHILD | WS_VISIBLE, 425, 8, 300, 20, h, (HMENU)IDC_LBLP, NULL, NULL);
        g_lblMods = a; g_lblPats = b;

        /* 渲染后端选择 */
        HWND lb = CreateWindowW(L"STATIC", L"渲染后端 (选中后点「部署」把零件放进根目录)",
            WS_CHILD | WS_VISIBLE, 10, 448, 420, 18, h, (HMENU)IDC_LBLB, NULL, NULL);
        g_lblBackend = lb;
        HWND rb1 = CreateWindowW(L"BUTTON", L"原生 D3D9",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP, 10, 468, 110, 22, h, (HMENU)IDC_RB_NATIVE, NULL, NULL);
        HWND rb2 = CreateWindowW(L"BUTTON", L"DXVK (Vulkan)",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 122, 468, 130, 22, h, (HMENU)IDC_RB_DXVK, NULL, NULL);
        HWND rb3 = CreateWindowW(L"BUTTON", L"D3D9On12 (D3D12)",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 254, 468, 145, 22, h, (HMENU)IDC_RB_ON12, NULL, NULL);
        HWND rb4 = CreateWindowW(L"BUTTON", L"dgVoodoo2 (D3D11)",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 401, 468, 155, 22, h, (HMENU)IDC_RB_DGV, NULL, NULL);
        HWND rb6 = CreateWindowW(L"BUTTON", L"[实验] CPU 渲染 (软光栅)",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 10, 492, 175, 22, h, (HMENU)IDC_RB_CPU, NULL, NULL);
        g_radBackend[0] = rb1; g_radBackend[1] = rb2; g_radBackend[2] = rb3;
        g_radBackend[3] = rb4; g_radBackend[4] = rb6;

        HWND d1 = CreateWindowW(L"BUTTON", L"部署后端",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 728, 466, 90, 26, h, (HMENU)IDC_DEPLOY, NULL, NULL);
        HWND d2 = CreateWindowW(L"BUTTON", L"全部回收",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 728, 496, 90, 26, h, (HMENU)IDC_RECALL, NULL, NULL);
        g_btnDeploy = d1; g_btnRecall = d2;

        HWND s1 = CreateWindowW(L"BUTTON", L"保存",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 502, 90, 30, h, (HMENU)IDC_SAVE, NULL, NULL);
        HWND s2 = CreateWindowW(L"BUTTON", L"左列全开",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 110, 502, 90, 30, h, (HMENU)IDC_ALLON, NULL, NULL);
        HWND s3 = CreateWindowW(L"BUTTON", L"左列全关",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 210, 502, 90, 30, h, (HMENU)IDC_ALLOFF, NULL, NULL);
        HWND s4 = CreateWindowW(L"BUTTON", L"重新读取",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 310, 502, 90, 30, h, (HMENU)IDC_RELOAD, NULL, NULL);

        HWND st = CreateWindowW(L"STATIC", L"就绪",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 425, 506, 400, 24, h, (HMENU)IDC_STATUS, NULL, NULL);
        g_lblStatus = st;
        g_btnSave = s1; g_btnAllOn = s2; g_btnAllOff = s3; g_btnReload = s4;

        HWND all[] = { lm, lpl, a, b, lb, rb1, rb2, rb3, rb4, rb6, d1, d2, s1, s2, s3, s4, st };
        for (int ai = 0; ai < (int)(sizeof(all) / sizeof(all[0])); ai++)
            SendMessageW(all[ai], WM_SETFONT, (WPARAM)font, TRUE);

        LayoutChildren(h);

        LoadModules();
        LoadPatches();
        LoadBackend();
        CheckRadioButton(h, IDC_RB_NATIVE, IDC_RB_CPU, IDC_RB_NATIVE + g_backend);
        FillList(lm, g_modules);
        FillList(lpl, g_patches);
        SetStatus("已载入 模组 %d 项 / 补丁 %d 项 / 后端: %S",
                  (int)g_modules.size(), (int)g_patches.size(), kBackends[g_backend].label);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_RB_NATIVE: g_backend = 0; SetStatus("渲染后端 -> %S", kBackends[0].label); break;
        case IDC_RB_DXVK:   g_backend = 1; SetStatus("渲染后端 -> %S", kBackends[1].label); break;
        case IDC_RB_ON12:   g_backend = 2; SetStatus("渲染后端 -> %S", kBackends[2].label); break;
        case IDC_RB_DGV:    g_backend = 3; SetStatus("渲染后端 -> %S", kBackends[3].label); break;
        case IDC_RB_CPU:    g_backend = 4; SetStatus("渲染后端 -> %S", kBackends[4].label); break;
        case IDC_DEPLOY: {
            char msg[256] = "";
            SaveBackend();
            int n = DeployBackend(g_backend, msg, sizeof(msg));
            SetStatus("后端 %S: %s", kBackends[g_backend].label, msg);
            (void)n;
            break;
        }
        case IDC_RECALL:
            CleanBackendFiles();
            SetStatus("已回收根目录下的全部后端文件 (仅剩加载器 d3d9.dll)");
            break;
        case IDC_SAVE:
            SyncFromList(g_listModules, g_modules);
            SyncFromList(g_listPatches, g_patches);
            SaveAll();
            break;
        case IDC_RELOAD:
            LoadModules(); LoadPatches(); LoadBackend();
            CheckRadioButton(h, IDC_RB_NATIVE, IDC_RB_CPU, IDC_RB_NATIVE + g_backend);
            FillList(g_listModules, g_modules);
            FillList(g_listPatches, g_patches);
            SetStatus("已重新读取 (后端: %S)", kBackends[g_backend].label);
            break;
        case IDC_ALLON:
            for (int i = 0; i < ListView_GetItemCount(g_listModules); i++)
                ListView_SetCheckState(g_listModules, i, TRUE);
            SetStatus("左列已全选 (记得点保存)");
            break;
        case IDC_ALLOFF:
            for (int i = 0; i < ListView_GetItemCount(g_listModules); i++)
                ListView_SetCheckState(g_listModules, i, FALSE);
            SetStatus("左列已全不选 (记得点保存)");
            break;
        }
        return 0;

    case WM_GETMINMAXINFO:
    {
        /* 底栏是固定高度的，窗口再矮就要重叠 —— 把最小值钉死。
           客户区至少 760x620，换算成外框尺寸。 */
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        RECT r = { 0, 0, 760, 620 };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        mmi->ptMinTrackSize.x = r.right - r.left;
        mmi->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) LayoutChildren(h);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR cmdLine, int)
{
    INITCOMMONCONTROLSEX icc = {0};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    /* 命令行可指定游戏目录 */
    if (cmdLine && *cmdLine) {
        wchar_t w[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, cmdLine, -1, w, MAX_PATH);
        // 去掉首尾引号
        if (w[0] == L'"') { memmove(w, w + 1, sizeof(w) - sizeof(wchar_t)); 
                            size_t n = wcslen(w); if (n && w[n-1] == L'"') w[n-1] = 0; }
        if (GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES)
            wcsncpy_s(g_dir, MAX_PATH, w, _TRUNCATE);
    }
    InitPaths(g_dir);

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"SokuConfigWnd";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);

    wchar_t title[MAX_PATH + 40];
    _snwprintf_s(title, _countof(title), _TRUNCATE,
        L"非想天则 模组配置器   —   %s", g_dir);

    HWND h = CreateWindowExW(0, L"SokuConfigWnd", title,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 920, 700,
        NULL, NULL, hInst, NULL);
    if (!h) return 1;
    /* 最小尺寸：低于这个再缩放会把底栏压掉 */
    {
        RECT r = { 0, 0, 760, 620 };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(h, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
