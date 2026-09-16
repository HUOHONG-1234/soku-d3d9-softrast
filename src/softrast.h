/*
 * softrast.h -- CPU 软光栅核心（定点 + 行带并行 + SSE2）
 *
 * 设计目标（操作员指定）:
 *   "实现CPU加速 只让GPU做到一个输出的作用"
 *   "效率太低 优化好了上个AVX或者SSE吧"
 *   "先跑到性能过剩再谈消除浪费"
 *
 * 【性能设计】
 *   1. 跨行扫描线: 每行解析求 [xa, xb)，不再逐像素试边函数
 *   2. 定点插值: 颜色与 UV 全用 16.16 整数。
 *      压测实测: 纹理路径 85->129 M px/s (+51%)
 *   3. /255 用移位替代整数除法 (全值域零误差)。
 *      原来每个混合像素 4 次整数除法 = 80-160 周期
 *   4. 【行带并行】帧级批处理 + 6 线程分行带。
 *      关键: 批处理而非每次 Draw 开线程 —— 实测平均每次 Draw 只有 2 个三角形,
 *            同步开销会吃光收益。
 *      正确性: 混合是"同一像素内有序", 而一个像素只属于一个行带,
 *              所以按行带切分不改变混合顺序。
 *
 * 顶点语义: D3D9 XYZRHW (屏幕坐标) —— 无需矩阵变换与透视除法。
 */
#ifndef SOFTRAST_H
#define SOFTRAST_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <emmintrin.h>      /* SSE2 */

/* ---------------- 帧缓冲 ---------------- */
static unsigned int *SR_buf = NULL;
static int           SR_w = 0, SR_h = 0;

/* 【外部托管】帧延迟环接管帧缓冲时必须置 1。
   否则 SR_Reset 会把环缓冲 free 掉又重新 malloc，延迟就断了。
   必须声明在 SR_Reset 之前。 */
static int SR_bufExternal = 0;

/* SSE2 填充 n 个像素 */
static void SR_Fill32(unsigned int *dst, unsigned int col, int n)
{
    __m128i v = _mm_set1_epi32((int)col);
    int i = 0;
    for (; i + 4 <= n; i += 4)
        _mm_storeu_si128((__m128i *)(dst + i), v);
    for (; i < n; i++) dst[i] = col;
}

static int SR_Reset(int w, int h)
{
    if (w <= 0 || h <= 0) return 0;
    if (SR_buf && SR_w == w && SR_h == h) return 1;
    /* 【外部托管】帧延迟环形缓冲接管时必须跳过 ——
       否则这里会把环缓冲 free 掉、又重新 malloc 一张, 延迟就断了。 */
    if (SR_bufExternal) return 1;
    if (SR_buf) { free(SR_buf); SR_buf = NULL; }
    SR_buf = (unsigned int *)malloc((size_t)w * h * 4);
    if (!SR_buf) return 0;
    SR_w = w; SR_h = h;
    memset(SR_buf, 0, (size_t)w * h * 4);
    return 1;
}

/* 把帧缓冲指向外部内存（帧延迟环用）。
   调用方负责这块内存的生死。 */
static void SR_SetBuffer(unsigned int *buf, int w, int h)
{
    SR_buf = buf; SR_w = w; SR_h = h;
    SR_bufExternal = 1;
}
static void SR_ClearExternal(void) { SR_bufExternal = 0; }

static void SR_Clear(unsigned int argb)
{
    if (!SR_buf) return;
    SR_Fill32(SR_buf, argb, SR_w * SR_h);
}

/* 只清一个矩形 —— D3D9 的 Clear 支持 Count>0 + pRects，
   只清指定区域。游戏常靠它做局部重绘。
   【之前忽略 pRects 直接整屏清 -> 把早先画好的东西全抹掉，
     实测症状: 对手变空气、只剩片段。】 */
static void SR_ClearRect(int x1, int y1, int x2, int y2, unsigned int argb)
{
    int y;
    if (!SR_buf) return;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > SR_w) x2 = SR_w;
    if (y2 > SR_h) y2 = SR_h;
    if (x1 >= x2 || y1 >= y2) return;
    for (y = y1; y < y2; y++)
        SR_Fill32(SR_buf + (size_t)y * SR_w + x1, argb, x2 - x1);
}

/* ---------------- 纹理 ---------------- */
typedef struct {
    int            w, h;
    unsigned int  *px;      /* 0xAARRGGBB */
    int            valid;
} SR_Tex;

