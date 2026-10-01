/*
 * shim.cpp -- D3D9 device shim backed by a CPU software rasterizer.
 *
 * Pipeline:  th123.exe -> [this DLL] -> CPU rasterizer -> bitmap
 *            -> real D3D9 device -> GPU (display only)
 *
 *   COM 接口的虚表槽位 = 方法声明顺序。
 *   所以类体里只能按 d3d9.h 的声明顺序摆方法, 一个都不能挪。
 *   特殊实现全部做成自由函数 (H9_/HD_/HS_/HF_ 前缀), 由生成器产出的
 *   方法体以正确顺序转发过去 —— 手写方法插在 #include 之前会让整张表错位。
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* CPU 软光栅核心*/
#include "softrast.h"

/* ================= 日志 ================= */
static FILE *g_log = NULL;
static FILE *g_callLog = NULL;
static int   g_callCount = 0;

static void LogF(const char *fmt, ...);

/* 纹理绑定统计（必须放在最前，DumpStatsToFile 要用） */
static int   g_nullTex = 0;
static int   g_realTex = 0;
static int   g_frameCount = 0;      /* 帧计数: DumpStatsToFile 算帧率要用, 必须提前声明 */
static unsigned int g_lastClear = 0;

/* ================= 性能分解 =================
   为什么要这个: 前后两次跑出的帧率对不上 (49.28 -> 38.56),
   光看总帧率分不清是【光栅慢】还是【拷贝/呈现慢】还是【等垂直同步】。
   这里分别计时, 加总后才能定位真正的瓶颈。 */
static LARGE_INTEGER g_qpcFreq = { 0 };
static long long g_ticksDraw    = 0;   /* DrawPrimitiveUP 累计 (光栅化) */
static long long g_ticksPresent = 0;   /* Core_Present 累计 (拷贝+翻页+等待) */
static long long g_ticksConvert = 0;   /* 纹理转换累计 */
static long long g_ticksClear   = 0;   /* Core_Clear 累计 (之前漏计时!) */
/* 【O_Present 分段】—— 专门回答“DXVK 这一跳贵不贵”
   之前所有 stats 文件里都没有耗时分解段 —— 是我自己的自举死锁:
   g_snapQpc 只在守卫块【内部】赋值, 而守卫条件又要求它非零。
   修了之后这三项才第一次能看见。 */
static long long g_ticksLock = 0;   /* LockRect + 拷图 (SR_buf -> 离屏面) */
static long long g_ticksBlit = 0;   /* UpdateSurface / StretchRect -> backbuffer */
static long long g_ticksSwap = 0;   /* Present (DXVK 转 Vulkan) */
static long long g_snapDraw = 0, g_snapPresent = 0, g_snapConvert = 0;
static long long g_snapClear = 0;
static long long g_snapLock = 0, g_snapBlit = 0, g_snapSwap = 0;
static long long g_snapFrames = 0;
static long long g_snapQpc = 0;

/* ---- 帧间隔直方图: 判定 vsync 是否在量化帧率 ----
   若被 vsync 卡住, 间隔会聚在 16.7ms / 33.3ms 两个尖峰;
   若是 CPU 瓶颈, 会是连续分布。 */
static long long g_fiPrev = 0;
static long long g_fiMin = 0, g_fiMax = 0, g_fiSum = 0;
static long long g_fiN = 0;
static long long g_fiHist[10];   /* <8 8-16 16-20 20-25 25-33 33-40 40-50 50-60 60-100 >100 ms */
static const char *kFiLabel[10] = {
    "  < 8ms", " 8-16ms", "16-20ms", "20-25ms", "25-33ms",
    "33-40ms", "40-50ms", "50-60ms", "60-100ms", ">100ms"
};

/* ---- 每帧光栅耗时: 抓符卡尖峰 ----
   平均值会掩盖尖峰: 平均 0.97ms 但符卡那几帧可能飙到几十毫秒。
   tick0 在 Present 时记下, 累加中间的 DrawPrimitiveUP; 下次 Present 结算。 */
static long long g_drawAtPresent = 0;   /* 上次 Present 时的 g_ticksDraw 快照 */
static long long g_frameDrawMax  = 0;   /* 单帧光栅耗时最大值 */
static long long g_frameDrawSum  = 0;
static int       g_frameOver167 = 0;    /* 光栅 >16.7ms 的帧数 */
static int       g_frameOver33  = 0;    /* 光栅 >33.3ms 的帧数 */
static int       g_frameDrawN   = 0;

