/* test_blend.cpp -- regression test for the frame-batched blend-state bug.
 *
 * The bug: SR_BlendPixel used to read the GLOBAL SR_st.  With frame-level
 * batching the draw is queued (state snapshotted into the command) but
 * rasterized at end of frame -- by which time the game has usually changed
 * the global render state (typically switching alpha blending OFF for a
 * later draw).  So the blend branch was reached (the caller checks the
 * per-triangle snapshot) but the blend function saw blendOn==0 and did a
 * plain replace.  Result: sprites whose alpha is 0 wrote their raw RGB
 * (usually black) -> the "black boxes" / "transparency not handled" symptom.
 *
 * This test reproduces the sequence explicitly:
 *   1. enable blending, queue a triangle
 *   2. change the global state (simulating the rest of the frame)
 *   3. flush
 * and checks the blend was actually applied.
 *
 * It works against BOTH cores (avx2/sse2) because it only uses the public
 * Queue/Flush path, not SR_BlendPixel directly.
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "softrast.h"

#define W 64
#define H 64

static int g_fail = 0;

static unsigned int px(int x, int y)
{
    if (!SR_buf || x < 0 || y < 0 || x >= SR_w || y >= SR_h) return 0xDEADBEEFu;
    return SR_buf[(size_t)y * SR_w + x];
}

static void MkVtx(SR_Vtx *v, float x, float y, unsigned int col)
{
    v->x = x; v->y = y;
    v->u = 0.0f; v->v = 0.0f;
    v->col = col;
    v->hasUV = 0;
}

/* Draw a 20x20 quad with the given colour through the queued path, after
   deliberately clobbering the global state to simulate end-of-frame. */
static void QueuedQuad(unsigned int col, int blendOn, int srcB, int dstB,
                       int clobberBlendTo)
{
    SR_Vtx a, b, c, d;

    MkVtx(&a, 10, 10, col);
    MkVtx(&b, 30, 10, col);
    MkVtx(&c, 30, 30, col);
    MkVtx(&d, 10, 30, col);

    SR_st.texOn     = 0;
    SR_st.alphaTest = 0;
    SR_st.blendOn   = blendOn;
    SR_st.srcBlend  = srcB;
    SR_st.dstBlend  = dstB;
    SR_boundTex     = NULL;

    SR_Queue(&a, &b, &c, &SR_st, NULL);
    SR_Queue(&a, &c, &d, &SR_st, NULL);

    /* ---- the whole point: state changes AFTER queueing ---- */
    SR_st.blendOn  = clobberBlendTo;
    SR_st.srcBlend = 1;
    SR_st.dstBlend = 1;

    SR_Flush();
}

int main(void)
{
    SR_Vtx a, b, c, d;

    SR_InitState();
    SR_Reset(W, H);

    printf("=== frame-batched blend regression (canvas %dx%d) ===\n\n", W, H);

    /* ---------- B1: 5/6 with sa==0 over white -> must stay white ---------- */
    {
        unsigned int got;
        SR_Clear(0xFFFFFFFFu);                     /* white background */
        QueuedQuad(0x00000000u, 1, 5, 6, 0);       /* transparent black, blend ON at queue time */
        got = px(20, 20);
        printf("B1 5/6  sa=0 over white      got 0x%08X  (expect 0xFFFFFFFF)\n", got);
        if ((got & 0x00FFFFFFu) != 0x00FFFFFFu) { printf("     *** FAIL: black box ***\n"); g_fail++; }
    }

    /* ---------- B2: 5/2 additive with sa==0 -> must leave dst alone ---------- */
    {
        unsigned int got;
        SR_Clear(0xFFFFFFFFu);
        QueuedQuad(0x00000000u, 1, 5, 2, 0);       /* this is the combo that made boxes */
        got = px(20, 20);
        printf("B2 5/2  sa=0 over white      got 0x%08X  (expect 0xFFFFFFFF)\n", got);
        if ((got & 0x00FFFFFFu) != 0x00FFFFFFu) { printf("     *** FAIL: black box ***\n"); g_fail++; }
    }

    /* ---------- B3: 5/6 half-over-black -> must be half-blended ---------- */
    {
        unsigned int got;
        int r, g, bl;
        SR_Clear(0xFF000000u);                     /* black background */
        QueuedQuad(0x80FF0000u, 1, 5, 6, 0);       /* half-alpha red */
        got = px(20, 20);
        r = (got >> 16) & 0xFF; g = (got >> 8) & 0xFF; bl = got & 0xFF;
        printf("B3 5/6  a=128 red over black got 0x%08X  R=%d (expect ~128, replace would be 255)\n",
               got, r);
        if (r > 200) { printf("     *** FAIL: replaced instead of blended ***\n"); g_fail++; }
        if (g != 0 || bl != 0) { printf("     *** FAIL: unexpected G/B\n"); g_fail++; }
    }

    /* ---------- B4: blending genuinely OFF must still replace ---------- */
    {
        unsigned int got;
        SR_Clear(0xFFFFFFFFu);
        QueuedQuad(0x00404040u, 0, 5, 6, 0);       /* blendOn=0 -> raw write */
        got = px(20, 20);
        printf("B4 blendOn=0 raw write       got 0x%08X  (expect 0x00404040)\n", got);
        if ((got & 0x00FFFFFFu) != 0x00404040u) {
            printf("     *** FAIL: no-blend path broken (blend off must always replace) ***\n");
            g_fail++;
        }
    }

    /* ---------- B5: immediate (non-queued) path still works ---------- */
    {
        unsigned int got;
        int r;
        SR_InitState();
        SR_Clear(0xFF000000u);
        SR_st.texOn = 0; SR_st.alphaTest = 0;
        SR_st.blendOn = 1; SR_st.srcBlend = 5; SR_st.dstBlend = 6;
        SR_boundTex = NULL;
        MkVtx(&a, 10, 10, 0x80FF0000u);
        MkVtx(&b, 30, 10, 0x80FF0000u);
        MkVtx(&c, 30, 30, 0x80FF0000u);
        MkVtx(&d, 10, 30, 0x80FF0000u);
        SR_Triangle(&a, &b, &c);
        SR_Triangle(&a, &c, &d);
        got = px(20, 20);
        r = (got >> 16) & 0xFF;
        printf("B5 immediate path a=128      got 0x%08X  R=%d (expect ~128)\n", got, r);
        if (r > 200) { printf("     *** FAIL: immediate path replaced ***\n"); g_fail++; }
    }

    printf("\n=== %s (failures: %d) ===\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail;
}