/* 【纹理生命周期】
   帧级批处理下, 命令入队到 Flush 之间隔着一段时间。
   若纹理在此期间被 Release 并回收 (px 释放, valid=0), Flush 时就会全退回顶点色。
   实测症状: 纹理未就绪命中 1245 万次, 正常采样 0 次 -> 贴图全白, 立绘蒸发。
   **保证留存的责任在调用方** (shim 侧用 TexHold/TexReleaseHeld 做帧级保活)。
   本文件不做引用计数 —— 在队列里逐个 AddRef 会造成释放路径不对称。 */

/* ---------------- 渲染状态 ---------------- */
typedef struct {
    int   texOn;
    int   srcBlend, dstBlend;
    int   alphaTest;
    unsigned int alphaRef;
    int   colorOp;
    int   zwrite;
    int   blendOn;
} SR_State;

static SR_State SR_st;

static void SR_InitState(void)
{
    memset(&SR_st, 0, sizeof(SR_st));
    SR_st.colorOp = 0;
    SR_st.srcBlend = 5;
    SR_st.dstBlend = 6;
    SR_st.zwrite   = 1;
    SR_st.blendOn  = 0;
}

/* ---------------- 顶点(已解码) ---------------- */
typedef struct {
    float x, y;
    float u, v;
    unsigned int col;       /* 0xAARRGGBB */
    int   hasUV;
} SR_Vtx;

/* 除以 255 的精确快速替代（全值域 0..65535 零误差）。 */
static inline unsigned int SR_Div255(unsigned int x)
{
    unsigned int y = x + 1u;
    return (y + (y >> 8)) >> 8;
}

/* ---------------- 混合 ---------------- */
static inline unsigned int SR_BlendPixel(unsigned int src, unsigned int dst)
{
    unsigned int sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
    unsigned int sa = (src >> 24) & 0xFF;
    unsigned int dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    unsigned int da = (dst >> 24) & 0xFF;
    unsigned int r, g, b, a;

    if (!SR_st.blendOn) return src;
    if (SR_st.srcBlend == 5 && SR_st.dstBlend == 6) {
        if (sa >= 255) return src;
        if (sa == 0)   return dst;
        r = SR_Div255(sr * sa + dr * (255 - sa));
        g = SR_Div255(sg * sa + dg * (255 - sa));
        b = SR_Div255(sb * sa + db * (255 - sa));
        a = sa + SR_Div255(da * (255 - sa));
        if (a > 255) a = 255;
        return (a << 24) | (r << 16) | (g << 8) | b;
    }
    if (SR_st.srcBlend == 2 && SR_st.dstBlend == 1) return src;
    if (SR_st.srcBlend == 1 && SR_st.dstBlend == 1) return dst;
    {
        int sf, df;
        switch (SR_st.srcBlend) {
        case 1: sf = 0; break;
        case 2: sf = 255; break;
        case 3: sf = (int)((sr+sg+sb)/3); break;
        case 4: sf = 255 - (int)((sr+sg+sb)/3); break;
        case 5: sf = (int)sa; break;
        case 6: sf = 255 - (int)sa; break;
        case 7: sf = (int)da; break;
        case 8: sf = 255 - (int)da; break;
        default: sf = 0; break;
        }
        switch (SR_st.dstBlend) {
        case 1: df = 0; break;
        case 2: df = 255; break;
        case 3: df = (int)((dr+dg+db)/3); break;
        case 4: df = 255 - (int)((dr+dg+db)/3); break;
        case 5: df = (int)sa; break;
        case 6: df = 255 - (int)sa; break;
        case 7: df = (int)da; break;
        case 8: df = 255 - (int)da; break;
        default: df = 255; break;
        }
        r = SR_Div255(sr * sf + dr * df);
        g = SR_Div255(sg * sf + dg * df);
        b = SR_Div255(sb * sf + db * df);
        a = 255;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        return (a << 24) | (r << 16) | (g << 8) | b;
    }
}

/* ---------------- 计数器 (long long: 采样数会超 2^31) ---------------- */
static long long g_whiteSample = 0;
static long long g_validSample = 0;
static long long g_triIn = 0;
static long long g_triDegenerate = 0;
static long long g_triOffscreen = 0;
static long long g_spans = 0;
static long long g_spanPix = 0;
static long long g_texNoBlend = 0;   /* 有纹理但混合关闭的像素数 */
static long long g_texNoAlpha = 0;   /* 纹理 alpha 全不透明的像素数 */