static long long QpcNow(void)
{
    LARGE_INTEGER t;
    if (g_qpcFreq.QuadPart == 0) QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

/* 作用域计时器: 自动处理函数里的多个 return 路径 */
struct QpcScope {
    long long   t0;
    long long  *acc;
    QpcScope(long long *a) : acc(a), t0(QpcNow()) {}
    ~QpcScope() { *acc += QpcNow() - t0; }
};
static unsigned long long g_pixDrawn = 0;
#define MAX_CALL_TRACE 4000      /* 只限制【写字数】, 计数不受影响 */
#define MAX_STUB 512

#define MAX_STUB 512
static char  g_stubName[MAX_STUB][64];
static int   g_stubCount[MAX_STUB];
static int   g_stubTotal = 0;

/* 计数永远累加, 不受日志上限影响 —— 之前的 bug: 撞上限就以为没调用 */
static void CountCall(const char *name)
{
    int i;
    for (i = 0; i < g_stubTotal; i++) {
        if (strcmp(g_stubName[i], name) == 0) { g_stubCount[i]++; return; }
    }
    if (g_stubTotal < MAX_STUB) {
        strncpy_s(g_stubName[g_stubTotal], 64, name, _TRUNCATE);
        g_stubCount[g_stubTotal] = 1;
        g_stubTotal++;
    }
}

/* 真实调用总数 (不过 MAX_CALL_TRACE 上限)。
   旧代码崩溃报告写的 `已拦截调用数 = 4000` 就是撞了上限的假值，
   真值要从 g_stubCount[] 求和。 */
static int CountCallTotal(void)
{
    int i, n = 0;
    for (i = 0; i < g_stubTotal; i++) n += g_stubCount[i];
    return n;
}

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

static void Trace(const char *name)
{
    CountCall(name);
    if (!g_callLog || g_callCount >= MAX_CALL_TRACE) return;
    g_callCount++;
    fprintf(g_callLog, "%d %s\n", g_callCount, name);
    fflush(g_callLog);
}

static void LogStub(const char *name)
{
    int i;
    Trace(name);
    for (i = 0; i < g_stubTotal; i++) {
        if (strcmp(g_stubName[i], name) == 0) {
            if (g_stubCount[i] == 1) LogF("[stub] %s  (首次调用)", name);
            return;
        }
    }
}

static void DumpStubSummary(void)
{
    int i;
    LogF("=== 调用统计 (%d 种方法) ===", g_stubTotal);
    for (i = 0; i < g_stubTotal; i++)
        LogF("  %-40s x%d", g_stubName[i], g_stubCount[i]);
}

/* Standalone statistics dump, unaffected by the MAX_CALL_TRACE cap.
   Counting from shim_calls.log is unreliable: the file is capped at 4000
   lines, which previously made CreateTexture appear never to be called.
   The counters below are authoritative. */
static void BlendDump(FILE *f);   /* 前置声明: DumpStatsToFile 要用 */

static void DumpStatsToFile(const char *tag)
{
    FILE *f = fopen("D:\\th123\\shim_stats.log", "w");
    int i;
    if (!f) return;
    /* 真实帧率 + 各阶段耗时分解 */
    {
        LARGE_INTEGER freq, now;
        static LARGE_INTEGER t0;
        static int n0 = 0;
        static int init = 0;
        if (!init) { t0.QuadPart = 0; init = 1; }
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        if (t0.QuadPart == 0) {
            t0 = now; n0 = g_frameCount;
            /* 【自举死锁修复】必须在这里就把快照设好。
               之前 g_snapQpc 只在守卫块【内部】赋值, 而守卫条件
               (wsec > 0.0001) 又要求它非零 -> wsec 恒为 0 -> 整段耗时分解永不输出。
               实测: 从 06:38 到 08:11 所有 stats 文件里都没有“耗时分解”段,
               而它正是判断“DXVK 那一跳贵不贵”的唯一依据。 */
            g_snapQpc = now.QuadPart;
            g_snapFrames = g_frameCount;
            g_snapDraw = g_ticksDraw; g_snapPresent = g_ticksPresent;
            g_snapConvert = g_ticksConvert; g_snapClear = g_ticksClear;
        }
        else if (now.QuadPart > t0.QuadPart) {
            double secs = (double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
            int df = g_frameCount - n0;
            /* 【换算 bug 修正 —— 必须看这段】
               dDraw/dPresent 等是【上次 dump 以来】的增量,
               而 df 是【首次 dump 以来】的总帧数。
               两者混用 → 每帧耗时被低估几十倍:
               实测把 14.5 ms/帧 报成 0.28 ms/帧, 于是得出"光栅只占 1%"的错误结论。
               这里单独算【窗口】帧数与窗口墙钟, 作为每帧耗时的分母。 */
            int nwin = g_frameCount - (int)g_snapFrames;
            double wsec = g_snapQpc
                        ? (double)(now.QuadPart - g_snapQpc) / (double)freq.QuadPart
                        : 0.0;
            if (secs > 0.5 && nwin > 0 && wsec > 0.0001) {
                long long dDraw    = g_ticksDraw    - g_snapDraw;
                long long dPresent = g_ticksPresent - g_snapPresent;
                long long dConvert = g_ticksConvert - g_snapConvert;
                long long dClear   = g_ticksClear   - g_snapClear;
                long long dLock    = g_ticksLock    - g_snapLock;
                long long dBlit    = g_ticksBlit    - g_snapBlit;
                long long dSwap    = g_ticksSwap    - g_snapSwap;
                double tot = wsec * 1000.0;
                int wf = nwin;
                double dMs = (double)dDraw    / (double)freq.QuadPart * 1000.0;
                double pMs = (double)dPresent / (double)freq.QuadPart * 1000.0;
                double cMs = (double)dConvert / (double)freq.QuadPart * 1000.0;
                double kMs = (double)dClear   / (double)freq.QuadPart * 1000.0;

                fprintf(f, "帧率(全程)            %.2f fps  (%d 帧 / %.2f 秒)\n",
                        (double)df / secs, df, secs);
                fprintf(f, "-- 耗时分解 (窗口 %d 帧, 墙钟 %.0f ms) --\n", wf, tot);
                fprintf(f, "光栅 DrawPrimitiveUP  %8.1f ms  (%.3f ms/帧)\n", dMs, dMs / wf);
                fprintf(f, "清屏 Clear            %8.1f ms  (%.3f ms/帧)\n", kMs, kMs / wf);
                fprintf(f, "呈现 Present          %8.1f ms  (%.3f ms/帧)\n", pMs, pMs / wf);
                {
                    double lMs = (double)dLock / (double)freq.QuadPart * 1000.0;
                    double bMs = (double)dBlit / (double)freq.QuadPart * 1000.0;
                    double sMs = (double)dSwap / (double)freq.QuadPart * 1000.0;
                    fprintf(f, "  ├ Lock+拷图          %8.1f ms  (%.3f ms/帧)\n", lMs, lMs / wf);
                    fprintf(f, "  ├ Blit/UpdateSurface %8.1f ms  (%.3f ms/帧)\n", bMs, bMs / wf);
                    fprintf(f, "  └ Swap(Present)      %8.1f ms  (%.3f ms/帧)  <- DXVK 这一跳\n", sMs, sMs / wf);
                }
                fprintf(f, "纹理转换 Convert      %8.1f ms  (%.3f ms/帧)\n", cMs, cMs / wf);
                fprintf(f, "我代码合计            %.1f%% 窗口墙钟 (光栅占 %.1f%%)\n",
                        100.0 * (dMs + pMs + cMs + kMs) / (tot > 0 ? tot : 1),
                        100.0 * dMs / (tot > 0 ? tot : 1));
                fprintf(f, "  >> 其余(游戏自身+真D3D9) %.1f%%\n",
                        100.0 * (tot - (dMs + pMs + cMs + kMs)) / (tot > 0 ? tot : 1));
                /* 帧间隔直方图 */
                if (g_fiN > 0) {
                    fprintf(f, "-- 帧间隔 (%lld 帧) --\n", g_fiN);
                    fprintf(f, "最小 %.2f ms   平均 %.2f ms   最大 %.2f ms\n",
                            (double)g_fiMin / (double)freq.QuadPart * 1000.0,
                            (double)g_fiSum / (double)g_fiN / (double)freq.QuadPart * 1000.0,
                            (double)g_fiMax / (double)freq.QuadPart * 1000.0);
                    {
                        int bi;
                        for (bi = 0; bi < 10; bi++) {
                            if (g_fiHist[bi])
                                fprintf(f, "  %-10s %7lld  (%.1f%%)\n", kFiLabel[bi],
                                        g_fiHist[bi], 100.0 * (double)g_fiHist[bi] / (double)g_fiN);
                        }
                        fprintf(f, "  [注] 若集中在 16.7/33.3ms 两档 -> vsync 量化; 连续分布 -> CPU 瓶颈\n");
                    }
                }
                g_snapDraw = g_ticksDraw; g_snapPresent = g_ticksPresent;
                g_snapConvert = g_ticksConvert; g_snapClear = g_ticksClear;
                g_snapLock = g_ticksLock; g_snapBlit = g_ticksBlit; g_snapSwap = g_ticksSwap;
                g_snapFrames = g_frameCount; g_snapQpc = now.QuadPart;
                g_fiMin = 0; g_fiMax = 0; g_fiSum = 0; g_fiN = 0;
                { int bi; for (bi = 0; bi < 10; bi++) g_fiHist[bi] = 0; }
            }
        }
    }
    fprintf(f, "=== %s : %d 种方法 ===\n", tag, g_stubTotal);
    for (i = 0; i < g_stubTotal; i++)
        fprintf(f, "%-40s %d\n", g_stubName[i], g_stubCount[i]);
    fprintf(f, "--- 纹理 ---\n");
    fprintf(f, "SetTexture(NULL)      %d\n", g_nullTex);
    fprintf(f, "SetTexture(real)      %d\n", g_realTex);
    fprintf(f, "最后 Clear 颜色       0x%08X\n", g_lastClear);
    fprintf(f, "--- 光栅 ---\n");
    fprintf(f, "进入三角形            %lld\n", g_triIn);
    fprintf(f, "退化三角形            %lld\n", g_triDegenerate);
    fprintf(f, "完全屏外              %lld\n", g_triOffscreen);
    fprintf(f, "填充扫描段            %lld\n", g_spans);
    fprintf(f, "填充像素总数          %lld\n", g_spanPix);
    fprintf(f, "纹理未就绪命中        %lld\n", g_whiteSample);
    fprintf(f, "正常采样              %lld\n", g_validSample);
    fprintf(f, "-- 纹理/混合诊断 --\n");
    fprintf(f, "有纹理但混合关闭      %lld\n", g_texNoBlend);
    fprintf(f, "纹理 alpha 全不透明    %lld\n", g_texNoAlpha);
    fprintf(f, "  [注] 第一项大 -> 精灵透明区变成黑边/白底 (混合没开)\n");
    fprintf(f, "  [注] 第二项大 -> 素材本身没抠干净 (游戏侧问题)\n");
    BlendDump(f);
    fprintf(f, "--- 每帧光栅耗时 (抓符卡尖峰) ---\n");
    if (g_frameDrawN > 0 && g_qpcFreq.QuadPart > 0) {
        double q = (double)g_qpcFreq.QuadPart;
        fprintf(f, "采样帧数              %d\n", g_frameDrawN);
        fprintf(f, "最大单帧光栅          %.3f ms\n", (double)g_frameDrawMax / q * 1000.0);
        fprintf(f, "平均单帧光栅          %.3f ms\n",
                (double)g_frameDrawSum / q * 1000.0 / (double)g_frameDrawN);
        fprintf(f, "光栅 >16.7ms 的帧     %d  (%.2f%%)\n", g_frameOver167,
                100.0 * (double)g_frameOver167 / (double)g_frameDrawN);
        fprintf(f, "光栅 >33.3ms 的帧     %d  (%.2f%%)\n", g_frameOver33,
                100.0 * (double)g_frameOver33 / (double)g_frameDrawN);
        fprintf(f, "  [注] 若最大值远高于平均 -> 符卡/特效尖峰就是卡顿来源\n");
    } else {
        fprintf(f, "(尚无采样)\n");
    }
    fclose(f);
}

/* ================= 全局状态 ================= */
static HWND  g_hwnd = NULL;
static IDirect3DDevice9 *g_devPtr = NULL;

/* IDirect3D9 实例指针。
   设备侧的 GetDirect3D 需要返回它 —— D3DX 建纹理前会拿它做格式检查，
   之前这里返回失败，直接导致 D3DX 放弃创建纹理 (实测 CreateTexture 0 次)。
   g_d3d9 本体在文件末尾才定义，所以这里用一个早绑定的指针。 */
static IDirect3D9 *g_d3d9Ptr = NULL;

/* ================= GPU identity profiles =================
   Advertise a plausible DX9-era GPU so that device creation always succeeds.
   The game imports d3dx9_33; D3DX queries VRAM and capability, and a zero
   reply makes it skip texture creation entirely.
   Values are real card specifications so the reported capability set stays
   self-consistent. */
typedef struct {
    const char *name;
    DWORD vendorId, deviceId;
    DWORD vramMB;          /* 显存 MB */
    DWORD maxTex;          /* 最大纹理边长 */
    DWORD vsVer, psVer;    /* shader 版本 (0xFFFE0101 = vs_1_1, 0xFFFF0101 = ps_1_1) */
    int   dx9only;         /* 1 = 纯 DX9 固定管线卡 */
} GpuProfile;

/* 数据来源: 各卡公开规格。
   NV34 = GeForce FX 5200, 64MB, DX9,  VS/PS 2.0+
   Intel 915GM/GMA900, 共享显存 64MB, DX9, 无硬件 T&L (纯vertex shader) */
#define VS_2_0   0xFFFE0200u
#define PS_2_0   0xFFFF0200u
#define VS_1_1   0xFFFE0101u
#define PS_1_1   0xFFFF0101u
static GpuProfile g_gpu = { "NVIDIA GeForce FX 5200", 0x10DE, 0x0322, 64, 4096, VS_2_0, PS_2_0, 1 };

/* 切档: 由 soku_shim.ini [soku_shim] Gpu= 控制 */
static void GpuSelect(const char *key)
{
    if (!key || !key[0]) return;
    if (_stricmp(key, "gf5200") == 0 || _stricmp(key, "nv34") == 0) {
        g_gpu.name = "NVIDIA GeForce FX 5200"; g_gpu.vendorId = 0x10DE; g_gpu.deviceId = 0x0322;
        g_gpu.vramMB = 64; g_gpu.maxTex = 4096; g_gpu.vsVer = VS_2_0; g_gpu.psVer = PS_2_0;
    } else if (_stricmp(key, "gma900") == 0 || _stricmp(key, "i915") == 0) {
        g_gpu.name = "Intel(R) 82915G/GV/910GL Express Chipset Family";
        g_gpu.vendorId = 0x8086; g_gpu.deviceId = 0x2582;
        g_gpu.vramMB = 64; g_gpu.maxTex = 2048; g_gpu.vsVer = VS_1_1; g_gpu.psVer = PS_1_1;
    } else if (_stricmp(key, "weak") == 0) {
        /* 极端降级档: 报最小能力, 逼游戏走最简渲染路径 */
        g_gpu.name = "Generic Software Rasterizer";
        g_gpu.vendorId = 0x1234; g_gpu.deviceId = 0x1111;
        g_gpu.vramMB = 8; g_gpu.maxTex = 512; g_gpu.vsVer = VS_1_1; g_gpu.psVer = PS_1_1;
    }
    LogF("[gpu] 显卡身份 -> %s (VRAM %uMB, tex %u, vs=0x%X ps=0x%X)",
         g_gpu.name, g_gpu.vramMB, g_gpu.maxTex, g_gpu.vsVer, g_gpu.psVer);
}

#define VRAM_TOTAL ((UINT)g_gpu.vramMB * 1024 * 1024)

/* 显存回报 —— 实现见下面。之前生成器返回 0, 会让 D3DX 判定
   "一张纹理都放不下" 从而根本不建纹理 (实测 CreateTexture 0 次)。 */
static UINT HD_GetAvailableTextureMem_Val(void);

/* ================= CPU 侧纹理 ================= */
/* CPU-side texture.  All shading and sampling happen on the CPU; the
   GPU receives only the finished frame.
   所以纹理也留在 CPU: 游戏 LockRect 写像素 -> UnlockRect 时转 ARGB8888
   Game writes pixels via LockRect; UnlockRect converts them to ARGB8888
   in SR_Tex, which the rasterizer samples directly. */
class ShimTexture9;
HRESULT HT_GetDevice(IDirect3DDevice9 **pp);
HRESULT HT_GetLevelDesc(ShimTexture9 *t, UINT Level, D3DSURFACE_DESC *p);
HRESULT HT_LockRect(ShimTexture9 *t, UINT Level, D3DLOCKED_RECT *p, CONST RECT *r, DWORD f);
HRESULT HT_UnlockRect(ShimTexture9 *t, UINT Level);
HRESULT HT_GetSurfaceLevel(ShimTexture9 *t, UINT Level, IDirect3DSurface9 **pp);

/* Texture-level surface.
   真 D3DX 和游戏常走 texture->GetSurfaceLevel(0) 再 LockRect 填像素。
   之前这里返回 NULL+失败, 游戏接着就用那个空指针 -> 0xC0000005。 */
class ShimTexSurf9;
static void SurFreeSlot(ShimTexSurf9 *s);   /* 前置声明: 类里 Release() 要用 */
HRESULT HT_S_GetDevice(ShimTexSurf9 *s, IDirect3DDevice9 **pp);
HRESULT HT_S_GetContainer(ShimTexSurf9 *s, REFIID riid, void **pp);
HRESULT HT_S_GetDesc(ShimTexSurf9 *s, D3DSURFACE_DESC *p);
HRESULT HT_S_LockRect(ShimTexSurf9 *s, D3DLOCKED_RECT *p, CONST RECT *r, DWORD f);
HRESULT HT_S_UnlockRect(ShimTexSurf9 *s);

class ShimTexSurf9 : public IDirect3DSurface9
{
public:
    ULONG          ref;
    int            inUse;     /* 槽位占用标志 —— Release 到 0 时归还 */
    ShimTexture9  *owner;
    UINT           level;

    ULONG STDMETHODCALLTYPE AddRef()  override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        if (ref > 0) ref--;
        if (ref == 0) SurFreeSlot(this);
        return ref;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **p) override
    { if (!p) return E_POINTER; *p = this; return S_OK; }
#include "stubs_texturesurf9.h"
};

#define MAX_TEXSURF 8192
static ShimTexSurf9 g_texSurf[MAX_TEXSURF];
static int          g_texSurfUsed = 0;

/* 归还 surface 槽位。
   【关键】原来是自增游标、Release 从不归还 —— 实测 4097 次就超 4096 上限,
   返回 NULL -> 游戏拿到空指针: 先表现为【对手隐身/血条没人】(填色失败),
   再在某处解引用 NULL -> 0xC0000005 @ 0x405256。
   跟纹理池那个坑同型, 当时只修了纹理池、漏了这个。 */
static void SurFreeSlot(ShimTexSurf9 *s)
{
    if (!s) return;
    s->owner = NULL;
    s->inUse = 0;
}static void TexFreeSlot(ShimTexture9 *t);   /* 前置声明: 类里 Release() 要用 */

static void SurFreeSlot(ShimTexSurf9 *s);   /* 前置声明 */

class ShimTexture9 : public IDirect3DTexture9
{
public:
    ULONG          ref;
    int            inUse;     /* 池子槽位占用标志 —— Release 到 0 时归还 */    UINT           w, h;
    D3DFORMAT      fmt;
    unsigned char *bits;      /* 锁定后游戏写入的缓冲 */
    int            pitch;
    int            locked;
    SR_Tex         sr;        /* 解锁后转好的 ARGB8888 */

    ULONG STDMETHODCALLTYPE AddRef()  override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        if (ref > 0) ref--;
        if (ref == 0) TexFreeSlot(this);
        return ref;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **p) override
    { if (!p) return E_POINTER; *p = this; return S_OK; }
#include "stubs_texture9.h"
};

