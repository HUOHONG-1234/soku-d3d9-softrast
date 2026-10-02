/* test_rast.cpp -- coverage unit tests for the rasterizer core
 *
 * Motivation: mirrored sprites (opponent facing the other way) were not
 * being drawn at all.  The cause was triangle winding: a horizontally
 * flipped sprite reverses the vertex order, and the rasterizer only
 * accepted one winding.
 *
 * These tests feed known geometry and measure coverage, so winding and
 * pixel-centre rounding regressions are caught immediately rather than by
 * visual inspection.
 *
 * Build: cl /I src test_rast.cpp
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "softrast.h"

static int CountNonBlack(void)
{
    int i, n = SR_w * SR_h, c = 0;
    unsigned int clear = 0xFF000000;
    for (i = 0; i < n; i++)
        if ((SR_buf[i] & 0x00FFFFFFu) != (clear & 0x00FFFFFFu)) c++;
    return c;
}

static int CountColor(unsigned int want)
{
    int i, n = SR_w * SR_h, c = 0;
    for (i = 0; i < n; i++)
        if ((SR_buf[i] & 0x00FFFFFFu) == (want & 0x00FFFFFFu)) c++;
    return c;
}

#define W 64
#define H 64

static unsigned int g_texbuf[16 * 16];
static SR_Tex       g_tex;

int main(void)
{
    SR_Vtx a, b, c, d;
    int fails = 0;

    SR_InitState();
    SR_Reset(W, H);
    SR_st.texOn = 0;
    SR_st.blendOn = 0;
    SR_st.alphaTest = 0;
    SR_boundTex = NULL;

    memset(g_texbuf, 0, sizeof(g_texbuf));
    { int i; for (i = 0; i < 16 * 16; i++) g_texbuf[i] = 0xFF00FF00u; }   /* 纯绿 */
    g_tex.w = 16; g_tex.h = 16; g_tex.px = g_texbuf; g_tex.valid = 1;

    printf("=== 光栅覆盖率测试 (画布 %dx%d) ===\n\n", W, H);

    /* ---------- T1: 轴对齐四边形, 绕序 A ---------- */
    SR_Clear(0xFF000000);
    a.x=10; a.y=10; a.col=0xFFFF0000u; a.u=0; a.v=0; a.hasUV=0;
    b.x=30; b.y=10; b.col=0xFFFF0000u; b.u=0; b.v=0; b.hasUV=0;
    c.x=30; c.y=30; c.col=0xFFFF0000u; c.u=0; c.v=0; c.hasUV=0;
    d.x=10; d.y=30; d.col=0xFFFF0000u; d.u=0; d.v=0; d.hasUV=0;
    SR_Triangle(&a,&b,&c);
    SR_Triangle(&a,&c,&d);
    {
        int n = CountNonBlack();
        printf("T1 四边形 绕序A      %4d px  (期望 400)  %s\n", n,
               (n >= 390 && n <= 410) ? "PASS" : "*** FAIL ***");
        if (!(n >= 390 && n <= 410)) fails++;
    }

    /* ---------- T2: 同一四边形, 反向绕序 ---------- */
    SR_Clear(0xFF000000);
    SR_Triangle(&a,&d,&c);
    SR_Triangle(&a,&c,&b);
    {
        int n = CountNonBlack();
        printf("T2 四边形 绕序B(反)  %4d px  (期望 400)  %s\n", n,
               (n >= 390 && n <= 410) ? "PASS" : "*** FAIL ***");
        if (!(n >= 390 && n <= 410)) fails++;
    }

    /* ---------- T3: 水平镜像位置 (模拟反面朝上的对手精灵) ---------- */
    SR_Clear(0xFF000000);
    a.x=54; b.x=34; c.x=34; d.x=54;
    SR_Triangle(&a,&b,&c);
    SR_Triangle(&a,&c,&d);
    {
        int n = CountNonBlack();
        printf("T3 矩形 镜像位置     %4d px  (期望 400)  %s\n", n,
               (n >= 390 && n <= 410) ? "PASS" : "*** FAIL ***");
        if (!(n >= 390 && n <= 410)) fails++;
    }

    /* ---------- T4: 全屏四边形 ---------- */
    SR_Clear(0xFF000000);
    a.x=0; a.y=0; b.x=W; b.y=0; c.x=W; c.y=H; d.x=0; d.y=H;
    SR_Triangle(&a,&b,&c);
    SR_Triangle(&a,&c,&d);
    {
        int n = CountNonBlack();
        printf("T4 全屏四边形        %4d px  (期望 4096) %s\n", n,
               (n >= 4050 && n <= 4100) ? "PASS" : "*** FAIL ***");
        if (!(n >= 4050 && n <= 4100)) fails++;
    }

    /* ---------- T5: 细长条 (斜边接近水平) ---------- */
    SR_Clear(0xFF000000);
    a.x=10; a.y=10; b.x=50; b.y=11; c.x=50; c.y=13; d.x=10; d.y=12;
    SR_Triangle(&a,&b,&c);
    SR_Triangle(&a,&c,&d);
    {
        int n = CountNonBlack();
        printf("T5 斜细长条          %4d px  (>0 即通过)\n", n);
        if (n <= 0) { printf("     *** FAIL: 细长条完全没画出来 ***\n"); fails++; }
    }

    /* ---------- T6: 带纹理 (走采样路径) ---------- */
    SR_Clear(0xFF000000);
    SR_st.texOn = 1;
    SR_boundTex = &g_tex;
    SR_st.colorOp = 0;                     /* MODULATE */
    a.x=10; a.y=10; a.col=0xFFFFFFFFu; a.u=0.0f; a.v=0.0f;
    b.x=30; b.y=10; b.col=0xFFFFFFFFu; b.u=1.0f; b.v=0.0f;
    c.x=30; c.y=30; c.col=0xFFFFFFFFu; c.u=1.0f; c.v=1.0f;
    d.x=10; d.y=30; d.col=0xFFFFFFFFu; d.u=0.0f; d.v=1.0f;
    SR_Triangle(&a,&b,&c);
    SR_Triangle(&a,&c,&d);
    {
        int n = CountColor(0xFF00FF00u);
        printf("T6 带纹理四边形      %4d px  (期望 ~400 绿) %s\n", n,
               (n >= 380 && n <= 410) ? "PASS" : "*** FAIL ***");
        if (!(n >= 380 && n <= 410)) fails++;
    }

    /* ---------- T7: 顶点色渐变 (走插值路径) ---------- */
    SR_Clear(0xFF000000);
    SR_st.texOn = 0;
    SR_boundTex = NULL;
    a.x=10; a.y=10; a.col=0xFFFF0000u;
    b.x=30; b.y=10; b.col=0xFF00FF00u;
    c.x=30; c.y=30; c.col=0xFF0000FFu;
    SR_Triangle(&a,&b,&c);
    {
        int n = CountNonBlack();
        printf("T7 渐变三角形        %4d px  (期望 200)  %s\n", n,
               (n >= 190 && n <= 210) ? "PASS" : "*** FAIL ***");
        if (!(n >= 190 && n <= 210)) fails++;
    }

    /* ---------- T8: 零尺寸退化 ---------- */
    SR_Clear(0xFF000000);
    a.x=10; a.y=10; a.col=0xFFFFFFFFu;
    b.x=10; b.y=10; b.col=0xFFFFFFFFu;
    c.x=10; c.y=10; c.col=0xFFFFFFFFu;
    SR_Triangle(&a,&b,&c);
    {
        int n = CountNonBlack();
        printf("T8 退化点三角形      %4d px  (期望 0)    %s\n", n,
               (n == 0) ? "PASS" : "*** FAIL ***");
        if (n != 0) fails++;
    }

    /* 这七个计数器在 softrast_*.h 里都是 long long，全部必须用 %lld。
       原来 6 个写成 %d（只有 g_spanPix 对）—— 格式串与实参类型不匹配，
       CWE-686，在可变参数调用里会读到错误的位宽。 */
    printf("\n计数器: triIn=%lld 退化=%lld 屏外=%lld 扫描段=%lld 像素=%lld 无效采样=%lld 有效采样=%lld\n",
           g_triIn, g_triDegenerate, g_triOffscreen, g_spans, g_spanPix,
           g_whiteSample, g_validSample);

    printf("\n=== %s (失败 %d 项) ===\n", fails ? "有失败" : "全部通过", fails);
    return fails;
}