/* 每线程统计（并行时避免争用） */
typedef struct {
    long long spans, spanPix, white, valid;
    long long texNoBlend;   /* 【诊断】有纹理但 ALPHABLENDENABLE=FALSE
                               这一项会让精灵的透明区变成黑边、
                               白字透明底变成纯白块 —— 与操作员症状完全对应 */
    long long texNoAlpha;   /* 【诊断】纹理 alpha 全不透明 (素材本身没抠干净) */
} SR_Stats;

#define SR_NO_TEXTURE  0x00000000u

/* ---------------- 帧缓冲导出 (诊断) ---------------- */
static int SR_DumpBMP(const char *path)
{
    FILE *f;
    int y, rowsz, imgsz, filesz, tmp;
    unsigned short bf[2], p, bpp;
    if (!SR_buf || SR_w <= 0 || SR_h <= 0) return 0;
    f = fopen(path, "wb");
    if (!f) return 0;
    rowsz  = SR_w * 4;
    imgsz  = rowsz * SR_h;
    filesz = 54 + imgsz;
    bf[0] = 0x4D42; fwrite(bf, 2, 1, f);
    tmp = filesz;   fwrite(&tmp, 4, 1, f);
    tmp = 0;        fwrite(&tmp, 2, 1, f);
    fwrite(&tmp, 2, 1, f);
    tmp = 54;       fwrite(&tmp, 4, 1, f);
    tmp = 40;       fwrite(&tmp, 4, 1, f);
    tmp = SR_w;     fwrite(&tmp, 4, 1, f);
    tmp = SR_h;     fwrite(&tmp, 4, 1, f);
    p = 1; bpp = 32; fwrite(&p, 2, 1, f); fwrite(&bpp, 2, 1, f);
    tmp = 0;        fwrite(&tmp, 4, 1, f);
    tmp = imgsz;    fwrite(&tmp, 4, 1, f);
    tmp = 0;        fwrite(&tmp, 4, 1, f);
    tmp = 0;        fwrite(&tmp, 4, 1, f);
    tmp = 0;        fwrite(&tmp, 4, 1, f);
    tmp = 0;        fwrite(&tmp, 4, 1, f);
    for (y = SR_h - 1; y >= 0; y--)
        fwrite(SR_buf + (size_t)y * SR_w, 1, rowsz, f);
    fclose(f);
    return 1;
}

/* ---------------- 三角形预处理结果 ---------------- */
typedef struct {
    /* 几何 */
    float ax, ay, bx, by, cx, cy;
    float area, invArea;
    float dw0dx, dw1dx, dw2dx;
    int   x0, y0, x1, y1;
    /* 颜色插值 */
    int   uniform;
    unsigned int ucol;
    float r0, g0, b0, a0;
    float r1, g1, b1, a1;
    float r2, g2, b2, a2;
    float drdx, dgdx, dbdx, dadx;   /* 行首用 (float) */
    int   dcr, dcg, dcb, dca;       /* 行内步进 (16.16 定点) */
    /* 纹理 */
    int   useTex;
    const unsigned int *texPx;
    int   texW, texH;
    float u0, v0, u1, v1, u2, v2;
    float dudx, dvdx;
    int   dui, dvi;                 /* texel 空间 16.16 步进 */
    /* 状态快照 */
    int stColorOp, stAlphaTest, stBlendOn, stSrcB, stDstB;
    unsigned stAlphaRef;
    int texWanted;                  /* 状态要求纹理但纹理不可用 -> 诊断用 */
} SR_Tri;