/* 槽位归还: 释放堆内存并标记空闲。
   对象本体在静态池里, 不 free —— 这样即使游戏持有野指针也不会崩,
   只是槽位可能被复用 (D3D 语义上 Release 后对象就失效了)。 */
static void TexFreeSlot(ShimTexture9 *t)
{
    if (!t) return;
    /* Deferred release.  Do NOT free the pixel buffers here.
       A texture can still be bound when its refcount reaches zero:
         SetTexture(A) -> Draw -> Clear -> Draw ...
       The game does not re-issue SetTexture after Clear, so its binding
       still points at A.  Freeing A's pixels here would cause subsequent
       draws to either fall back to vertex colour (white blocks) or sample
       freed memory (garbage / transparent output).
       Therefore only mark the slot free; the actual free happens when the
       slot is reused (see Core_CreateTexture). */
    t->inUse = 0;
}

/* Texture slots.  The pool is larger than needed and slots are recycled via
   the inUse flag; the earlier implementation used a monotonically growing
   cursor and never returned slots, so 1373 texture creations exhausted a
   pool of 1024. */

/* ---------- Frame-level texture retention ----------
   Frame-level batching defers rasterization to the end of the frame.  A
   texture can be released (refcount 0) in the meantime: TexFreeSlot would
   drop its pixel buffer and clear the valid flag, so later draws fall back
   to vertex colour (white) or sample released memory.

   Solution: AddRef every texture bound during the frame and release the
   whole set after the frame's Flush.  This is cheaper than per-command
   reference counting and avoids asymmetric release.

   Note: these are forward declarations.  TexHold is called from
   Core_SetTexture while the texture class is defined further below. */
#define MAX_HELD 4096
static ShimTexture9 *g_held[MAX_HELD];
static int           g_heldN = 0;
static void TexHold(ShimTexture9 *t);
static void TexReleaseHeld(void);

#define MAX_TEX 8192
static ShimTexture9 g_texPool[MAX_TEX];
static int          g_texUsed = 0;

static void TexHold(ShimTexture9 *t)
{
    int i;
    if (!t || !t->inUse) return;
    for (i = 0; i < g_heldN; i++)
        if (g_held[i] == t) return;          /* 一帧只持一次 */
    if (g_heldN >= MAX_HELD) return;
    t->AddRef();
    g_held[g_heldN++] = t;
}

static void TexReleaseHeld(void)
{
    int i;
    for (i = 0; i < g_heldN; i++)
        if (g_held[i]) g_held[i]->Release();
    g_heldN = 0;
}

/* ---- FVF 普查 ---- */
static DWORD g_fvf = 0;
static DWORD g_fvfVal[24];
static int   g_fvfCnt[24];
static int   g_fvfN = 0;

static void FvfCensus(DWORD fvf)
{
    int i;
    g_fvf = fvf;
    for (i = 0; i < g_fvfN; i++)
        if (g_fvfVal[i] == fvf) { g_fvfCnt[i]++; return; }
    if (g_fvfN < 24) {
        LogF("[FVF] 新格式 0x%08X  pos2D=%d diffuse=%d tex1=%d texcount=%d",
             fvf, (fvf & D3DFVF_XYZRHW) ? 1 : 0, (fvf & D3DFVF_DIFFUSE) ? 1 : 0,
             (fvf & D3DFVF_TEX1) ? 1 : 0, (int)((fvf >> 8) & 0xF));
        g_fvfVal[g_fvfN] = fvf; g_fvfCnt[g_fvfN] = 1; g_fvfN++;
    }
}

/* ---- 渲染状态普查 ---- */
static int g_rsLogged = 0;

/* 【决定性探针】混合模式直方图。
   症状“素材黑边没扣干净 / 黑块”—— 典型是【加算混合被当成 alpha 混合】:
     加算(ONE/ONE): 黑色像素加 0, 自然不可见 -> 正常抠底
     alpha混合(SRCALPHA/INVSRCALPHA): 黑底不透明 -> 露出黑边/黑块
   游戏用哪个、我实际用哪个, 一看这个表就知道。 */
static int g_blendPairs[16][16];
static int g_blendLogged = 0;

static void RsCensus(D3DRENDERSTATETYPE State, DWORD Value)
{
    if (g_rsLogged < 40) {
        LogF("[RS] state=%d val=0x%X", (int)State, Value);
        g_rsLogged++;
    }
}

/* 在每次 SetTexture+Draw 前记一次当前混合对 */
static void BlendCensus(int srcB, int dstB)
{
    if (srcB < 0 || srcB > 15) srcB = 15;
    if (dstB < 0 || dstB > 15) dstB = 15;
    g_blendPairs[srcB][dstB]++;
    g_blendLogged++;
}

static void BlendDump(FILE *f)
{
    int i, j;
    fprintf(f, "-- 混合模式直方图 (SrcBlend, DstBlend) --\n");
    fprintf(f, "  枚举: 1=ZERO 2=ONE 3=SRCCOLOR 4=INVSRCCOLOR 5=SRCALPHA 6=INVSRCALPHA\n");
    for (i = 0; i < 16; i++)
        for (j = 0; j < 16; j++)
            if (g_blendPairs[i][j])
                fprintf(f, "  src=%2d dst=%2d  x%d\n", i, j, g_blendPairs[i][j]);
    fprintf(f, "  [读法] src=2,dst=2 (ONE/ONE) = 加算 -> 黑底自然透明\n");
    fprintf(f, "  [读法] src=5,dst=6 (SRCALPHA/INVSRCALPHA) = 普通 alpha\n");
}

/* 按 FVF 解码顶点。
   内存布局(D3D9): [位置4float][diffuse? u32][specular? u32][tex1? 2float]...
   不能只靠 stride 猜, 否则把 texcoord 当颜色读。 */
static void SR_Decode(const unsigned char *p, UINT stride, SR_Vtx *v)
{
    DWORD fvf = g_fvf;
    int off = 16;
    /* SSAA: 顶点已在屏幕空间, 直乘倍率 -> 光栅在超采样分辨率上跑 */
    v->x = ((const float *)p)[0] * (float)SR_ss;
    v->y = ((const float *)p)[1] * (float)SR_ss;
    v->col = 0xFFFFFFFFu;
    v->u = 0.0f; v->v = 0.0f; v->hasUV = 0;

    if (fvf == 0) {                 /* SetFVF 没调过 -> 用 stride 启发式 */
        if (stride >= 20) v->col = *(const DWORD *)(p + 16);
        if (stride >= 28) {
            v->u = ((const float *)p)[5];
            v->v = ((const float *)p)[6];
            v->hasUV = 1;
        }
        return;
    }
    if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW) return;
    if (fvf & D3DFVF_DIFFUSE)  { if ((UINT)off + 4 <= stride) v->col = *(const DWORD *)(p + off); off += 4; }
    if (fvf & D3DFVF_SPECULAR) { off += 4; }
    /* 纹理坐标【组数】由 FVF bits 8..11 编码: TEX1=1组(0x100) TEX2=2组(0x200) ...
       原来是 `if (fvf & D3DFVF_TEX1)` —— 只测 bit 8, 于是 FVF=0x244 (TEX2)
       整组坐标被跳过, 顶点 u,v 全留在 0 -> 整片采样到纹素 (0,0).
       改为按组数判, 只取第 0 组 (光栅器只用 stage 0). */
    {
        UINT texcount = (fvf >> 8) & 0xF;
        if (texcount >= 1 && (UINT)off + 8 <= stride) {
            v->u = *(const float *)(p + off);
            v->v = *(const float *)(p + off + 4);
            v->hasUV = 1;
        }
    }
}

HRESULT HT_GetDevice(IDirect3DDevice9 **pp) { if (pp) *pp = g_devPtr; return S_OK; }

HRESULT HT_GetLevelDesc(ShimTexture9 *t, UINT Level, D3DSURFACE_DESC *p)
{
    Trace("HT_GetLevelDesc");
    if (!p) return D3DERR_INVALIDCALL;
    memset(p, 0, sizeof(*p));
    p->Format = t ? t->fmt : D3DFMT_A8R8G8B8;
    p->Type   = D3DRTYPE_SURFACE;
    p->Pool   = D3DPOOL_MANAGED;
    p->Width  = t ? t->w : 0;
    p->Height = t ? t->h : 0;
    return S_OK;
}

