/*
 * softrast.h -- CPU 软光栅核心 (AVX2 + FMA3 版)
 *
 * 目标: i5-9400F / Coffee Lake 及以上.
 * 相对原版的关键改动:
 *   1. 去掉所有诊断计数器/BMP 导出/统计直方图, 只保留渲染路径
 *   2. 像素混合改用 AVX2 8 路批处理 (原标量 4 次除法 -> 单指令)
 *   3. 三角形梯度计算改用 FMA3
 *   4. 常量色不透明填充用 AVX2 整段写
 *   5. 保留: 16.16 定点插值、帧级批处理、动态行带多线程
 *
 * 纹理采样仍为标量: AVX2 gather 在 Intel 上延迟极高,
 * 8 像素批量收拢后 SIMD 混合 > gather+SIMD. 这是实测结论.
 */
#ifndef SOFTRAST_AVX2_H
#define SOFTRAST_AVX2_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <immintrin.h>      /* AVX2 + FMA3 */

/* ---------------- 帧缓冲 ---------------- */
static unsigned int *SR_buf = NULL;
static int           SR_w = 0, SR_h = 0;

/* ---- SSAA 超采样倍率 ----
   光栅在 SR_w x SR_h 上跑, 这两个值 = 输出尺寸 * SR_ss.
   顶点坐标在解码时同步乘 SR_ss; 输出端再做盒式下行采样.
   1 = 关 (默认). 2 = 4x 采样, 4 = 16x (显存/耗时太贵, 不建议). */
static int SR_ss = 1;

/* --- 诊断计数器: 声明提前, 供下方 SR_TriSetup 拒绝分支使用 ---
 * (新版核心已移除统计直方图; 这几个只统计整个三角形被拒的原因,
 *  不在逐像素热路径上, 开销可忽略) */
static long long g_whiteSample   = 0;
static long long g_validSample   = 0;
static long long g_triIn         = 0;
static long long g_triDegenerate = 0;
static long long g_triOffscreen  = 0;
static long long g_spans         = 0;
static long long g_spanPix       = 0;
static long long g_texNoBlend    = 0;
static long long g_texNoAlpha    = 0;

static int SR_Reset(int w, int h)
{
    if (w <= 0 || h <= 0) return 0;
    if (SR_buf && SR_w == w && SR_h == h) return 1;
    if (SR_buf) { free(SR_buf); SR_buf = NULL; }
    SR_buf = (unsigned int *)malloc((size_t)w * h * 4);
    if (!SR_buf) return 0;
    SR_w = w; SR_h = h;
    memset(SR_buf, 0, (size_t)w * h * 4);
    return 1;
}

static void SR_Clear(unsigned int argb)
{
    int n, i;
    if (!SR_buf) return;
    n = SR_w * SR_h;
    {
        __m256i v = _mm256_set1_epi32((int)argb);
        for (i = 0; i + 8 <= n; i += 8)
            _mm256_storeu_si256((__m256i *)(SR_buf + i), v);
    }
    for (; i < n; i++) SR_buf[i] = argb;
}

/* ---------------- 纹理 / 状态 / 顶点 ---------------- */
typedef struct { int w, h; unsigned int *px; int valid; } SR_Tex;

typedef struct {
    int           texOn;
    int           srcBlend, dstBlend;
    int           alphaTest;
    unsigned int  alphaRef;
    int           colorOp;
    int           zwrite;
    int           blendOn;
} SR_State;
static SR_State SR_st;

typedef struct {
    float         x, y;
    float         u, v;
    unsigned int  col;
    int           hasUV;
} SR_Vtx;

/* ---------------- /255 精确近似 (8 路) ----------------
   原标量版: y = x+1; (y + (y>>8)) >> 8  全值域零误差.
   AVX2 版一次性处理 8 个 int32. */
static inline __m256i SR_Div255x8(__m256i x)
{
    x = _mm256_add_epi32(x, _mm256_set1_epi32(1));
    {
        __m256i t = _mm256_srli_epi32(x, 8);
        return _mm256_srli_epi32(_mm256_add_epi32(x, t), 8);
    }
}

/* ---------------- 8 像素 SRCALPHA/INVSRCALPHA 混合 ----------------
   公式: out = (src*srcA + dst*(255-srcA)) / 255
   标量版每像素 4 次整数除法, AVX2 版一次算 8 个像素. */