/* 预处理。返回 0 = 退化/全屏外, 无需光栅化。 */
static int SR_TriSetup(SR_Tri *t, const SR_Vtx *a, const SR_Vtx *b, const SR_Vtx *c,
                       const SR_State *st, const SR_Tex *tex)
{
    float minx, miny, maxx, maxy;
    float l0x, l1x, l2x;

    minx = a->x; if (b->x < minx) minx = b->x; if (c->x < minx) minx = c->x;
    maxx = a->x; if (b->x > maxx) maxx = b->x; if (c->x > maxx) maxx = c->x;
    miny = a->y; if (b->y < miny) miny = b->y; if (c->y < miny) miny = c->y;
    maxy = a->y; if (b->y > maxy) maxy = b->y; if (c->y > maxy) maxy = c->y;

    t->x0 = (int)ceilf(minx - 0.5f);       if (t->x0 < 0) t->x0 = 0;
    t->y0 = (int)ceilf(miny - 0.5f);       if (t->y0 < 0) t->y0 = 0;
    t->x1 = (int)floorf(maxx - 0.5f) + 1;  if (t->x1 > SR_w) t->x1 = SR_w;
    t->y1 = (int)floorf(maxy - 0.5f) + 1;  if (t->y1 > SR_h) t->y1 = SR_h;
    if (t->x0 >= t->x1 || t->y0 >= t->y1) { g_triOffscreen++; return 0; }

    t->ax = a->x; t->ay = a->y; t->bx = b->x; t->by = b->y; t->cx = c->x; t->cy = c->y;
    t->area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (t->area > -1e-6f && t->area < 1e-6f) { g_triDegenerate++; return 0; }
    t->invArea = 1.0f / t->area;

    t->dw0dx = b->y - c->y;
    t->dw1dx = c->y - a->y;
    t->dw2dx = a->y - b->y;

    /* 状态快照 */
    t->stColorOp   = st->colorOp;
    t->stAlphaTest = st->alphaTest;
    t->stAlphaRef  = st->alphaRef;
    t->stBlendOn   = st->blendOn;
    t->stSrcB      = st->srcBlend;
    t->stDstB      = st->dstBlend;

    t->uniform = (a->col == b->col && b->col == c->col);
    t->ucol = a->col;

    t->useTex = (st->texOn && tex && tex->valid && tex->px) ? 1 : 0;
    t->texWanted = (st->texOn && !t->useTex) ? 1 : 0;
    t->texPx  = t->useTex ? tex->px : NULL;
    t->texW   = t->useTex ? tex->w : 0;
    t->texH   = t->useTex ? tex->h : 0;

    l0x = t->dw0dx * t->invArea;
    l1x = t->dw1dx * t->invArea;
    l2x = t->dw2dx * t->invArea;

    t->r0 = (float)((a->col >> 16) & 0xFF); t->r1 = (float)((b->col >> 16) & 0xFF); t->r2 = (float)((c->col >> 16) & 0xFF);
    t->g0 = (float)((a->col >> 8)  & 0xFF); t->g1 = (float)((b->col >> 8)  & 0xFF); t->g2 = (float)((c->col >> 8)  & 0xFF);
    t->b0 = (float)( a->col        & 0xFF); t->b1 = (float)( b->col        & 0xFF); t->b2 = (float)( c->col        & 0xFF);
    t->a0 = (float)((a->col >> 24) & 0xFF); t->a1 = (float)((b->col >> 24) & 0xFF); t->a2 = (float)((c->col >> 24) & 0xFF);

    if (!t->uniform) {
        t->drdx = t->r0 * l0x + t->r1 * l1x + t->r2 * l2x;
        t->dgdx = t->g0 * l0x + t->g1 * l1x + t->g2 * l2x;
        t->dbdx = t->b0 * l0x + t->b1 * l1x + t->b2 * l2x;
        t->dadx = t->a0 * l0x + t->a1 * l1x + t->a2 * l2x;
        t->dcr = (int)(t->drdx * 65536.0f);
        t->dcg = (int)(t->dgdx * 65536.0f);
        t->dcb = (int)(t->dbdx * 65536.0f);
        t->dca = (int)(t->dadx * 65536.0f);
    } else {
        t->drdx = t->dgdx = t->dbdx = t->dadx = 0.0f;
        t->dcr = t->dcg = t->dcb = t->dca = 0;
    }

    t->u0 = a->u; t->v0 = a->v;
    t->u1 = b->u; t->v1 = b->v;
    t->u2 = c->u; t->v2 = c->v;
    if (t->useTex) {
        t->dudx = t->u0 * l0x + t->u1 * l1x + t->u2 * l2x;
        t->dvdx = t->v0 * l0x + t->v1 * l1x + t->v2 * l2x;
        t->dui  = (int)(t->dudx * 65536.0f * (float)t->texW);
        t->dvi  = (int)(t->dvdx * 65536.0f * (float)t->texH);
    } else {
        t->dudx = t->dvdx = 0.0f;
        t->dui = t->dvi = 0;
    }
    return 1;
}