static int TexBpp(D3DFORMAT f)
{
    switch (f) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: return 4;
    case D3DFMT_R5G6B5: case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4: case D3DFMT_A8L8: return 2;
    /* 【修正】8bit 格式之前落到 default 返回 4, 与 HT_Convert 里按 1 字节
       读取的分支矛盾 —— 会造成 pitch 与数据布局不一致。 */
    case D3DFMT_A8: case D3DFMT_L8:
    case D3DFMT_P8: case D3DFMT_A8P8: return 1;
    default: return 4;
    }
}

/* ---------------- 纹理内容诊断 ----------------
   症状是“游戏里黑块/空气”, 但导出的帧看着正常 —— 必须直接看纹素。
   记录: 转换次数、像素和、非零数、alpha 范围、格式分布。
   若 sum=0 -> 上传失败; 若 alpha 全 255 -> 素材未抠透(游戏侧)。 */
static int       g_fmtSeen[64];
static long long g_convLogged = 0;
static int       g_texDumped  = 0;

static int DumpTexBmp(ShimTexture9 *t, const char *path)
{
    FILE *f; int x, y, rowbytes, pad, tmp;
    unsigned char *row;
    if (!t || !t->sr.px || t->sr.w <= 0 || t->sr.h <= 0) return 0;
    f = fopen(path, "wb");
    if (!f) return 0;
    rowbytes = t->sr.w * 3;
    pad = (4 - (rowbytes & 3)) & 3;
    { unsigned short bf = 0x4D42; fwrite(&bf, 2, 1, f); }
    tmp = 54 + (rowbytes + pad) * t->sr.h; fwrite(&tmp, 4, 1, f);
    tmp = 0; fwrite(&tmp, 2, 1, f); fwrite(&tmp, 2, 1, f);
    tmp = 54; fwrite(&tmp, 4, 1, f);
    tmp = 40; fwrite(&tmp, 4, 1, f);
    tmp = t->sr.w; fwrite(&tmp, 4, 1, f);
    tmp = t->sr.h; fwrite(&tmp, 4, 1, f);
    { unsigned short p = 1, b = 24; fwrite(&p, 2, 1, f); fwrite(&b, 2, 1, f); }
    tmp = 0; fwrite(&tmp, 4, 1, f);
    tmp = 0; fwrite(&tmp, 4, 1, f);
    tmp = 0; fwrite(&tmp, 4, 1, f);
    tmp = 0; fwrite(&tmp, 4, 1, f);
    row = (unsigned char *)malloc(rowbytes + pad);
    if (!row) { fclose(f); return 0; }
    for (y = t->sr.h - 1; y >= 0; y--) {
        const unsigned int *s = t->sr.px + (size_t)y * t->sr.w;
        for (x = 0; x < t->sr.w; x++) {
            unsigned int c = s[x];
            row[x*3+0] = (unsigned char)(c & 0xFF);
            row[x*3+1] = (unsigned char)((c >> 8) & 0xFF);
            row[x*3+2] = (unsigned char)((c >> 16) & 0xFF);
        }
        for (x = 0; x < pad; x++) row[rowbytes + x] = 0;
        fwrite(row, 1, rowbytes + pad, f);
    }
    free(row);
    fclose(f);
    return 1;
}

HRESULT HT_LockRect(ShimTexture9 *t, UINT Level, D3DLOCKED_RECT *p, CONST RECT *r, DWORD f)
{
    Trace("HT_LockRect");
    if (!t || !p) return D3DERR_INVALIDCALL;
    if (!t->bits) {
        t->pitch = (int)t->w * TexBpp(t->fmt);
        t->bits  = (unsigned char *)malloc((size_t)t->pitch * t->h);
        if (!t->bits) return D3DERR_INVALIDCALL;
        memset(t->bits, 0, (size_t)t->pitch * t->h);
    }
    t->locked = 1;
    p->Pitch  = t->pitch;
    p->pBits  = t->bits;
    return S_OK;
}

/* 把 t->bits 的原始像素转成 ARGB8888 存进 t->sr。
   抽成独立函数: UpdateTexture/UpdateSurface 也走这条路
   (字体/菜单面板是动态纹理, 靠 UpdateTexture 更新像素)。 */
static HRESULT HT_Convert(ShimTexture9 *t)
{
    QpcScope _perf(&g_ticksConvert);
    UINT x, y;
    if (!t || !t->bits) return D3DERR_INVALIDCALL;

    if (!t->sr.px || t->sr.w != (int)t->w || t->sr.h != (int)t->h) {
        if (t->sr.px) free(t->sr.px);
        t->sr.px = (unsigned int *)malloc((size_t)t->w * t->h * 4);
        if (!t->sr.px) return E_OUTOFMEMORY;
        t->sr.w = (int)t->w;
        t->sr.h = (int)t->h;
    }

    for (y = 0; y < t->h; y++) {
        const unsigned char *src = t->bits + (size_t)y * t->pitch;
        unsigned int *dst = t->sr.px + (size_t)y * t->w;
        for (x = 0; x < t->w; x++) {
            unsigned int c;
            switch (t->fmt) {
            case D3DFMT_R5G6B5: {
                unsigned short v = *(const unsigned short *)(src + x * 2);
                c = 0xFF000000u | ((((v >> 11) & 0x1F) << 3) << 16)
                                | ((((v >> 5)  & 0x3F) << 2) << 8)
                                |  ((v & 0x1F) << 3);
                break; }
            case D3DFMT_A1R5G5B5: {
                unsigned short v = *(const unsigned short *)(src + x * 2);
                c = (((v & 0x8000) ? 255u : 0u) << 24)
                  | ((((v >> 10) & 0x1F) << 3) << 16)
                  | ((((v >> 5)  & 0x1F) << 3) << 8)
                  |  ((v & 0x1F) << 3);
                break; }
            case D3DFMT_A4R4G4B4: {
                unsigned short v = *(const unsigned short *)(src + x * 2);
                c = ((((v >> 12) & 0xF) * 17) << 24)
                  | ((((v >> 8)  & 0xF) * 17) << 16)
                  | ((((v >> 4)  & 0xF) * 17) << 8)
                  |  (((v & 0xF) * 17));
                break; }
            case D3DFMT_A8L8: {
                unsigned int a = src[x * 2 + 1], l = src[x * 2];
                c = (a << 24) | (l << 16) | (l << 8) | l;
                break; }
            case D3DFMT_A8:      /* 纯 alpha */
            case D3DFMT_L8: {    /* 亮度 */
                unsigned int v = src[x];
                c = 0xFF000000u | (v << 16) | (v << 8) | v;
                break; }
            default:   /* A8R8G8B8 / X8R8G8B8: 内存布局 = 0xAARRGGBB */
                c = *(const unsigned int *)(src + x * 4);
                if (t->fmt == D3DFMT_X8R8G8B8) c |= 0xFF000000u;
                break;
            }
            dst[x] = c;
        }
    }
    t->sr.valid = 1;
    return S_OK;
}

HRESULT HT_UnlockRect(ShimTexture9 *t, UINT Level)
{
    Trace("HT_UnlockRect");
    if (!t) return D3DERR_INVALIDCALL;
    t->locked = 0;
    return HT_Convert(t);
}

HRESULT HT_GetSurfaceLevel(ShimTexture9 *t, UINT Level, IDirect3DSurface9 **pp)
{
    ShimTexSurf9 *s = NULL;
    int i;
    Trace("HT_GetSurfaceLevel");
    if (pp) *pp = NULL;
    if (!t || !pp) return D3DERR_INVALIDCALL;

    /* 先找空闲槽位 (Release 会归还), 找不到再用新槽 */
    for (i = 0; i < g_texSurfUsed; i++) {
        if (!g_texSurf[i].inUse) { s = &g_texSurf[i]; break; }
    }
    if (!s) {
        if (g_texSurfUsed >= MAX_TEXSURF) {
            LogF("[!] surface 池满 (%d) -> 返回失败", MAX_TEXSURF);
            return D3DERR_INVALIDCALL;
        }
        s = &g_texSurf[g_texSurfUsed++];
    }

    /* 同样不能 memset 整个对象 —— 会清掉 vptr */
    s->inUse = 1;
    s->ref   = 1;
    s->owner = t;
    s->level = Level;
    {
        static int n = 0;
        if (n < 4) { LogF("[tex] GetSurfaceLevel(%u) -> %p (槽位复用已启用)", Level, (void *)s); n++; }
    }
    *pp = s;
    return S_OK;
}

HRESULT HT_S_GetDevice(ShimTexSurf9 *, IDirect3DDevice9 **pp)
{ if (pp) *pp = g_devPtr; return S_OK; }

HRESULT HT_S_GetContainer(ShimTexSurf9 *s, REFIID, void **pp)
{ if (pp) *pp = s ? (void *)s->owner : NULL; return s ? S_OK : D3DERR_INVALIDCALL; }

HRESULT HT_S_GetDesc(ShimTexSurf9 *s, D3DSURFACE_DESC *p)
{ return HT_GetLevelDesc(s ? s->owner : NULL, s ? s->level : 0, p); }

HRESULT HT_S_LockRect(ShimTexSurf9 *s, D3DLOCKED_RECT *p, CONST RECT *r, DWORD f)
{ return HT_LockRect(s ? s->owner : NULL, s ? s->level : 0, p, r, f); }

HRESULT HT_S_UnlockRect(ShimTexSurf9 *s)
{ return HT_UnlockRect(s ? s->owner : NULL, s ? s->level : 0); }

/* ================= Core_* 实现 ================= */

/* CPU rasterizer output stage.
   The finished frame goes into a system-memory surface, is blitted to the
   back buffer, then presented by a real D3D9 device. */
#include "d3d9out.h"

#include "core_proto.h"

HRESULT Core_BeginScene() { Trace("Core_BeginScene"); return S_OK; }
HRESULT Core_EndScene()   { Trace("Core_EndScene"); return S_OK; }

HRESULT Core_Clear(DWORD Count, CONST D3DRECT *pRects, DWORD Flags,
                   D3DCOLOR Color, float Z, DWORD Stencil)
{
    QpcScope _perf(&g_ticksClear);
    Trace("Core_Clear");

    /* Sequence is significant.  Draw calls are deferred to end of frame,
       but Clear must take effect immediately, so the queue is flushed first.
       Otherwise the order becomes:
           intended:  Clear1 -> Draw1 -> Clear2 -> Draw2
           actual  :  Clear1 -> Clear2 -> [end of frame] Draw1 -> Draw2
       and Draw1 is erased by Clear2.
       Flush must happen before SR_Reset, otherwise the queued commands still
       reference the previous buffer size. */
    {
        QpcScope _raster(&g_ticksDraw);
        SR_Flush();
        TexReleaseHeld();
    }

    /* CPU 光栅: 清自己的帧缓冲 (按 SSAA 倍率放大) */
    SR_Reset(O_ready ? (int)O_w * SR_ss : 640 * SR_ss,
             O_ready ? (int)O_h * SR_ss : 480 * SR_ss);

    if (Flags & D3DCLEAR_TARGET) {
        SR_Clear(Color);
        g_lastClear = Color;
        {
            static int cn = 0;
            if (cn < 8) { LogF("[clear] color=0x%08X flags=0x%X", Color, Flags); cn++; }
        }
    }
    Trace("after_SR_Clear");
    return S_OK;
}