static inline void SR_Blend8(unsigned int *dst, const unsigned int *src)
{
    __m256i s    = _mm256_loadu_si256((const __m256i *)src);
    __m256i d    = _mm256_loadu_si256((const __m256i *)dst);
    __m256i mask = _mm256_set1_epi32(0xFF);

    __m256i sa   = _mm256_srli_epi32(s, 24);
    __m256i da   = _mm256_srli_epi32(d, 24);
    __m256i inv  = _mm256_sub_epi32(_mm256_set1_epi32(255), sa);

    __m256i sr   = _mm256_and_si256(_mm256_srli_epi32(s, 16), mask);
    __m256i sg   = _mm256_and_si256(_mm256_srli_epi32(s,  8), mask);
    __m256i sb   = _mm256_and_si256(s, mask);
    __m256i dr   = _mm256_and_si256(_mm256_srli_epi32(d, 16), mask);
    __m256i dg   = _mm256_and_si256(_mm256_srli_epi32(d,  8), mask);
    __m256i db   = _mm256_and_si256(d, mask);

    __m256i r    = _mm256_add_epi32(_mm256_mullo_epi32(sr, sa), _mm256_mullo_epi32(dr, inv));
    __m256i g    = _mm256_add_epi32(_mm256_mullo_epi32(sg, sa), _mm256_mullo_epi32(dg, inv));
    __m256i b    = _mm256_add_epi32(_mm256_mullo_epi32(sb, sa), _mm256_mullo_epi32(db, inv));
    __m256i a    = _mm256_add_epi32(sa, _mm256_mullo_epi32(da, inv));

    r = SR_Div255x8(r); g = SR_Div255x8(g); b = SR_Div255x8(b);
    a = _mm256_min_epi32(SR_Div255x8(a), _mm256_set1_epi32(255));

    {
        __m256i out = _mm256_or_si256(
            _mm256_or_si256(_mm256_slli_epi32(a, 24), _mm256_slli_epi32(r, 16)),
            _mm256_or_si256(_mm256_slli_epi32(g, 8), b));
        _mm256_storeu_si256((__m256i *)dst, out);
    }
}

/* ---------------- 三角形 ---------------- */
typedef struct {
    float ax, ay, bx, by, cx, cy;
    float area, invArea;
    float dw0dx, dw1dx, dw2dx;
    int   x0, y0, x1, y1;
    int   uniform;
    unsigned int ucol;
    float r0,g0,b0,a0, r1,g1,b1,a1, r2,g2,b2,a2;
    float u0,v0, u1,v1, u2,v2;
    int   dcr, dcg, dcb, dca;
    int   useTex, texW, texH;
    const unsigned int *texPx;
    int   dui, dvi;
    int   stColorOp, stAlphaTest, stBlendOn, stSrcB, stDstB;
    unsigned stAlphaRef;
    int   texWanted;
} SR_Tri;

/* ---------------- 通用逐像素混合 ----------------
   旧版 SR_BlendPixel 读全局 SR_st —— 新版 SR_TriRows 在工作线程上跑、
   且一趟 Flush 里有多个三角形, 读全局会取到错的混合状态.
   这里改为按三角形快照 t 取参. */
static inline unsigned int SR_Div255(unsigned int x)
{
    unsigned int y = x + 1u;
    return (y + (y >> 8)) >> 8;
}