/* 光栅化 [yFrom, yTo) 行。线程安全: 只写自己的行带。 */
static void SR_TriRows(const SR_Tri *t, int yFrom, int yTo, SR_Stats *stats)
{
    int y;
    int iy0 = t->y0 > yFrom ? t->y0 : yFrom;
    int iy1 = t->y1 < yTo   ? t->y1 : yTo;
    float negA = (t->area < 0) ? -1.0f : 1.0f;
    int   negI = (t->area < 0);

    if (iy0 >= iy1) return;

    for (y = iy0; y < iy1; y++) {
        float py = (float)y + 0.5f;
        float pxc = (float)t->x0 + 0.5f;
        float w0, w1, w2, xa, xb, l0, l1, l2;
        int ia, ib, n;

        w0 = (t->bx - pxc) * (t->cy - py) - (t->by - py) * (t->cx - pxc);
        w1 = (t->cx - pxc) * (t->ay - py) - (t->cy - py) * (t->ax - pxc);
        w2 = (t->ax - pxc) * (t->by - py) - (t->ay - py) * (t->bx - pxc);

        /* 解析求 [xa, xb)，绕序归一化 */
        xa = (float)t->x0; xb = (float)t->x1;
        {
            float w_[3], dw_[3];
            int k;
            w_[0] = w0; w_[1] = w1; w_[2] = w2;
            dw_[0] = t->dw0dx; dw_[1] = t->dw1dx; dw_[2] = t->dw2dx;
            for (k = 0; k < 3; k++) {
                float wk  = negI ? -w_[k]  : w_[k];
                float dwk = negI ? -dw_[k] : dw_[k];
                if (dwk > 1e-9f) {
                    float xc = pxc - wk / dwk;
                    if (xc > xa) xa = xc;
                } else if (dwk < -1e-9f) {
                    float xc = pxc - wk / dwk;
                    if (xc < xb) xb = xc;
                } else {
                    if (wk < 0) { xa = (float)t->x1; xb = (float)t->x0; }
                }
            }
        }
        if (!(xb > xa)) continue;

        ia = (int)ceilf(xa - 0.5f);       if (ia < t->x0) ia = t->x0;
        ib = (int)floorf(xb - 0.5f) + 1;  if (ib > t->x1) ib = t->x1;
        if (ib <= ia) continue;
        n = ib - ia;
        stats->spans++;
        stats->spanPix += n;

        {
            float px = (float)ia + 0.5f;
            float dx = px - pxc;
            unsigned int *dst;
            int i;
            int ci_r = 0, ci_g = 0, ci_b = 0, ci_a = 0;
            int ui = 0, vi = 0;

            w0 += t->dw0dx * dx; w1 += t->dw1dx * dx; w2 += t->dw2dx * dx;
            l0 = w0 * t->invArea; l1 = w1 * t->invArea; l2 = w2 * t->invArea;

            if (!t->uniform) {
                float cr = t->r0 * l0 + t->r1 * l1 + t->r2 * l2;
                float cg = t->g0 * l0 + t->g1 * l1 + t->g2 * l2;
                float cb = t->b0 * l0 + t->b1 * l1 + t->b2 * l2;
                float ca = t->a0 * l0 + t->a1 * l1 + t->a2 * l2;
                ci_r = (int)(cr * 65536.0f);
                ci_g = (int)(cg * 65536.0f);
                ci_b = (int)(cb * 65536.0f);
                ci_a = (int)(ca * 65536.0f);
            }
            if (t->useTex) {
                float uu = t->u0 * l0 + t->u1 * l1 + t->u2 * l2;
                float vv = t->v0 * l0 + t->v1 * l1 + t->v2 * l2;
                ui = (int)(uu * 65536.0f * (float)t->texW);
                vi = (int)(vv * 65536.0f * (float)t->texH);
            }

            /* 快路径: 纯色 + 无纹理 + 无 alpha test + 不透明 -> SSE2 整段写 */
            if (t->uniform && !t->useTex && !t->stAlphaTest &&
                ((t->ucol >> 24) & 0xFF) == 0xFF) {
                SR_Fill32(SR_buf + (size_t)y * SR_w + ia, t->ucol, n);
                continue;
            }

            /* 统计: 按行累加而非逐像素 (省掉内循环里的开销)
               注意: 重写成定点+内联采样后, valid/white 一度只增到 0,
               因为计数留在旧路径里 —— 这就是“正常采样 0”的真相。 */
            if (t->useTex)          stats->valid += n;
            else if (t->texWanted)  stats->white += n;
            if (t->useTex && !t->stBlendOn) stats->texNoBlend += n;

            dst = SR_buf + (size_t)y * SR_w + ia;
            for (i = 0; i < n; i++) {
                unsigned int col;
                if (t->uniform) {
                    col = t->ucol;
                } else {
                    int r = ci_r >> 16, g = ci_g >> 16, b = ci_b >> 16, al = ci_a >> 16;
                    if (r < 0) r = 0; else if (r > 255) r = 255;
                    if (g < 0) g = 0; else if (g > 255) g = 255;
                    if (b < 0) b = 0; else if (b > 255) b = 255;
                    if (al < 0) al = 0; else if (al > 255) al = 255;
                    col = ((unsigned)al << 24) | ((unsigned)r << 16)
                        | ((unsigned)g << 8) | (unsigned)b;
                }

                if (t->useTex) {
                    int tx = ui >> 16, ty = vi >> 16;
                    unsigned int tc;
                    if (tx < 0) tx = 0; else if (tx >= t->texW) tx = t->texW - 1;
                    if (ty < 0) ty = 0; else if (ty >= t->texH) ty = t->texH - 1;
                    tc = t->texPx[(size_t)ty * t->texW + tx];
                    {
                        unsigned int tr = (tc >> 16) & 0xFF, tg = (tc >> 8) & 0xFF;
                        unsigned int tb = tc & 0xFF, ta = (tc >> 24) & 0xFF;
                        unsigned int cr = (col >> 16) & 0xFF, cg = (col >> 8) & 0xFF;
                        unsigned int cb = col & 0xFF, ca = (col >> 24) & 0xFF;
                        unsigned int r, g, b, al;
                        if (t->stColorOp == 1) { r = tr; g = tg; b = tb; al = ta; }
                        else if (t->stColorOp == 2) {
                            r = tr + cr; g = tg + cg; b = tb + cb; al = ta;
                            if (r > 255) r = 255;
                            if (g > 255) g = 255;
                            if (b > 255) b = 255;
                        } else {
                            r = SR_Div255(tr * cr);
                            g = SR_Div255(tg * cg);
                            b = SR_Div255(tb * cb);
                            al = SR_Div255(ta * ca);
                        }
                        col = (al << 24) | (r << 16) | (g << 8) | b;
                    }
                } else {
                    /* texWanted 已在行首按 n 计入 stats->white */
                }

                if (t->stAlphaTest && ((col >> 24) & 0xFF) < t->stAlphaRef) {
                    /* 丢弃 */
                } else if (!t->stBlendOn) {
                    *dst = col;
                } else if (t->stSrcB == 5 && t->stDstB == 6) {
                    unsigned int sa = (col >> 24) & 0xFF;
                    if (sa >= 255) {
                        *dst = col;
                    } else if (sa != 0) {
                        unsigned int d = *dst;
                        unsigned int sr = (col >> 16) & 0xFF, sg2 = (col >> 8) & 0xFF, sb2 = col & 0xFF;
                        unsigned int dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF, da = (d >> 24) & 0xFF;
                        unsigned int r = SR_Div255(sr * sa + dr * (255 - sa));
                        unsigned int g = SR_Div255(sg2 * sa + dg * (255 - sa));
                        unsigned int b = SR_Div255(sb2 * sa + db * (255 - sa));
                        unsigned int a = sa + SR_Div255(da * (255 - sa));
                        if (a > 255) a = 255;
                        *dst = (a << 24) | (r << 16) | (g << 8) | b;
                    }
                } else {
                    *dst = SR_BlendPixel(col, *dst);
                }

                if (t->useTex) { ui += t->dui; vi += t->dvi; }
                if (!t->uniform) { ci_r += t->dcr; ci_g += t->dcg; ci_b += t->dcb; ci_a += t->dca; }
                dst++;
            }
        }
    }
}