/* 帧缓冲导出 —— 直接写软光栅自己的 SR_buf。
   屏幕抓取靠不住 (DXVK 的 flip 输出 CopyFromScreen 抓不到，
   而且窗口坐标会跟终端打架)。SR_buf 就是最终画面本身。 */
static void DumpFrameBmp(const char *path)
{
    FILE *f;
    int w = SR_w, h = SR_h;
    int rowBytes, pad, i, x, y;
    unsigned char hdr[54];

    if (!SR_buf || w <= 0 || h <= 0) return;

    /* 导出前先算校验和 —— 避免"文件写了但是全黑"这种哑谜 */
    {
        unsigned int sum = 0;
        int nonBlack = 0, n = w * h, i;
        for (i = 0; i < n; i++) {
            unsigned int c = SR_buf[i];
            sum = sum * 31u + c;
            if ((c & 0x00FFFFFFu) != 0) nonBlack++;
        }
        LogF("[dump] %s frame=%d %dx%d sum=0x%08X nonBlack=%.1f%%",
             path, g_frameCount, w, h, sum, 100.0 * nonBlack / n);
    }

    f = fopen(path, "wb");
    if (!f) return;

    rowBytes = w * 4;
    pad = (4 - (rowBytes % 4)) % 4;   /* 32bpp 本来就不用补，保险起见 */

    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    {
        unsigned int fsz = 54 + (unsigned int)((rowBytes + pad) * h);
        unsigned int off = 54;
        unsigned int ih  = 40;
        unsigned int sz  = rowBytes * h;
        memcpy(hdr + 2,  &fsz, 4);
        memcpy(hdr + 10, &off, 4);
        memcpy(hdr + 14, &ih,  4);
        memcpy(hdr + 18, &w,   4);
        memcpy(hdr + 22, &h,   4);
        hdr[26] = 1;  hdr[28] = 32;   /* planes=1, bpp=32 */
        memcpy(hdr + 34, &sz,  4);
    }
    fwrite(hdr, 1, 54, f);

    /* BMP 是 bottom-up，而 SR_buf 是 top-down */
    for (y = h - 1; y >= 0; y--) {
        const unsigned int *src = SR_buf + (size_t)y * w;
        for (x = 0; x < w; x++) {
            unsigned int c = src[x];
            unsigned char px[4];
            px[0] = (unsigned char)( c        & 0xFF);   /* B */
            px[1] = (unsigned char)((c >> 8)  & 0xFF);   /* G */
            px[2] = (unsigned char)((c >> 16) & 0xFF);   /* R */
            px[3] = (unsigned char)((c >> 24) & 0xFF);   /* A */
            fwrite(px, 1, 4, f);
        }
        for (i = 0; i < pad; i++) fputc(0, f);
    }
    fclose(f);
}

HRESULT Core_Present(CONST RECT *, CONST RECT *, HWND, CONST RGNDATA *)
{
    QpcScope _perf(&g_ticksPresent);
    Trace("Core_Present");

    /* 优先: CPU 光栅 -> 真 D3D9 离屏面 -> backbuffer -> Present (32 位色) */
    if (O_ready) {
        /* 帧间隔采样: 用于判定 vsync 是否在量化帧率 */
        {
            long long now = QpcNow();
            if (g_fiPrev) {
                long long d = now - g_fiPrev;
                double ms = (double)d / (g_qpcFreq.QuadPart ? (double)g_qpcFreq.QuadPart : 1.0) * 1000.0;
                if (g_fiMin == 0 || d < g_fiMin) g_fiMin = d;
                if (d > g_fiMax) g_fiMax = d;
                g_fiSum += d; g_fiN++;
                {
                    int b;
                    if      (ms <   8.0) b = 0;
                    else if (ms <  16.0) b = 1;
                    else if (ms <  20.0) b = 2;
                    else if (ms <  25.0) b = 3;
                    else if (ms <  33.0) b = 4;
                    else if (ms <  40.0) b = 5;
                    else if (ms <  50.0) b = 6;
                    else if (ms <  60.0) b = 7;
                    else if (ms < 100.0) b = 8;
                    else                 b = 9;
                    g_fiHist[b]++;
                }
            }
            g_fiPrev = now;
        }
        /* 本帧的绘制入队完毕 —— 在这里统一光栅化 (多线程生效点)
           计时归到 g_ticksDraw: 否则光栅耗时会错记到 Present 名下,
           仪表就又不准了 (之前吃过这个亏)。 */
        {
            QpcScope _raster(&g_ticksDraw);
            SR_Flush();
            TexReleaseHeld();     /* 光栅化完毕, 放掉本帧持住的纹理 */
        }

        /* 结算上一帧的光栅耗时 —— 抓符卡尖峰 */
        {
            long long frameDraw = g_ticksDraw - g_drawAtPresent;
            if (g_frameDrawN > 0) {   /* 跳过第一帧 */
                if (frameDraw > g_frameDrawMax) g_frameDrawMax = frameDraw;
                g_frameDrawSum += frameDraw;
                if (frameDraw > (long long)(g_qpcFreq.QuadPart * 0.0167)) g_frameOver167++;
                if (frameDraw > (long long)(g_qpcFreq.QuadPart * 0.0333)) g_frameOver33++;
            }
            g_frameDrawN++;
            g_drawAtPresent = g_ticksDraw;
        }
        O_PresentAA();
        g_frameCount++;
        if (g_frameCount % 200 == 0) DumpFrameBmp("D:\\th123\\sr_dump.bmp");
        if (g_frameCount % 120 == 0) DumpStatsToFile("运行中");
        return S_OK;
    }

    return S_OK;
}

HRESULT Core_DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
                             CONST void *pStream, UINT Stride)
{
    static int logged = 0;
    QpcScope _perf(&g_ticksDraw);
    Trace("Core_DrawPrimitiveUP");
    /* 混合模式取证: 每次绘制记一次当前混合对 */
    BlendCensus(SR_st.srcBlend, SR_st.dstBlend);
    if (!pStream || Stride < 20) return S_OK;

    if (!logged) {
        LogF("[!] DrawPrimitiveUP: type=%d count=%u stride=%u (-> CPU 光栅)",
             (int)PrimitiveType, PrimitiveCount, Stride);
        logged = 1;
    }
    if (!O_ready || !SR_buf) return S_OK;

    /* 顶点格式解码: 只认 stride>=20 的 XYZRHW+DIFFUSE(+TEX1)
       XYZRHW: 已是屏幕坐标, 无需矩阵变换 —— 2D 游戏的便宜处
       【重要】这里只【入队】, 不直接光栅化 ——
       多线程只在帧级批处理下生效 (实测: 平均每次 Draw 只有 2 个三角形,
       每次开线程会被同步开销吃光)。 */
    {
        const unsigned char *v = (const unsigned char *)pStream;
        int hasUV = (Stride >= 28);
        SR_Vtx a, b, c;
        UINT t;

#define MKSR(idx, gv) SR_Decode(v + (idx) * Stride, Stride, &(gv))
#define QUEUE3(aa, bb, cc) SR_Queue((aa), (bb), (cc), &SR_st, SR_boundTex)

        switch (PrimitiveType) {
        case D3DPT_TRIANGLELIST:
            for (t = 0; t < PrimitiveCount; t++) {
                MKSR(t*3, a); MKSR(t*3+1, b); MKSR(t*3+2, c);
                QUEUE3(&a, &b, &c);
            }
            break;
        case D3DPT_TRIANGLESTRIP:
            for (t = 0; t < PrimitiveCount; t++) {
                UINT i0 = t, i1 = t + 1, i2 = t + 2;
                if (t & 1) { UINT tmp = i1; i1 = i2; i2 = tmp; }
                MKSR(i0, a); MKSR(i1, b); MKSR(i2, c);
                QUEUE3(&a, &b, &c);
            }
            break;
        case D3DPT_TRIANGLEFAN:
            MKSR(0, a);
            for (t = 0; t < PrimitiveCount; t++) {
                MKSR(t+1, b); MKSR(t+2, c);
                QUEUE3(&a, &b, &c);
            }
            break;
        default:
            break;
        }
#undef MKSR
#undef QUEUE3
    }
    return S_OK;
}

HRESULT Core_SetRenderState(D3DRENDERSTATETYPE State, DWORD Value)
{
    Trace("Core_SetRenderState");
    RsCensus(State, Value);
    switch (State) {
    case D3DRS_ALPHABLENDENABLE:  SR_st.blendOn    = (int)Value; break;
    case D3DRS_SRCBLEND:          SR_st.srcBlend   = (int)Value; break;
    case D3DRS_DESTBLEND:         SR_st.dstBlend   = (int)Value; break;
    case D3DRS_ALPHATESTENABLE:   SR_st.alphaTest  = (int)Value; break;
    case D3DRS_ALPHAREF:          SR_st.alphaRef   = Value;      break;
    case D3DRS_ZWRITEENABLE:      SR_st.zwrite     = (int)Value; break;
    default: break;
    }
    return S_OK;
}
HRESULT Core_GetRenderState(D3DRENDERSTATETYPE, DWORD *v) { if (v) *v = 0; return S_OK; }
HRESULT Core_SetTransform(D3DTRANSFORMSTATETYPE, CONST D3DMATRIX *) { Trace("Core_SetTransform"); return S_OK; }
HRESULT Core_SetViewport(CONST D3DVIEWPORT9 *v)
{ (void)v; Trace("Core_SetViewport"); return S_OK; }