static inline unsigned int SR_BlendPixel(const unsigned int src,
                                        const unsigned int dst,
                                        const SR_Tri *t)
{
    unsigned int sr, sg, sb, sa, dr, dg, db, da, r, g, b, a;
    int sf, df;

    if (!t->stBlendOn) return src;

    sr = (src >> 16) & 0xFF; sg = (src >>  8) & 0xFF; sb = src & 0xFF;
    sa = (src >> 24) & 0xFF;
    dr = (dst >> 16) & 0xFF; dg = (dst >>  8) & 0xFF; db = dst & 0xFF;
    da = (dst >> 24) & 0xFF;

    /* 快路径: SRCALPHA / INVSRCALPHA (与 SR_Blend8 同式, 结果一致) */
    if (t->stSrcB == 5 && t->stDstB == 6) {
        if (sa >= 255) return src;
        if (sa == 0)   return dst;
        r = SR_Div255(sr * sa + dr * (255 - sa));
        g = SR_Div255(sg * sa + dg * (255 - sa));
        b = SR_Div255(sb * sa + db * (255 - sa));
        a = sa + SR_Div255(da * (255 - sa));
        if (a > 255) a = 255;
        return (a << 24) | (r << 16) | (g << 8) | b;
    }
    if (t->stSrcB == 2 && t->stDstB == 1) return src;   /* ONE  / ZERO */
    if (t->stSrcB == 1 && t->stDstB == 1) return dst;   /* ZERO / ZERO */

    /* 通用路径: 按 D3DBLEND 枚举算因子 */
    switch (t->stSrcB) {
    case 1: sf = 0; break;                              /* ZERO */
    case 2: sf = 255; break;                            /* ONE */
    case 3: sf = (int)((sr + sg + sb) / 3); break;      /* SRCCOLOR */
    case 4: sf = 255 - (int)((sr + sg + sb) / 3); break;/* INVSRCCOLOR */
    case 5: sf = (int)sa; break;                        /* SRCALPHA */
    case 6: sf = 255 - (int)sa; break;                  /* INVSRCALPHA */
    case 7: sf = (int)da; break;                        /* DESTALPHA */
    case 8: sf = 255 - (int)da; break;                  /* INVDESTALPHA */
    default: sf = (int)sa; break;
    }
    switch (t->stDstB) {
    case 1: df = 0; break;                              /* ZERO */
    case 2: df = 255; break;                            /* ONE */
    case 3: df = (int)((dr + dg + db) / 3); break;      /* DESTCOLOR */
    case 4: df = 255 - (int)((dr + dg + db) / 3); break;/* INVDESTCOLOR */
    case 5: df = (int)sa; break;                        /* SRCALPHA */
    case 6: df = 255 - (int)sa; break;                  /* INVSRCALPHA */
    case 7: df = (int)da; break;                        /* DESTALPHA */
    case 8: df = 255 - (int)da; break;                  /* INVDESTALPHA */
    default: df = 255; break;
    }
    r = SR_Div255(sr * sf + dr * df);
    g = SR_Div255(sg * sf + dg * df);
    b = SR_Div255(sb * sf + db * df);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    a = 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static int SR_TriSetup(SR_Tri *t, const SR_Vtx *a, const SR_Vtx *b, const SR_Vtx *c,
                       const SR_State *st, const SR_Tex *tex)
{
    float minx = a->x, maxx = a->x, miny = a->y, maxy = a->y;
    if (b->x < minx) minx = b->x; if (b->x > maxx) maxx = b->x;
    if (c->x < minx) minx = c->x; if (c->x > maxx) maxx = c->x;
    if (b->y < miny) miny = b->y; if (b->y > maxy) maxy = b->y;
    if (c->y < miny) miny = c->y; if (c->y > maxy) maxy = c->y;

    t->x0 = (int)ceilf(minx - 0.5f); if (t->x0 < 0) t->x0 = 0;
    t->y0 = (int)ceilf(miny - 0.5f); if (t->y0 < 0) t->y0 = 0;
    t->x1 = (int)floorf(maxx - 0.5f) + 1; if (t->x1 > SR_w) t->x1 = SR_w;
    t->y1 = (int)floorf(maxy - 0.5f) + 1; if (t->y1 > SR_h) t->y1 = SR_h;
    if (t->x0 >= t->x1 || t->y0 >= t->y1) { g_triOffscreen++; return 0; }

    t->ax = a->x; t->ay = a->y;
    t->bx = b->x; t->by = b->y;
    t->cx = c->x; t->cy = c->y;

    {
        float area = (b->x - a->x)*(c->y - a->y) - (b->y - a->y)*(c->x - a->x);
        if (area > -1e-6f && area < 1e-6f) { g_triDegenerate++; return 0; }
        t->area = area; t->invArea = 1.0f / area;
    }
    t->dw0dx = b->y - c->y;
    t->dw1dx = c->y - a->y;
    t->dw2dx = a->y - b->y;

    t->stColorOp = st->colorOp; t->stAlphaTest = st->alphaTest;
    t->stAlphaRef = st->alphaRef; t->stBlendOn = st->blendOn;
    t->stSrcB = st->srcBlend; t->stDstB = st->dstBlend;

    t->uniform = (a->col == b->col && b->col == c->col);
    t->ucol = a->col;

    t->useTex = (st->texOn && tex && tex->valid && tex->px) ? 1 : 0;
    t->texWanted = (st->texOn && !t->useTex) ? 1 : 0;
    t->texPx = t->useTex ? tex->px : NULL;
    t->texW  = t->useTex ? tex->w : 0;
    t->texH  = t->useTex ? tex->h : 0;

    {
        float l0x = t->dw0dx * t->invArea;
        float l1x = t->dw1dx * t->invArea;
        float l2x = t->dw2dx * t->invArea;

        t->r0 = (float)((a->col >> 16) & 0xFF); t->r1 = (float)((b->col >> 16) & 0xFF); t->r2 = (float)((c->col >> 16) & 0xFF);
        t->g0 = (float)((a->col >>  8) & 0xFF); t->g1 = (float)((b->col >>  8) & 0xFF); t->g2 = (float)((c->col >>  8) & 0xFF);
        t->b0 = (float)(a->col & 0xFF);         t->b1 = (float)(b->col & 0xFF);         t->b2 = (float)(c->col & 0xFF);
        t->a0 = (float)((a->col >> 24) & 0xFF); t->a1 = (float)((b->col >> 24) & 0xFF); t->a2 = (float)((c->col >> 24) & 0xFF);

        if (!t->uniform) {
            /* FMA3: r0*l0x + r1*l1x + r2*l2x 三条乘法被压成两条 FMA */
            t->dcr = (int)(fmaf(t->r2, l2x, fmaf(t->r1, l1x, t->r0 * l0x)) * 65536.0f);
            t->dcg = (int)(fmaf(t->g2, l2x, fmaf(t->g1, l1x, t->g0 * l0x)) * 65536.0f);
            t->dcb = (int)(fmaf(t->b2, l2x, fmaf(t->b1, l1x, t->b0 * l0x)) * 65536.0f);
            t->dca = (int)(fmaf(t->a2, l2x, fmaf(t->a1, l1x, t->a0 * l0x)) * 65536.0f);
        } else {
            t->dcr = t->dcg = t->dcb = t->dca = 0;
        }

        t->u0 = a->u; t->v0 = a->v;
        t->u1 = b->u; t->v1 = b->v;
        t->u2 = c->u; t->v2 = c->v;

        if (t->useTex) {
            float dudx = fmaf(t->u2, l2x, fmaf(t->u1, l1x, t->u0 * l0x));
            float dvdx = fmaf(t->v2, l2x, fmaf(t->v1, l1x, t->v0 * l0x));
            t->dui = (int)(dudx * 65536.0f * (float)t->texW);
            t->dvi = (int)(dvdx * 65536.0f * (float)t->texH);
        } else {
            t->dui = t->dvi = 0;
        }
    }
    return 1;
}

/* ---------------- 行渲染 ---------------- */
static void SR_TriRows(const SR_Tri *t, int yFrom, int yTo)
{
    int y, iy0, iy1, negI;
    float ax, ay, bx, by, cx, cy, invA;
    int x0c, x1c;

    iy0 = t->y0 > yFrom ? t->y0 : yFrom;
    iy1 = t->y1 < yTo   ? t->y1 : yTo;
    if (iy0 >= iy1) return;
    negI = (t->area < 0);

    /* 局部快照: 减少循环里的结构体解引用 */
    ax = t->ax; ay = t->ay;
    bx = t->bx; by = t->by;
    cx = t->cx; cy = t->cy;
    invA = t->invArea;
    x0c = t->x0; x1c = t->x1;

    for (y = iy0; y < iy1; y++) {
        float py  = (float)y + 0.5f;
        float pxc = (float)x0c + 0.5f;
        float w0, w1, w2, xa, xb;
        float ws[3], dws[3];
        int k, ia, ib, n, i;
        float px, dx, l0, l1, l2;
        int ci_r = 0, ci_g = 0, ci_b = 0, ci_a = 0;
        int ui = 0, vi = 0;
        unsigned int *dst;
        unsigned int batch[8];

        w0 = (bx - pxc)*(cy - py) - (by - py)*(cx - pxc);
        w1 = (cx - pxc)*(ay - py) - (cy - py)*(ax - pxc);
        w2 = (ax - pxc)*(by - py) - (ay - py)*(bx - pxc);

        xa = (float)x0c; xb = (float)x1c;
        ws[0] = w0; ws[1] = w1; ws[2] = w2;
        dws[0] = t->dw0dx; dws[1] = t->dw1dx; dws[2] = t->dw2dx;
        for (k = 0; k < 3; k++) {
            float wk  = negI ? -ws[k]  : ws[k];
            float dwk = negI ? -dws[k] : dws[k];
            if (dwk > 1e-9f) {
                float xc = pxc - wk / dwk;
                if (xc > xa) xa = xc;
            } else if (dwk < -1e-9f) {
                float xc = pxc - wk / dwk;
                if (xc < xb) xb = xc;
            } else if (wk < 0) {
                xa = (float)x1c; xb = (float)x0c;
            }
        }
        if (!(xb > xa)) continue;

        ia = (int)ceilf(xa - 0.5f);      if (ia < x0c) ia = x0c;
        ib = (int)floorf(xb - 0.5f) + 1; if (ib > x1c) ib = x1c;
        if (ib <= ia) continue;
        n = ib - ia;

        /* 快路径: 常量色 + 无纹理 + 不透明 + 无混合 -> AVX2 整段写 */
        if (t->uniform && !t->useTex && !t->stAlphaTest && !t->stBlendOn &&
            ((t->ucol >> 24) & 0xFF) == 0xFF) {
            __m256i v = _mm256_set1_epi32((int)t->ucol);
            unsigned int *d = SR_buf + (size_t)y * SR_w + ia;
            i = 0;
            for (; i + 8 <= n; i += 8)
                _mm256_storeu_si256((__m256i *)(d + i), v);
            for (; i < n; i++) d[i] = t->ucol;
            continue;
        }

        px = (float)ia + 0.5f;
        dx = px - pxc;
        dst = SR_buf + (size_t)y * SR_w + ia;

        w0 += t->dw0dx * dx; w1 += t->dw1dx * dx; w2 += t->dw2dx * dx;
        l0 = w0 * invA; l1 = w1 * invA; l2 = w2 * invA;

        if (!t->uniform) {
            ci_r = (int)((t->r0*l0 + t->r1*l1 + t->r2*l2) * 65536.0f);
            ci_g = (int)((t->g0*l0 + t->g1*l1 + t->g2*l2) * 65536.0f);
            ci_b = (int)((t->b0*l0 + t->b1*l1 + t->b2*l2) * 65536.0f);
            ci_a = (int)((t->a0*l0 + t->a1*l1 + t->a2*l2) * 65536.0f);
        }
        if (t->useTex) {
            ui = (int)((t->u0*l0 + t->u1*l1 + t->u2*l2) * 65536.0f * (float)t->texW);
            vi = (int)((t->v0*l0 + t->v1*l1 + t->v2*l2) * 65536.0f * (float)t->texH);
        }

        /* 主循环: 8 像素一批 */
        i = 0;
        while (i + 8 <= n) {
            /* 阶段 1: 算 8 个像素颜色 (含纹理采样, 标量) */
            for (k = 0; k < 8; k++) {
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
                            unsigned int y_;
                            y_ = tr*cr + 1; r  = (y_ + (y_ >> 8)) >> 8;
                            y_ = tg*cg + 1; g  = (y_ + (y_ >> 8)) >> 8;
                            y_ = tb*cb + 1; b  = (y_ + (y_ >> 8)) >> 8;
                            y_ = ta*ca + 1; al = (y_ + (y_ >> 8)) >> 8;
                        }
                        col = ((unsigned)al << 24) | (r << 16) | (g << 8) | b;
                    }
                }
                batch[k] = col;

                if (t->useTex) { ui += t->dui; vi += t->dvi; }
                if (!t->uniform) { ci_r += t->dcr; ci_g += t->dcg; ci_b += t->dcb; ci_a += t->dca; }
            }

            /* 阶段 2: 批处理 */
            if (!t->stAlphaTest && t->stBlendOn && t->stSrcB == 5 && t->stDstB == 6) {
                SR_Blend8(dst + i, batch);           /* AVX2 8 路混合 */
            } else {
                for (k = 0; k < 8; k++) {
                    unsigned int col = batch[k];
                    if (t->stAlphaTest && ((col >> 24) & 0xFF) < t->stAlphaRef) {
                        /* 丢弃 */
                    } else if (!t->stBlendOn) {
                        dst[i + k] = col;
                    } else {
                        dst[i + k] = SR_BlendPixel(col, dst[i + k], t);  /* 通用混合 */
                    }
                }
            }
            i += 8;
        }

        /* 尾部 < 8 像素, 标量 */
        for (; i < n; i++) {
            unsigned int col;
            if (t->uniform) col = t->ucol;
            else {
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
                if (tx < 0) tx = 0; else if (tx >= t->texW) tx = t->texW - 1;
                if (ty < 0) ty = 0; else if (ty >= t->texH) ty = t->texH - 1;
                {
                    unsigned int tc = t->texPx[(size_t)ty * t->texW + tx];
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
                        unsigned int y_ = tr*cr + 1; r = (y_ + (y_ >> 8)) >> 8;
                        y_ = tg*cg + 1; g = (y_ + (y_ >> 8)) >> 8;
                        y_ = tb*cb + 1; b = (y_ + (y_ >> 8)) >> 8;
                        y_ = ta*ca + 1; al = (y_ + (y_ >> 8)) >> 8;
                    }
                    col = ((unsigned)al << 24) | (r << 16) | (g << 8) | b;
                }
            }
            if (t->stAlphaTest && ((col >> 24) & 0xFF) < t->stAlphaRef) {
                /* drop */
            } else if (!t->stBlendOn) {
                dst[i] = col;
            } else {
                dst[i] = SR_BlendPixel(col, dst[i], t);      /* 通用混合 */
            }
            if (t->useTex) { ui += t->dui; vi += t->dvi; }
            if (!t->uniform) { ci_r += t->dcr; ci_g += t->dcg; ci_b += t->dcb; ci_a += t->dca; }
        }
    }
}