/* ---------------- 立即模式（单元测试 / 基准用） ---------------- */
static const SR_Tex *SR_boundTex = NULL;

static void SR_Triangle(const SR_Vtx *a, const SR_Vtx *b, const SR_Vtx *c)
{
    SR_Tri t;
    SR_Stats st;
    if (!SR_buf) return;
    g_triIn++;
    memset(&st, 0, sizeof(st));
    if (!SR_TriSetup(&t, a, b, c, &SR_st, SR_boundTex)) return;
    SR_TriRows(&t, 0, SR_h, &st);
    g_spans += st.spans; g_spanPix += st.spanPix;
    g_whiteSample += st.white; g_validSample += st.valid;
}

/* ---------------- 帧级批处理 + 行带并行 ---------------- */

#define SR_MAX_CMD      16384
#define SR_MAX_THREADS  8

typedef struct {
    SR_Vtx   a, b, c;
    SR_State st;
    const SR_Tex *tex;
} SR_Cmd;

static SR_Cmd   SR_cmds[SR_MAX_CMD];
static int      SR_cmdCount = 0;
static SR_Tri   SR_tris[SR_MAX_CMD];

static int      SR_nThreads = 0;        /* 实际工作线程数（不含主线程） */
static int      SR_nBands   = 0;
static int      SR_bandH    = 0;
/* 【同步设计 —— 踩过坑, 别改回手动重置】
   错误做法: 共用一个手动重置事件。事件在整个帧内保持 signaled,
             工作线程做完自己的趟数后会立即再次通过 Wait, 空转并对 busy 重复递减
             -> busy 提前归零 -> 主线程提前放行 -> 数据竞态甚至挂死。
   正确做法: 每线程一个【自动重置】事件, 一帧一次唤醒, 做完就回去等。 */