HRESULT Core_CreateTexture(UINT W, UINT H, UINT Levels, DWORD Usage,
                           D3DFORMAT Format, D3DPOOL Pool,
                           IDirect3DTexture9 **ppTexture, HANDLE *)
{
    ShimTexture9 *t = NULL;
    int i;
    Trace("Core_CreateTexture");
    if (ppTexture) *ppTexture = NULL;
    if (W == 0 || H == 0) return D3DERR_INVALIDCALL;

    /* 先找空闲槽位 (Release 会归还), 找不到再用新槽 */
    for (i = 0; i < g_texUsed; i++) {
        if (!g_texPool[i].inUse) { t = &g_texPool[i]; break; }
    }
    if (!t) {
        if (g_texUsed >= MAX_TEX) {
            LogF("[!] 纹理池满 (%d) -> 返回失败", MAX_TEX);
            return D3DERR_INVALIDCALL;
        }
        t = &g_texPool[g_texUsed++];
    }

    /* 【延迟释放的真正清理点】
       Release 只把 inUse 置 0 (不清数据, 因为游戏可能还绑着);
       真正归还内存放在这里 —— 槽位被复用时。
       不清就会泄漏: 旧 bits/sr.px 的指针被覆盖, 谁也没法再 free。 */
    if (t->bits)  { free(t->bits);  t->bits  = NULL; }
    if (t->sr.px) { free(t->sr.px); t->sr.px = NULL; }

    /* Never memset the whole object:
       the first 4 bytes hold the vtable pointer.  Zeroing it makes any
       subsequent virtual call jump through a null pointer (0xC0000005).
       Reset only the data members. */
    t->inUse  = 1;
    t->ref    = 1;
    t->w      = W;
    t->h      = H;
    t->fmt    = Format;
    t->bits   = NULL;
    t->pitch  = 0;
    t->locked = 0;
    memset(&t->sr, 0, sizeof(t->sr));

    {
        static int n0 = 0;
        if (n0 < 4) { LogF("[tex] vptr=%p 纹理 %ux%u (槽位复用已启用)", *(void **)t, W, H); n0++; }
    }
    if (ppTexture) *ppTexture = t;
    return S_OK;
}
HRESULT Core_CreateVertexBuffer(UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9 **pp, HANDLE *)
{ LogStub("CreateVertexBuffer"); if (pp) *pp = NULL; return D3DERR_INVALIDCALL; }
HRESULT Core_CreateIndexBuffer(UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9 **pp, HANDLE *)
{ LogStub("CreateIndexBuffer"); if (pp) *pp = NULL; return D3DERR_INVALIDCALL; }
/* ---- 纹理绑定状况 ---- */
HRESULT Core_SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture)
{
    Trace("Core_SetTexture");
    if (Stage != 0) return S_OK;
    if (!pTexture) {
        static int n0 = 0;
        g_nullTex++;
        if (n0 < 3) { LogF("[tex] SetTexture(0)=NULL (第%d次)", g_nullTex); n0++; }
        SR_st.texOn = 0; SR_boundTex = NULL; return S_OK;
    }
    {
        ShimTexture9 *t = (ShimTexture9 *)pTexture;
        g_realTex++;
        static int n1 = 0;
        if (n1 < 8) {
            LogF("[tex] SetTexture(0)=%p  %ux%u fmt=%d created=%d valid=%d",
                 (void *)t, t->w, t->h, (int)t->fmt, t->bits ? 1 : 0, t->sr.valid);
            n1++;
        }
        SR_boundTex = &t->sr;
        SR_st.texOn = 1;
        TexHold(t);      /* 帧内保活 —— 否则 Flush 时已被回收, 贴图会变白 */

        /* 【纹素取证】把最早几张纹理导出成 BMP。
           症状“游戏里黑块/空气”但导出的帧看着正常 -> 必须直接看纹理内容。 */
        if (g_texDumped < 8) {
            char path[MAX_PATH];
            sprintf_s(path, MAX_PATH, "D:\\th123\\tex_%d.bmp", g_texDumped);
            if (DumpTexBmp(t, path)) {
                long long sum = 0;
                int nz = 0, amin = 255, amax = 0;
                int k, n = t->sr.w * t->sr.h;
                for (k = 0; k < n; k++) {
                    unsigned int c = t->sr.px[k];
                    unsigned int a = (c >> 24) & 0xFF;
                    sum += (c & 0x00FFFFFFu);
                    if (c & 0x00FFFFFFu) nz++;
                    if ((int)a < amin) amin = (int)a;
                    if ((int)a > amax) amax = (int)a;
                }
                LogF("[texdump] #%d %ux%u fmt=%d rgbSum=%lld 非黑=%d/%d alpha=%d..%d -> %s",
                     g_texDumped, t->sr.w, t->sr.h, (int)t->fmt, sum, nz, n, amin, amax, path);
            }
            g_texDumped++;
        }
    }
    return S_OK;
}
HRESULT Core_SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
    if (Stage != 0) return S_OK;
    switch (Type) {
    case D3DTSS_COLOROP:
        /* D3DTOP_MODULATE=2 默认; D3DTOP_SELECTARG1=2? 实际: DISABLE=1,SELECTARG1=2,
           SELECTARG2=3,MODULATE=4,ADD=7 -> 映到我们内部的 0/1/2 */
        if      (Value == D3DTOP_SELECTARG1 || Value == D3DTOP_SELECTARG2) SR_st.colorOp = 1;
        else if (Value == D3DTOP_ADD)      SR_st.colorOp = 2;
        else if (Value == D3DTOP_DISABLE)  SR_st.texOn   = 0;
        else                               SR_st.colorOp = 0;
        break;
    default: break;
    }
    return S_OK;
}
HRESULT Core_SetSamplerState(DWORD, D3DSAMPLERSTATETYPE, DWORD) { return S_OK; }
HRESULT Core_DrawPrimitive(D3DPRIMITIVETYPE, UINT, UINT) { LogStub("DrawPrimitive"); return S_OK; }
HRESULT Core_DrawIndexedPrimitive(D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT)
{ LogStub("DrawIndexedPrimitive"); return S_OK; }
HRESULT Core_SetFVF(DWORD Fvf)
{
    Trace("Core_SetFVF");
    FvfCensus(Fvf);
    return S_OK;
}
HRESULT Core_SetStreamSource(UINT, IDirect3DVertexBuffer9 *, UINT, UINT) { LogStub("SetStreamSource"); return S_OK; }
HRESULT Core_SetIndices(IDirect3DIndexBuffer9 *) { LogStub("SetIndices"); return S_OK; }

/* ================= 最小 Surface / SwapChain ================= */
/* Minimal surface / swap-chain objects.  The game does not check failure
   codes, so these must be real objects (never NULL). */

/* 前置声明: H*_ 实现在下面, 类体里要用到 */
HRESULT HF_GetDevice(IDirect3DDevice9 **pp);
HRESULT HF_GetDesc(D3DSURFACE_DESC *p);
HRESULT HF_LockRect(D3DLOCKED_RECT *p, CONST RECT *r, DWORD f);
HRESULT HF_UnlockRect();
HRESULT HF_GetDC(HDC *p);
HRESULT HF_ReleaseDC(HDC h);
HRESULT HF_SetPrivateData(REFGUID g, const void *d, DWORD s, DWORD f);
HRESULT HF_GetPrivateData(REFGUID g, void *d, DWORD *s);
HRESULT HF_FreePrivateData(REFGUID g);

HRESULT HS_Present(CONST RECT *a, CONST RECT *b, HWND h, CONST RGNDATA *r, DWORD f);
HRESULT HS_GetFrontBufferData(IDirect3DSurface9 *p);
HRESULT HS_GetBackBuffer(UINT i, D3DBACKBUFFER_TYPE t, IDirect3DSurface9 **pp);
HRESULT HS_GetRasterStatus(D3DRASTER_STATUS *p);
HRESULT HS_GetDisplayMode(D3DDISPLAYMODE *p);
HRESULT HS_GetDevice(IDirect3DDevice9 **pp);
HRESULT HS_GetPresentParameters(D3DPRESENT_PARAMETERS *p);

class ShimSurface9; class ShimSwapChain9;
static ShimSurface9   *g_backBufferPtr = NULL;
static ShimSwapChain9 *g_swapChainPtr  = NULL;

class ShimSurface9 : public IDirect3DSurface9
{
public:
    ULONG STDMETHODCALLTYPE AddRef()  override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **p) override
    { if (!p) return E_POINTER; *p = this; return S_OK; }
#include "stubs_surface9.h"
};

class ShimSwapChain9 : public IDirect3DSwapChain9
{
public:
    ULONG STDMETHODCALLTYPE AddRef()  override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **p) override
    { if (!p) return E_POINTER; *p = this; return S_OK; }
#include "stubs_swapchain9.h"
};

static ShimSurface9   g_backBuffer;
static ShimSwapChain9 g_swapChain;

/* ================= 特殊方法实现 (自由函数) ================= */

/* --- IDirect3DSurface9 --- */
HRESULT HF_GetDevice(IDirect3DDevice9 **pp) { if (pp) *pp = g_devPtr; return S_OK; }
HRESULT HF_GetDesc(D3DSURFACE_DESC *p)
{
    if (!p) return D3DERR_INVALIDCALL;
    memset(p, 0, sizeof(*p));
    p->Width = 640; p->Height = 480;
    p->Format = D3DFMT_X8R8G8B8;
    p->Type = D3DRTYPE_SURFACE; p->Pool = D3DPOOL_DEFAULT;
    return S_OK;
}
HRESULT HF_LockRect(D3DLOCKED_RECT *, CONST RECT *, DWORD) { LogStub("LockRect"); return D3DERR_INVALIDCALL; }
HRESULT HF_UnlockRect() { return S_OK; }
HRESULT HF_GetDC(HDC *p) { if (p) *p = NULL; return D3DERR_INVALIDCALL; }
HRESULT HF_ReleaseDC(HDC) { return S_OK; }
HRESULT HF_SetPrivateData(REFGUID, const void *, DWORD, DWORD) { return S_OK; }
HRESULT HF_GetPrivateData(REFGUID, void *, DWORD *) { return D3DERR_INVALIDCALL; }
HRESULT HF_FreePrivateData(REFGUID) { return S_OK; }

/* --- IDirect3DSwapChain9 --- */
HRESULT HS_Present(CONST RECT *, CONST RECT *, HWND, CONST RGNDATA *, DWORD)
{ return Core_Present(NULL, NULL, NULL, NULL); }
HRESULT HS_GetFrontBufferData(IDirect3DSurface9 *) { return D3DERR_INVALIDCALL; }
HRESULT HS_GetBackBuffer(UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **pp)
{ if (pp) *pp = &g_backBuffer; return S_OK; }
HRESULT HS_GetRasterStatus(D3DRASTER_STATUS *p) { if (p) memset(p, 0, sizeof(*p)); return S_OK; }
HRESULT HS_GetDisplayMode(D3DDISPLAYMODE *p)
{ if (!p) return D3DERR_INVALIDCALL;
  p->Width = 640; p->Height = 480; p->RefreshRate = 60; p->Format = D3DFMT_X8R8G8B8; return S_OK; }
HRESULT HS_GetDevice(IDirect3DDevice9 **pp) { if (pp) *pp = g_devPtr; return S_OK; }
HRESULT HS_GetPresentParameters(D3DPRESENT_PARAMETERS *p)
{ if (!p) return D3DERR_INVALIDCALL; memset(p, 0, sizeof(*p));
  p->BackBufferWidth = 640; p->BackBufferHeight = 480;
  p->BackBufferFormat = D3DFMT_X8R8G8B8; p->BackBufferCount = 2; p->Windowed = TRUE; return S_OK; }

/* --- IDirect3DDevice9 --- */

/* 统一的设备能力表。
   之前 memset 后只填了几个字段 -> 纹理/过滤/寻址能力全是 0，
   游戏查到"不支持纹理"就跳过 CreateTexture（实测 0 次），
   于是 SetTexture 永远是 NULL -> 全部白物体。
   这里按真实 D3D9 HAL 设备给全。 */