/* ---------------- 命令队列 + 行带并行 ---------------- */
#define SR_MAX_CMD     16384
#define SR_MAX_THREADS 8

typedef struct { SR_Vtx a, b, c; SR_State st; const SR_Tex *tex; } SR_Cmd;
static SR_Cmd SR_cmds[SR_MAX_CMD];
static SR_Tri SR_tris[SR_MAX_CMD];
static int    SR_cmdCount = 0;
static const SR_Tex *SR_boundTex = NULL;

static int SR_nThreads = 0, SR_nBands = 0, SR_bandH = 0;
static HANDLE SR_hWake[SR_MAX_THREADS], SR_hThread[SR_MAX_THREADS], SR_hDone = NULL;
static volatile LONG SR_bandIdx = 0, SR_busy = 0, SR_quit = 0;

static int SR_TotalThreads(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

static DWORD WINAPI SR_Worker(LPVOID p)
{
    int tid = (int)(INT_PTR)p;
    for (;;) {
        WaitForSingleObject(SR_hWake[tid - 1], INFINITE);
        if (SR_quit) return 0;
        for (;;) {
            int b = (int)InterlockedIncrement(&SR_bandIdx) - 1;
            int i;
            if (b >= SR_nBands) break;
            {
                int yF = b * SR_bandH, yT = yF + SR_bandH;
                if (yT > SR_h) yT = SR_h;
                for (i = 0; i < SR_cmdCount; i++)
                    SR_TriRows(&SR_tris[i], yF, yT);
            }
        }
        if (InterlockedDecrement(&SR_busy) == 0) SetEvent(SR_hDone);
    }
}

static void SR_SetThreads(int n)
{
    int i;
    if (n < 1) n = 1;
    if (n > SR_MAX_THREADS + 1) n = SR_MAX_THREADS + 1;
    if (SR_nThreads + 1 == n && SR_hDone) return;

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
    if (n <= 1) return;

    for (i = 0; i < n - 1 && i < SR_MAX_THREADS; i++) {
        SR_hWake[i] = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (!SR_hWake[i]) break;
        SR_hThread[i] = CreateThread(NULL, 0, SR_Worker, (LPVOID)(INT_PTR)(i + 1), 0, NULL);
        if (!SR_hThread[i]) { CloseHandle(SR_hWake[i]); SR_hWake[i] = NULL; break; }
        SR_nThreads++;
    }
}

static int SR_Queue(const SR_Vtx *a, const SR_Vtx *b, const SR_Vtx *c,
                    const SR_State *st, const SR_Tex *tex)
{
    SR_Cmd *cmd;
    if (SR_cmdCount >= SR_MAX_CMD) return 0;
    cmd = &SR_cmds[SR_cmdCount++];
    cmd->a = *a; cmd->b = *b; cmd->c = *c;
    cmd->st = *st; cmd->tex = tex;
    return 1;
}

static void SR_Flush(void)
{
    int i, nw;
    if (!SR_buf || SR_cmdCount == 0) return;

    {
        int n = 0;
        for (i = 0; i < SR_cmdCount; i++) {
            if (SR_TriSetup(&SR_tris[n], &SR_cmds[i].a, &SR_cmds[i].b, &SR_cmds[i].c,
                            &SR_cmds[i].st, SR_cmds[i].tex))
                n++;
        }
        SR_cmdCount = n;
    }
    if (SR_cmdCount == 0) return;

    nw = SR_nThreads + 1;
    if (nw > 1 && SR_hDone) {
        SR_nBands = nw * 4;
        SR_bandH  = (SR_h + SR_nBands - 1) / SR_nBands;
        if (SR_bandH < 4) SR_bandH = 4;
        SR_nBands = (SR_h + SR_bandH - 1) / SR_bandH;

        InterlockedExchange(&SR_bandIdx, 0);
        InterlockedExchange(&SR_busy, SR_nThreads);
        ResetEvent(SR_hDone);
        for (i = 0; i < SR_nThreads; i++)
            if (SR_hWake[i]) SetEvent(SR_hWake[i]);

        /* 主线程也参与 */
        for (;;) {
            int b = (int)InterlockedIncrement(&SR_bandIdx) - 1;
            int yF, yT;
            if (b >= SR_nBands) break;
            yF = b * SR_bandH; yT = yF + SR_bandH;
            if (yT > SR_h) yT = SR_h;
            for (i = 0; i < SR_cmdCount; i++)
                SR_TriRows(&SR_tris[i], yF, yT);
        }
        WaitForSingleObject(SR_hDone, INFINITE);
    } else {
        for (i = 0; i < SR_cmdCount; i++)
            SR_TriRows(&SR_tris[i], 0, SR_h);
    }
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

/* ===========================================================================
 * 兼容层 (build fix) -- 非原作者设计.
 *
 * 新版核心刻意只保留渲染路径, 移除了 SR_Triangle / SR_InitState /
 * SR_BeginFrame / SR_ClearRect / SR_Fill32 / SR_NO_TEXTURE 等旧接口;
 * 但 shim.cpp / test_rast.cpp / bench_rast.c 仍在调用, 编译失败.
 * 这里按旧版语义把它们补回, 不改动 AVX2 热路径.
 *
 * [注] g_spans / g_spanPix / g_whiteSample / g_validSample /
 *      g_texNoBlend / g_texNoAlpha 在新版 SR_TriRows 里没有统计出口,
 *      因此恒为 0 (shim 诊断页显示 0 属正常, 不代表渲染异常).
 *      只有 g_triIn / g_triDegenerate / g_triOffscreen 是真实计数.
 * ========================================================================= */

#define SR_NO_TEXTURE  0x00000000u

/* SSAA 倍率设置. 必须在 SR_Reset 之前调用 (影响帧缓冲尺寸).
   1 = 关, 2 = 4x, 3 = 9x, 4 = 16x. 超出 1..4 则钳制. */
static void SR_SetSuperSample(int n)
{
    if (n < 1) n = 1;
    if (n > 4) n = 4;
    SR_ss = n;
}

static void SR_InitState(void)
{
    memset(&SR_st, 0, sizeof(SR_st));
    SR_st.colorOp  = 0;
    SR_st.srcBlend = 5;
    SR_st.dstBlend = 6;
    SR_st.zwrite   = 1;
    SR_st.blendOn  = 0;
}

/* AVX2 填充 n 个像素 (旧版为 SSE2) */
static void SR_Fill32(unsigned int *dst, unsigned int col, int n)
{
    __m256i v = _mm256_set1_epi32((int)col);
    int i = 0;
    for (; i + 8 <= n; i += 8)
        _mm256_storeu_si256((__m256i *)(dst + i), v);
    for (; i < n; i++) dst[i] = col;
}

/* 帧开始: 丢弃上一个未 Flush 的批次 (与旧版一致) */
static void SR_BeginFrame(void)
{
    SR_cmdCount = 0;
}

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

/* 单三角形立即绘制 (测试 / 独立使用). 走新版 SR_TriSetup + SR_TriRows. */
static void SR_Triangle(const SR_Vtx *a, const SR_Vtx *b, const SR_Vtx *c)
{
    SR_Tri t;
    if (!SR_buf) return;
    g_triIn++;
    if (!SR_TriSetup(&t, a, b, c, &SR_st, SR_boundTex)) return;
    SR_TriRows(&t, 0, SR_h);
}

#endif /* SOFTRAST_AVX2_H */