static HANDLE   SR_hWake[SR_MAX_THREADS];
static HANDLE   SR_hThread[SR_MAX_THREADS];
static HANDLE   SR_hDone    = NULL;
static volatile LONG SR_bandIdx = 0;
static volatile LONG SR_busy    = 0;
static volatile LONG SR_quit    = 0;
static SR_Stats SR_tstat[SR_MAX_THREADS + 1];

static int SR_TotalThreads(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

static DWORD WINAPI SR_Worker(LPVOID param)
{
    int tid = (int)(INT_PTR)param;
    for (;;) {
        WaitForSingleObject(SR_hWake[tid - 1], INFINITE);
        if (SR_quit) return 0;

        /* 动态取行带 —— 负载均衡 (2D 场景三角形大小差异大) */
        for (;;) {
            int b = (int)InterlockedIncrement(&SR_bandIdx) - 1;
            int i;
            if (b >= SR_nBands) break;
            {
                int yFrom = b * SR_bandH;
                int yTo   = yFrom + SR_bandH;
                if (yTo > SR_h) yTo = SR_h;
                for (i = 0; i < SR_cmdCount; i++)
                    SR_TriRows(&SR_tris[i], yFrom, yTo, &SR_tstat[tid]);
            }
        }
        if (InterlockedDecrement(&SR_busy) == 0)
            SetEvent(SR_hDone);
    }
}

static void SR_SetThreads(int n)
{
    int i;
    if (n < 1) n = 1;
    if (n > SR_MAX_THREADS + 1) n = SR_MAX_THREADS + 1;
    if (SR_nThreads + 1 == n && SR_hDone) return;   /* 已就绪 */

    /* 先停掉旧的 */
    if (SR_nThreads > 0) {
        InterlockedExchange(&SR_quit, 1);
        InterlockedExchange(&SR_busy, SR_nThreads);
        for (i = 0; i < SR_nThreads; i++) {
            if (SR_hWake[i]) SetEvent(SR_hWake[i]);
            if (SR_hThread[i]) {
                WaitForSingleObject(SR_hThread[i], 3000);
                CloseHandle(SR_hThread[i]);
                SR_hThread[i] = NULL;
            }
            if (SR_hWake[i]) { CloseHandle(SR_hWake[i]); SR_hWake[i] = NULL; }
        }
    }
    SR_nThreads = 0;
    InterlockedExchange(&SR_quit, 0);

    if (!SR_hDone) {
        SR_hDone = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (!SR_hDone) return;
    }
    if (n <= 1) return;   /* 单线程: 不需要工作线程 */

    for (i = 0; i < n - 1 && i < SR_MAX_THREADS; i++) {
        SR_hWake[i] = CreateEvent(NULL, FALSE, FALSE, NULL);   /* 自动重置: 一帧一次 */
        if (!SR_hWake[i]) break;
        SR_hThread[i] = CreateThread(NULL, 0, SR_Worker, (LPVOID)(INT_PTR)(i + 1), 0, NULL);
        if (!SR_hThread[i]) { CloseHandle(SR_hWake[i]); SR_hWake[i] = NULL; break; }
        SR_nThreads++;
    }
}

static void SR_BeginFrame(void)
{
    SR_cmdCount = 0;
}

static int SR_Queue(const SR_Vtx *a, const SR_Vtx *b, const SR_Vtx *c,
                    const SR_State *st, const SR_Tex *tex)
{
    SR_Cmd *cmd;
    if (SR_cmdCount >= SR_MAX_CMD) return 0;
    cmd = &SR_cmds[SR_cmdCount++];
    cmd->a = *a; cmd->b = *b; cmd->c = *c;
    cmd->st = *st;
    cmd->tex = tex;
    return 1;
}

/* 分发并等待完成 */
static void SR_Flush(void)
{
    int i, nw;
    if (!SR_buf || SR_cmdCount == 0) return;

    /* 预处理一次, 供所有线程只读共享 */
    {
        int n = 0;
        for (i = 0; i < SR_cmdCount; i++) {
            g_triIn++;
            if (SR_TriSetup(&SR_tris[n], &SR_cmds[i].a, &SR_cmds[i].b, &SR_cmds[i].c,
                            &SR_cmds[i].st, SR_cmds[i].tex))
                n++;
            /* 退化/屏外的丢弃: 不推进 n, 下一条覆盖同一位置 */
        }
        SR_cmdCount = n;
    }
    if (SR_cmdCount == 0) return;

    nw = SR_nThreads + 1;
    if (nw > 1 && SR_hDone) {
        /* 行带数 = 线程数 * 4, 让动态调度能均衡 */
        SR_nBands = nw * 4;
        SR_bandH  = (SR_h + SR_nBands - 1) / SR_nBands;
        if (SR_bandH < 4) SR_bandH = 4;
        SR_nBands = (SR_h + SR_bandH - 1) / SR_bandH;

        for (i = 0; i <= SR_nThreads && i <= SR_MAX_THREADS; i++)
            memset(&SR_tstat[i], 0, sizeof(SR_Stats));
        InterlockedExchange(&SR_bandIdx, 0);
        InterlockedExchange(&SR_busy, SR_nThreads);
        ResetEvent(SR_hDone);
        for (i = 0; i < SR_nThreads; i++)
            if (SR_hWake[i]) SetEvent(SR_hWake[i]);   /* 每线程一个自动重置事件 */

        /* 主线程也干活: 一起抢行带 */
        {
            int b;
            for (;;) {
                b = (int)InterlockedIncrement(&SR_bandIdx) - 1;
                if (b >= SR_nBands) break;
                {
                    int yFrom = b * SR_bandH;
                    int yTo = yFrom + SR_bandH;
                    if (yTo > SR_h) yTo = SR_h;
                    for (i = 0; i < SR_cmdCount; i++)
                        SR_TriRows(&SR_tris[i], yFrom, yTo, &SR_tstat[0]);
                }
            }
        }
        WaitForSingleObject(SR_hDone, INFINITE);
    } else {
        for (i = 0; i < SR_cmdCount; i++)
            SR_TriRows(&SR_tris[i], 0, SR_h, &SR_tstat[0]);
    }

    /* 合并统计 */
    {
        int k;
        for (k = 0; k <= SR_nThreads && k <= SR_MAX_THREADS; k++) {
            g_spans  += SR_tstat[k].spans;
            g_spanPix += SR_tstat[k].spanPix;
            g_whiteSample += SR_tstat[k].white;
            g_validSample += SR_tstat[k].valid;
            g_texNoBlend  += SR_tstat[k].texNoBlend;
            g_texNoAlpha  += SR_tstat[k].texNoAlpha;
        }
    }
    /* 纹理的释放由调用方的帧级持有负责 (TexReleaseHeld) */
    SR_cmdCount = 0;
}

static void SR_ShutdownThreads(void)
{
    int i;
    if (SR_nThreads > 0) {
        InterlockedExchange(&SR_quit, 1);
        InterlockedExchange(&SR_busy, SR_nThreads);
        for (i = 0; i < SR_nThreads; i++) {
            if (SR_hWake[i]) SetEvent(SR_hWake[i]);
            if (SR_hThread[i]) {
                WaitForSingleObject(SR_hThread[i], 3000);
                CloseHandle(SR_hThread[i]);
                SR_hThread[i] = NULL;
            }
            if (SR_hWake[i]) { CloseHandle(SR_hWake[i]); SR_hWake[i] = NULL; }
        }
    }
    SR_nThreads = 0;
    if (SR_hDone) { CloseHandle(SR_hDone); SR_hDone = NULL; }
}

#endif /* SOFTRAST_H */