static void FillCaps9(D3DCAPS9 *p)
{
    memset(p, 0, sizeof(*p));
    p->DeviceType            = D3DDEVTYPE_HAL;
    p->Caps2                 = D3DCAPS2_CANMANAGERESOURCE | D3DCAPS2_DYNAMICTEXTURES | D3DCAPS2_FULLSCREENGAMMA;
    p->PresentationIntervals = 0x8000000F;
    p->CursorCaps            = 1;
    p->DevCaps               = D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_EXECUTESYSTEMMEMORY
                             | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY
                             | D3DDEVCAPS_TEXTURESYSTEMMEMORY;
    p->PrimitiveMiscCaps     = 0x000FFFFF;
    p->RasterCaps            = 0x0FFFFFFF;
    p->ZCmpCaps              = 0x000000FF;
    p->SrcBlendCaps          = 0x000007FF;
    p->DestBlendCaps         = 0x000007FF;
    p->AlphaCmpCaps          = 0x000000FF;
    p->ShadeCaps             = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_ALPHAGOURAUDBLEND
                             | D3DPSHADECAPS_FOGGOURAUD;
    p->TextureCaps           = D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_PERSPECTIVE
                             | D3DPTEXTURECAPS_POW2   | D3DPTEXTURECAPS_ALPHAPALETTE;
    p->TextureFilterCaps     = 0x03030303;
    p->CubeTextureFilterCaps = 0x03030303;
    p->VolumeTextureFilterCaps = 0x03030303;
    p->TextureAddressCaps    = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR
                             | D3DPTADDRESSCAPS_CLAMP | D3DPTADDRESSCAPS_BORDER
                             | D3DPTADDRESSCAPS_INDEPENDENTUV;
    p->VolumeTextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_CLAMP;
    p->LineCaps              = 0x0000000F;
    p->MaxTextureWidth       = g_gpu.maxTex;   /* 按真卡规格报 */
    p->MaxTextureHeight      = g_gpu.maxTex;
    p->MaxVolumeExtent       = 256;
    p->MaxTextureRepeat      = 8192;
    p->MaxTextureAspectRatio = 2048;
    p->MaxAnisotropy         = 16;
    p->MaxVertexW            = 1.0e10f;
    p->StencilCaps           = 0x000000FF;
    p->FVFCaps               = 0x00000108;
    p->TextureOpCaps         = 0x03FEFFFF;
    p->MaxTextureBlendStages = 8;
    p->MaxSimultaneousTextures = 8;
    p->VertexProcessingCaps  = D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_MATERIALSOURCE7
                             | D3DVTXPCAPS_DIRECTIONALLIGHTS | D3DVTXPCAPS_POSITIONALLIGHTS
                             | D3DVTXPCAPS_LOCALVIEWER | D3DVTXPCAPS_TWEENING;
    p->MaxActiveLights       = 8;
    p->MaxUserClipPlanes     = 6;
    p->MaxVertexBlendMatrices = 4;
    p->MaxVertexBlendMatrixIndex = 0;
    p->MaxPointSize          = 64.0f;
    p->MaxPrimitiveCount     = 0xFFFF;
    p->MaxVertexIndex        = 0xFFFF;
    p->MaxStreams            = 16;
    p->MaxStreamStride       = 508;
    p->VertexShaderVersion   = g_gpu.vsVer;   /* 报真卡支持的 shader 版本 */
    p->MaxVertexShaderConst  = 256;
    p->PixelShaderVersion    = g_gpu.psVer;
    p->MaxPixelShader30InstructionSlots = 0;
}

HRESULT HD_GetDeviceCaps(D3DCAPS9 *p)
{
    LogStub("HD_GetDeviceCaps");
    if (!p) return D3DERR_INVALIDCALL;
    FillCaps9(p);
    LogF("    [D3D9] DeviceCaps: texMax=%ux%u texCaps=0x%X filterCaps=0x%X addrCaps=0x%X",
         p->MaxTextureWidth, p->MaxTextureHeight, p->TextureCaps,
         p->TextureFilterCaps, p->TextureAddressCaps);
    return S_OK;
}
HRESULT HD_GetDirect3D(IDirect3D9 **pp)
{
    if (!pp) return D3DERR_INVALIDCALL;
    *pp = g_d3d9Ptr;
    LogF("[gpu] GetDirect3D -> %p", (void *)g_d3d9Ptr);
    return g_d3d9Ptr ? S_OK : D3DERR_INVALIDCALL;
}

HRESULT HD_GetDisplayMode(UINT, D3DDISPLAYMODE *p)
{ if (!p) return D3DERR_INVALIDCALL;
  p->Width = 640; p->Height = 480; p->RefreshRate = 60; p->Format = D3DFMT_X8R8G8B8; return S_OK; }
HRESULT HD_GetSwapChain(UINT, IDirect3DSwapChain9 **pp)
{ LogStub("GetSwapChain"); if (pp) *pp = &g_swapChain; return S_OK; }
HRESULT HD_GetBackBuffer(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **pp)
{ LogStub("GetBackBuffer"); if (pp) *pp = &g_backBuffer; return S_OK; }
HRESULT HD_GetRenderTarget(DWORD, IDirect3DSurface9 **pp)
{ LogStub("GetRenderTarget"); if (pp) *pp = &g_backBuffer; return S_OK; }
HRESULT HD_GetDepthStencilSurface(IDirect3DSurface9 **pp)
{ LogStub("GetDepthStencilSurface"); if (pp) *pp = &g_backBuffer; return S_OK; }
HRESULT HD_GetFrontBufferData(UINT, IDirect3DSurface9 *) { return D3DERR_INVALIDCALL; }

/* ---- Reset: 切全屏 / 改分辨率的【唯一】通路 ----
   之前是空桩 (返回 S_OK 但什么都不做) -> 游戏以为切了, 实际还在窗口里,
   A stub here means the game believes it switched but nothing happens.
   这里转发给真设备, 并按新尺寸重建离屏面 (否则尺寸不匹配会画错)。 */
HRESULT HD_Reset(D3DPRESENT_PARAMETERS *pp)
{
    LogF("[Reset] 请求 %ux%u fmt=%d windowed=%d interval=0x%X",
         pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
         pp ? (int)pp->BackBufferFormat : -1,
         pp ? (pp->Windowed ? 1 : 0) : -1,
         pp ? (unsigned)pp->PresentationInterval : 0);
    if (!pp) return D3DERR_INVALIDCALL;

    if (O_Reset(pp)) return S_OK;

    /* 真设备也失败了: 至少把尺寸同步过去, 别让后续绘制用旧尺寸 */
    if (pp->BackBufferWidth && pp->BackBufferHeight) {
        O_w = pp->BackBufferWidth;
        O_h = pp->BackBufferHeight;
        SR_Reset((int)O_w * SR_ss, (int)O_h * SR_ss);
    }
    return D3DERR_DEVICELOST;
}

/* ---- 动态纹理更新 ----
   字体/菜单面板走这条: 游戏先把像素写进一张 sysmem 纹理, 再 UpdateTexture
   拷到目标纹理。原来是空桩 -> 像素永远传不过去 -> 字体/菜单画不出来。
   两块都是我们的 ShimTexture9, 源码已在 bits 里, 直接重转一遍即可。 */
HRESULT HD_UpdateTexture(IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst)
{
    ShimTexture9 *s = (ShimTexture9 *)src;
    ShimTexture9 *d = (ShimTexture9 *)dst;
    static int n = 0;
    if (n < 6) { LogF("[tex] UpdateTexture(%p -> %p)", (void *)src, (void *)dst); n++; }
    if (!s || !d) return D3DERR_INVALIDCALL;

    /* 尺寸/格式取目标; 像素从源拷贝 (先保证源 bits 存在) */
    if (!s->bits) return D3DERR_INVALIDCALL;

    if (!d->bits || d->w != s->w || d->h != s->h) {
        if (d->bits) free(d->bits);
        d->w = s->w; d->h = s->h; d->fmt = s->fmt;
        d->pitch = s->pitch;
        d->bits = (unsigned char *)malloc((size_t)d->pitch * d->h);
        if (!d->bits) return E_OUTOFMEMORY;
    }
    memcpy(d->bits, s->bits, (size_t)d->pitch * d->h);
    return HT_Convert(d);
}

HRESULT HD_UpdateSurface(IDirect3DSurface9 *srcSurf, CONST RECT *srcRect,
                         IDirect3DSurface9 *dstSurf, CONST POINT *dstPt)
{
    static int n = 0;
    if (n < 6) { LogF("[tex] UpdateSurface(%p -> %p)", (void *)srcSurf, (void *)dstSurf); n++; }
    /* 我们的 surface 都是纹理级别的薄壳; 有 owner 就重转那个纹理 */
    if (srcSurf && dstSurf) {
        ShimTexSurf9 *ds = (ShimTexSurf9 *)dstSurf;
        if (ds->owner) return HT_Convert(ds->owner);
    }
    return S_OK;
}

/* --- IDirect3D9 --- */
static UINT HD_GetAvailableTextureMem_Val(void)
{
    UINT total = VRAM_TOTAL;
    UINT used  = (UINT)g_texUsed * 256 * 1024;   /* 粗估: 每张按 256KB */
    UINT avail = (used < total) ? (total - used) : (total / 2);
    static int logged = 0;
    if (!logged) {
        LogF("[gpu] GetAvailableTextureMem -> %u MB (总 %u MB, 已建纹理 %d 张)",
             avail / (1024 * 1024), total / (1024 * 1024), g_texUsed);
        logged = 1;
    }
    return avail;
}

/* --- IDirect3D9 --- */

/* 必须 >=1。报 0 会让 D3DX 认为"没有可用显卡"从而不建纹理。 */
UINT H9_GetAdapterCount(void) { return 1; }

