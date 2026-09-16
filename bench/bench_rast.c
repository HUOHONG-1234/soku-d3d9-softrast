/* bench_rast.c -- 光栅器独立压测台 (含多线程对比)
 *
 * 为什么需要: 优化必须能立刻量化, 不能每次去开操作员的游戏。
 * 场景 A 最贴近真实负载 (带纹理 + alpha 混合 + MODULATE, 角色/特效走这条)。
 *
 * 走【批处理路径】而非立即模式 —— 因为多线程只在批处理下生效。
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "softrast.h"

#define W 640
#define H 480
#define TSIDE 256
#define QUADS 400
#define ITERS 60

static unsigned int g_tex[TSIDE * TSIDE];
static SR_Tex       g_srtex;

static LARGE_INTEGER g_freq;

static double Now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)g_freq.QuadPart;
}

static void MakeQuad(SR_Vtx *a, SR_Vtx *b, SR_Vtx *c, SR_Vtx *d,
                     float x, float y, float side, unsigned int col, int uv)
{
    float u1 = side / TSIDE;
    a->x = x;      a->y = y;      a->col = col; a->u = 0;  a->v = 0;  a->hasUV = uv;
    b->x = x+side; b->y = y;      b->col = col; b->u = u1; b->v = 0;  b->hasUV = uv;
    c->x = x+side; c->y = y+side; c->col = col; c->u = u1; c->v = u1; c->hasUV = uv;
    d->x = x;      d->y = y+side; d->col = col; d->u = 0;  d->v = u1; d->hasUV = uv;
}

/* 跑一组四边形, 走批处理 + N 线程 */
static double RunBatch(int nThreads, double *outPixels)
{
    int i, k;
    double t0, t1;
    unsigned int seed = 12345;

    SR_SetThreads(nThreads);

    /* 预热 */
    SR_BeginFrame();
    {
        SR_Vtx a, b, c, d;
        MakeQuad(&a, &b, &c, &d, 0, 0, 32, 0xC0FFFFFFu, 1);
        SR_Queue(&a, &b, &c, &SR_st, &g_srtex);
        SR_Queue(&a, &c, &d, &SR_st, &g_srtex);
    }
    SR_Flush();

    t0 = Now();
    for (k = 0; k < ITERS; k++) {
        SR_BeginFrame();
        for (i = 0; i < QUADS; i++) {
            SR_Vtx a, b, c, d;
            float x, y;
            seed = seed * 1103515245u + 12345u;
            x = (float)((seed >> 16) % (W - 64));
            seed = seed * 1103515245u + 12345u;
            y = (float)((seed >> 16) % (H - 64));
            MakeQuad(&a, &b, &c, &d, x, y, 64, 0xC0FFFFFFu, 1);
            SR_Queue(&a, &b, &c, &SR_st, &g_srtex);
            SR_Queue(&a, &c, &d, &SR_st, &g_srtex);
        }
        SR_Flush();
    }
    t1 = Now();

    *outPixels = (double)QUADS * ITERS * 64.0 * 64.0;
    return t1 - t0;
}

int main(void)
{
    int i;
    double px, secs, base = 0.0;

    QueryPerformanceFrequency(&g_freq);

    for (i = 0; i < TSIDE * TSIDE; i++) {
        int x = i % TSIDE, y = i / TSIDE;
        unsigned int a = (unsigned int)((x ^ y) & 0xFF);
        if ((x / 16 + y / 16) & 1) a = 255;
        g_tex[i] = (a << 24) | (((unsigned)(x) & 0xFF) << 16)
                 | (((unsigned)(y) & 0xFF) << 8) | 0x80u;
    }
    g_srtex.w = TSIDE; g_srtex.h = TSIDE; g_srtex.px = g_tex; g_srtex.valid = 1;

    SR_InitState();
    if (!SR_Reset(W, H)) { printf("SR_Reset failed\n"); return 1; }
    SR_st.texOn = 1;
    SR_st.blendOn = 1;
    SR_st.srcBlend = 5;
    SR_st.dstBlend = 6;
    SR_st.colorOp = 0;
    SR_boundTex = &g_srtex;

    printf("=== rasterizer bench (%dx%d, texture %dx%d) ===\n", W, H, TSIDE, TSIDE);
    printf("scene: %d quads x %d iters, textured + alpha blend + MODULATE\n\n", QUADS, ITERS);
    printf("%-14s %10s %14s %12s %8s\n", "threads", "sec", "pixels", "M px/s", "speedup");
    printf("-------------------------------------------------------------------\n");

    {
        static const int T[] = { 1, 2, 3, 4, 6, 7 };
        int k;
        for (k = 0; k < (int)(sizeof(T) / sizeof(T[0])); k++) {
            secs = RunBatch(T[k], &px);
            {
                double mpps = px / secs / 1e6;
                if (T[k] == 1) base = mpps;
                printf("%-14d %10.3f %14.0f %12.1f %7.2fx\n",
                       T[k], secs, px, mpps, base > 0 ? mpps / base : 1.0);
            }
        }
    }
    SR_ShutdownThreads();

    printf("-------------------------------------------------------------------\n");
    printf("for reference (2005 GPUs, theoretical fill rate):\n");
    printf("  GeForce 6200   4 ROP @ 300MHz = 1200 M px/s\n");
    printf("  GeForce 6600LE 4 ROP @ 300MHz = 1200 M px/s\n");
    printf("  GeForce 6800U 16 ROP @ 425MHz = 6800 M px/s\n");
    printf("\ncounters: triIn=%lld degen=%lld offscreen=%lld spans=%lld px=%lld\n",
           g_triIn, g_triDegenerate, g_triOffscreen, g_spans, g_spanPix);
    return 0;
}