HMONITOR H9_GetAdapterMonitor(UINT) {
    /* 游戏窗口所在显示器；窗口还没有时回主显示器 */
    if (g_hwnd) return MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
    return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

HRESULT H9_GetDeviceCaps(UINT, D3DDEVTYPE DeviceType, D3DCAPS9 *p)
{
    LogStub("H9_GetDeviceCaps");
    if (!p) return D3DERR_INVALIDCALL;
    FillCaps9(p);
    p->DeviceType = DeviceType;
    LogF("    [D3D9] AdapterCaps: texMax=%ux%u texCaps=0x%X",
         p->MaxTextureWidth, p->MaxTextureHeight, p->TextureCaps);
    return S_OK;
}
HRESULT H9_GetAdapterDisplayMode(UINT, D3DDISPLAYMODE *p)
{
    LogStub("H9_GetAdapterDisplayMode");
    if (!p) return D3DERR_INVALIDCALL;
    p->Width = 640; p->Height = 480; p->RefreshRate = 60; p->Format = D3DFMT_X8R8G8B8;
    LogF("    [D3D9] 报告 640x480@60");
    return S_OK;
}
HRESULT H9_GetAdapterIdentifier(UINT, DWORD, D3DADAPTER_IDENTIFIER9 *p)
{
    LogStub("H9_GetAdapterIdentifier");
    if (!p) return D3DERR_INVALIDCALL;
    memset(p, 0, sizeof(*p));
    strncpy_s(p->Driver, sizeof(p->Driver), "nvd3dum.dll", _TRUNCATE);
    strncpy_s(p->Description, sizeof(p->Description), g_gpu.name, _TRUNCATE);
    p->VendorId = g_gpu.vendorId;
    p->DeviceId = g_gpu.deviceId;
    p->Revision = 1;
    LogF("    [D3D9] 自称 %s (vendor 0x%04X device 0x%04X)",
         g_gpu.name, g_gpu.vendorId, g_gpu.deviceId);
    return S_OK;
}

/* 崩溃转储: 未处理异常过滤器。
   上面 9 个纹理建成后就 0xC0000005, 且不再经过任何被拦截的调用 ——
   说明死在看不到的地方。让系统告诉我们【哪个模块的哪个地址】炸的。 */
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS *ep)
{
    char mod[MAX_PATH] = "?";
    HMODULE h = NULL;
    DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    void *addr = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionAddress : NULL;

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &h) && h)
        GetModuleFileNameA(h, mod, MAX_PATH);

    LogF("");
    LogF("*** 崩溃 code=0x%08X addr=%p", (unsigned)code, addr);
    LogF("*** 模块 = %s", mod);
    if (ep && ep->ExceptionRecord && ep->ExceptionRecord->NumberParameters >= 2) {
        LogF("*** 参数 = %p %p",
             (void *)ep->ExceptionRecord->ExceptionInformation[0],
             (void *)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    LogF("*** 已拦截调用数 = %d (真实计数, 日志只写前 %d 行)",
         CountCallTotal(), MAX_CALL_TRACE);
    {
        FILE *f = fopen("D:\\th123\\crash_modules.txt", "w");
        int i;
        if (f) {
            fprintf(f, "code=0x%08X addr=%p module=%s\n", (unsigned)code, addr, mod);
            for (i = 0; i < g_stubTotal; i++)
                fprintf(f, "%-40s %d\n", g_stubName[i], g_stubCount[i]);
            fclose(f);
        }
    }
    if (g_log) fflush(g_log);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* Vectored 版: 比 SetUnhandledExceptionFilter 更早触发，
   且游戏自己调 SetUnhandledExceptionFilter 也顶不掉它。 */
static LONG CALLBACK VecCrashFilter(EXCEPTION_POINTERS *ep)
{
    CrashFilter(ep);
    return EXCEPTION_CONTINUE_SEARCH;   /* 只记录，不吞异常 */
}

static void InstallCrashFilter(void)
{
    AddVectoredExceptionHandler(1, VecCrashFilter);
    SetUnhandledExceptionFilter(CrashFilter);
}

/* ================= 接口包装 ================= */
/* 【顺序铁律】QueryInterface/AddRef/Release 在前, 生成的方法按声明顺序,
   单独声明在最后的 CreateDevice 必须在末尾。 */

class ShimD3D9 : public IDirect3D9
{
public:
    ULONG STDMETHODCALLTYPE AddRef()  override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **ppv) override
    { if (!ppv) return E_POINTER; *ppv = this; return S_OK; }

#include "stubs_d3d9.h"

    /* CreateDevice 是 IDirect3D9 的最后一个方法 —— 必须留在末尾 */
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
                                           DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPP,
                                           IDirect3DDevice9 **ppOut) override
    {
        LogF("[+] CreateDevice: adapter=%u type=%d hwnd=%p", Adapter, (int)DeviceType, (void *)hFocusWindow);
        if (pPP) {
            LogF("    backbuffer %ux%u fmt=%d windowed=%d",
                 pPP->BackBufferWidth, pPP->BackBufferHeight,
                 (int)pPP->BackBufferFormat, pPP->Windowed);
            /* 【关键证据】游戏请求的 vsync 间隔。
               D3DPRESENT_INTERVAL_IMMEDIATE=0x80000000 (不等)
               D3DPRESENT_INTERVAL_ONE=1 (每帧等)  TWO=2  THREE=4
               如果游戏要的是 1, 而我们把它改成 DISCARD 且没保留 interval,
               游戏自带的帧配速就失效了 -> 它靠别的办法补, 表现异常。 */
            {
                DWORD iv = pPP->PresentationInterval;
                const char *desc = "?";
                if (iv == 0x80000000u) desc = "IMMEDIATE (不等 vsync)";
                else if (iv == 1) desc = "ONE (每帧等)";
                else if (iv == 2) desc = "TWO";
                else if (iv == 4) desc = "THREE";
                else if (iv == 0) desc = "0 (默认)";
                LogF("    游戏请求 PresentationInterval = 0x%08X  (%s)", iv, desc);
                LogF("    SwapEffect=%d FullScreen=%d RefreshRate=%u BackBufferCount=%u",
                     (int)pPP->SwapEffect, pPP->Windowed ? 0 : 1,
                     pPP->FullScreen_RefreshRateInHz, pPP->BackBufferCount);
            }
        }
        if (!ppOut) return E_POINTER;
        g_hwnd = hFocusWindow;

        /* 按 soku_shim.ini 的 Gpu= 选显卡身份（只生效一次） */
        {
            static int gpuDone = 0;
            if (!gpuDone) {
                char ini[MAX_PATH], val[64] = "";
                char *p;
                GetModuleFileNameA(NULL, ini, MAX_PATH);
                p = strrchr(ini, '\\'); if (p) p[1] = 0;
                strcat_s(ini, MAX_PATH, "soku_shim.ini");
                GetPrivateProfileStringA("soku_shim", "Gpu", "gf5200", val, sizeof(val), ini);
                GpuSelect(val);

                /* ---- 抗锯齿 (运行时可选) ----
                   AAMode       : off | ssaa | fxaa   (默认 off, 兼容弱机)
                   SuperSample  : ssaa 专用, 1..4  (1=关 2=4x 3=9x 4=16x)
                   FXAAStrength : fxaa 专用, 0..100 (默认 60) */
                {
                    char aa[32] = "";
                    int  si;

                    GetPrivateProfileStringA("soku_shim", "AAMode", "off", aa, sizeof(aa), ini);
                    if (_stricmp(aa, "fxaa") == 0)      O_aaMode = O_AA_FXAA;
                    else if (_stricmp(aa, "ssaa") == 0) O_aaMode = O_AA_SSAA;
                    else                                O_aaMode = O_AA_OFF;

                    if (O_aaMode == O_AA_SSAA) {
                        SR_SetSuperSample(GetPrivateProfileIntA("soku_shim", "SuperSample", 2, ini));
                    } else {
                        SR_SetSuperSample(1);   /* FXAA / OFF 都要 1:1 帧缓冲 */
                    }

                    si = GetPrivateProfileIntA("soku_shim", "FXAAStrength", 60, ini);
                    if (si < 0) si = 0; if (si > 100) si = 100;
                    O_aaStrength = (float)si * 0.01f;

                    LogF("[aa] 模式 = %s   超采样 x%d   FXAA强度 %.2f",
                         O_aaMode == O_AA_FXAA ? "FXAA" :
                         (O_aaMode == O_AA_SSAA ? "SSAA" : "OFF"),
                         SR_ss, (double)O_aaStrength);
                }
                gpuDone = 1;
            }
        }

        /* Create the real D3D9 output device. */
        if (O_CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPP)) {
            SR_Reset((int)O_w * SR_ss, (int)O_h * SR_ss);
            /* 启用多线程光栅 (核数-1 个工作线程, 主线程也参与分行带)
               实测 6 线程 4.40x (带纹理+混合场景) */
            SR_SetThreads(SR_TotalThreads());
            LogF("[gpu] 光栅线程数 = %d (+主线程)", SR_nThreads);

            *ppOut = (IDirect3DDevice9 *)g_devPtr;
            LogF("[+] 已返回 ShimDevice9 (输出端 = 真 D3D9)");
            return S_OK;
        }

        LogF("[!] D3D9 output unavailable - device not created");
        *ppOut = NULL;
        return D3DERR_NOTAVAILABLE;
    }
};

/* 前向声明: 设备类 */
class ShimDevice9;
static ShimDevice9 *g_devicePtr = NULL;

class ShimDevice9 : public IDirect3DDevice9
{
public:
    ULONG STDMETHODCALLTYPE AddRef()  override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **ppv) override
    { if (!ppv) return E_POINTER; *ppv = this; return S_OK; }

#include "stubs_device9.h"
};

static ShimDevice9 g_device;
static ShimD3D9    g_d3d9;

/* ================= 导出 ================= */
/* d3d9.h 里声明是 C++ 链接, 所以不加 extern "C", 名称由 d3d9.def 导出。 */
IDirect3D9 * WINAPI Direct3DCreate9(UINT SDKVersion)
{
    LogF("[>] Direct3DCreate9(sdk=%u)", SDKVersion);
    LogF("[i] shim active; draw calls are rasterized on the CPU");

    g_devPtr = &g_device;
    g_d3d9Ptr = &g_d3d9;
    g_backBufferPtr = &g_backBuffer;
    g_swapChainPtr  = &g_swapChain;
    g_hwnd = GetActiveWindow();
    return &g_d3d9;
}

HRESULT WINAPI Direct3DCreate9Ex(UINT, void **ppOut)
{ LogStub("Direct3DCreate9Ex"); if (ppOut) *ppOut = NULL; return E_NOTIMPL; }
void *  WINAPI Direct3DCreate9On12(UINT, void *, UINT)
{ LogStub("Direct3DCreate9On12"); return NULL; }
HRESULT WINAPI Direct3DCreate9On12Ex(UINT, void *, UINT, void **pp)
{ if (pp) *pp = NULL; return E_NOTIMPL; }
void *  WINAPI Direct3DShaderValidatorCreate9(void) { return NULL; }
int     WINAPI D3DPERF_BeginEvent(DWORD, const wchar_t *) { return 0; }
int     WINAPI D3DPERF_EndEvent(void) { return 0; }
DWORD   WINAPI D3DPERF_GetStatus(void) { return 0; }
BOOL    WINAPI D3DPERF_QueryRepeatFrame(void) { return FALSE; }
void    WINAPI D3DPERF_SetMarker(DWORD, const wchar_t *) {}
void    WINAPI D3DPERF_SetOptions(DWORD) {}
void    WINAPI D3DPERF_SetRegion(DWORD, const wchar_t *) {}
void    WINAPI DebugSetLevel(DWORD) {}
void    WINAPI DebugSetMute(void) {}
void    WINAPI PSGPError(void) {}
void    WINAPI PSGPSampleTexture(void) {}

BOOL WINAPI DllMain(HMODULE hSelf, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hSelf);
        InstallCrashFilter();
        g_log = fopen("shim_cpu.log", "w");
        g_callLog = fopen("shim_calls.log", "w");
        LogF("[i] shim.cpp loaded (D3D9 -> CPU software rasterizer)");
    } else if (reason == DLL_PROCESS_DETACH) {
        DumpStubSummary();
        LogF("[i] 卸载 (共追踪 %d 次调用)", g_callCount);
        if (g_log) { fclose(g_log); g_log = NULL; }
        if (g_callLog) { fclose(g_callLog); g_callLog = NULL; }
    }
    return TRUE;
}
