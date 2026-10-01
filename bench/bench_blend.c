/* bench_blend.c -- 比较三种 AVX2 alpha 混合实现的吞吐与精度
 *
 *  (a) baseline : 现在的整数 32 位版 (SR_Blend8 同型)
 *  (b) fma      : 乘加走 FMA(float), 拆包/掩码/移位/打包仍走整数
 *  (c) fullfloat: 全浮点 (FMA + 1/255 也走浮点乘法)
 *
 *  目标 CPU: i5-9400F (Coffee Lake, AVX2 + FMA3)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <immintrin.h>

#define W 640
#define H 480
#define NPIX (W * H)
#define ROUNDS 400
/* 实测每帧混合像素数(见 shim_stats 直方图) ≈ 1,042,230，约 3.4 个屏幕 */
#define PER_FRAME 1042230.0

static unsigned int *g_src = NULL, *g_dst = NULL, *g_dst2 = NULL, *g_dst3 = NULL;

static unsigned long long qpc_now(void)
{
    LARGE_INTEGER li; QueryPerformanceCounter(&li); return (unsigned long long)li.QuadPart;
}

/* ---------------- (a) 整数 32 位 ---------------- */
static inline __m256i div255x8(__m256i x)
{
    x = _mm256_add_epi32(x, _mm256_set1_epi32(1));
    {
        __m256i t = _mm256_srli_epi32(x, 8);
        return _mm256_srli_epi32(_mm256_add_epi32(x, t), 8);
    }
}

static void blend_int(unsigned int *dst, const unsigned int *src, int n)
{
    int i;
    for (i = 0; i + 8 <= n; i += 8) {
        __m256i s = _mm256_loadu_si256((const __m256i *)(src + i));
        __m256i d = _mm256_loadu_si256((const __m256i *)(dst + i));
        __m256i m = _mm256_set1_epi32(0xFF);
        __m256i sa = _mm256_srli_epi32(s, 24);
        __m256i da = _mm256_srli_epi32(d, 24);
        __m256i inv = _mm256_sub_epi32(_mm256_set1_epi32(255), sa);
        __m256i sr = _mm256_and_si256(_mm256_srli_epi32(s, 16), m);
        __m256i sg = _mm256_and_si256(_mm256_srli_epi32(s,  8), m);
        __m256i sb = _mm256_and_si256(s, m);
        __m256i dr = _mm256_and_si256(_mm256_srli_epi32(d, 16), m);
        __m256i dg = _mm256_and_si256(_mm256_srli_epi32(d,  8), m);
        __m256i db = _mm256_and_si256(d, m);
        __m256i r = _mm256_add_epi32(_mm256_mullo_epi32(sr, sa), _mm256_mullo_epi32(dr, inv));
        __m256i g = _mm256_add_epi32(_mm256_mullo_epi32(sg, sa), _mm256_mullo_epi32(dg, inv));
        __m256i b = _mm256_add_epi32(_mm256_mullo_epi32(sb, sa), _mm256_mullo_epi32(db, inv));
        __m256i a = _mm256_add_epi32(sa, _mm256_mullo_epi32(da, inv));
        r = div255x8(r); g = div255x8(g); b = div255x8(b);
        a = _mm256_min_epi32(div255x8(a), _mm256_set1_epi32(255));
        {
            __m256i o = _mm256_or_si256(
                _mm256_or_si256(_mm256_slli_epi32(a, 24), _mm256_slli_epi32(r, 16)),
                _mm256_or_si256(_mm256_slli_epi32(g, 8), b));
            _mm256_storeu_si256((__m256i *)(dst + i), o);
        }
    }
}

/* ---------------- (b) 乘加走 FMA, 其余整数 ---------------- */
static void blend_fma(unsigned int *dst, const unsigned int *src, int n)
{
    int i;
    const __m256 inv255 = _mm256_set1_ps(1.0f / 255.0f);
    const __m256 f255   = _mm256_set1_ps(255.0f);
    for (i = 0; i + 8 <= n; i += 8) {
        __m256i s = _mm256_loadu_si256((const __m256i *)(src + i));
        __m256i d = _mm256_loadu_si256((const __m256i *)(dst + i));
        __m256i m = _mm256_set1_epi32(0xFF);
        __m256i sa_i = _mm256_srli_epi32(s, 24);
        __m256i da_i = _mm256_srli_epi32(d, 24);
        __m256i sr_i = _mm256_and_si256(_mm256_srli_epi32(s, 16), m);
        __m256i sg_i = _mm256_and_si256(_mm256_srli_epi32(s,  8), m);
        __m256i sb_i = _mm256_and_si256(s, m);
        __m256i dr_i = _mm256_and_si256(_mm256_srli_epi32(d, 16), m);
        __m256i dg_i = _mm256_and_si256(_mm256_srli_epi32(d,  8), m);
        __m256i db_i = _mm256_and_si256(d, m);
        {
            __m256 fsa  = _mm256_cvtepi32_ps(sa_i);
            __m256 finv = _mm256_sub_ps(f255, fsa);
            __m256 fr = _mm256_fmadd_ps(_mm256_cvtepi32_ps(sr_i), fsa,
                                        _mm256_mul_ps(_mm256_cvtepi32_ps(dr_i), finv));
            __m256 fg = _mm256_fmadd_ps(_mm256_cvtepi32_ps(sg_i), fsa,
                                        _mm256_mul_ps(_mm256_cvtepi32_ps(dg_i), finv));
            __m256 fb = _mm256_fmadd_ps(_mm256_cvtepi32_ps(sb_i), fsa,
                                        _mm256_mul_ps(_mm256_cvtepi32_ps(db_i), finv));
            __m256 fa = _mm256_fmadd_ps(_mm256_mul_ps(_mm256_cvtepi32_ps(da_i), inv255), finv, fsa);
            __m256i r = _mm256_cvtps_epi32(_mm256_mul_ps(fr, inv255));
            __m256i g = _mm256_cvtps_epi32(_mm256_mul_ps(fg, inv255));
            __m256i b = _mm256_cvtps_epi32(_mm256_mul_ps(fb, inv255));
            __m256i a = _mm256_min_epi32(_mm256_cvtps_epi32(fa), _mm256_set1_epi32(255));
            __m256i o = _mm256_or_si256(
                _mm256_or_si256(_mm256_slli_epi32(a, 24), _mm256_slli_epi32(r, 16)),
                _mm256_or_si256(_mm256_slli_epi32(g, 8), b));
            _mm256_storeu_si256((__m256i *)(dst + i), o);
        }
    }
}

/* ---------------- (c) 全浮点 ---------------- */
static void blend_fullfloat(unsigned int *dst, const unsigned int *src, int n)
{
    int i;
    const __m256 inv255 = _mm256_set1_ps(1.0f / 255.0f);
    for (i = 0; i + 8 <= n; i += 8) {
        __m256i s = _mm256_loadu_si256((const __m256i *)(src + i));
        __m256i d = _mm256_loadu_si256((const __m256i *)(dst + i));
        __m256i m = _mm256_set1_epi32(0xFF);
        {
            (void)m;
            /* 逐通道: alpha = (s>>24)&0xFF */
            __m256 sa = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srli_epi32(s,24), m)), inv255);
            __m256 da = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srli_epi32(d,24), m)), inv255);
            __m256 inv = _mm256_sub_ps(_mm256_set1_ps(1.0f), sa);
            __m256 sr = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srli_epi32(s,16), m)), inv255);
            __m256 sg = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srli_epi32(s, 8), m)), inv255);
            __m256 sb = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_and_si256(s, m)), inv255);
            __m256 dr = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srli_epi32(d,16), m)), inv255);
            __m256 dg = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srli_epi32(d, 8), m)), inv255);
            __m256 db = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_and_si256(d, m)), inv255);
            __m256 fr = _mm256_fmadd_ps(sr, sa, _mm256_mul_ps(dr, inv));
            __m256 fg = _mm256_fmadd_ps(sg, sa, _mm256_mul_ps(dg, inv));
            __m256 fb = _mm256_fmadd_ps(sb, sa, _mm256_mul_ps(db, inv));
            __m256 fa = _mm256_fmadd_ps(da, inv, sa);
            __m256i r = _mm256_cvtps_epi32(_mm256_mul_ps(fr, _mm256_set1_ps(255.0f)));
            __m256i g = _mm256_cvtps_epi32(_mm256_mul_ps(fg, _mm256_set1_ps(255.0f)));
            __m256i b = _mm256_cvtps_epi32(_mm256_mul_ps(fb, _mm256_set1_ps(255.0f)));
            __m256i a = _mm256_min_epi32(_mm256_cvtps_epi32(_mm256_mul_ps(fa, _mm256_set1_ps(255.0f))),
                                         _mm256_set1_epi32(255));
            __m256i o = _mm256_or_si256(
                _mm256_or_si256(_mm256_slli_epi32(a, 24), _mm256_slli_epi32(r, 16)),
                _mm256_or_si256(_mm256_slli_epi32(g, 8), b));
            _mm256_storeu_si256((__m256i *)(dst + i), o);
        }
    }
}

static unsigned int xorshift(unsigned int *s)
{
    unsigned int x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x; return x;
}

int main(void)
{
    LARGE_INTEGER f; double freq;
    int i, k;
    unsigned int seed = 12345;
    double t[3], nsp[3];
    int diff_max = 0, diff_cnt = 0;

    QueryPerformanceFrequency(&f); freq = (double)f.QuadPart;

    g_src  = (unsigned int *)_aligned_malloc(NPIX * 4, 32);
    g_dst  = (unsigned int *)_aligned_malloc(NPIX * 4, 32);
    g_dst2 = (unsigned int *)_aligned_malloc(NPIX * 4, 32);
    g_dst3 = (unsigned int *)_aligned_malloc(NPIX * 4, 32);
    if (!g_src || !g_dst || !g_dst2 || !g_dst3) { printf("alloc fail\n"); return 1; }

    /* 造数据: 半透明精灵 + 不透明底 */
    for (i = 0; i < NPIX; i++) {
        unsigned int a = 1 + (xorshift(&seed) % 255);
        g_src[i] = (a << 24) | (xorshift(&seed) & 0x00FFFFFF);
        g_dst[i] = 0xFF000000u | (xorshift(&seed) & 0x00FFFFFF);
    }

    printf("canvas %dx%d (%d px), rounds %d\n\n", W, H, NPIX, ROUNDS);

    /* 计时 (a) */
    {
        unsigned long long t0, t1;
        memcpy(g_dst2, g_dst, NPIX * 4);
        t0 = qpc_now();
        for (k = 0; k < ROUNDS; k++) blend_int(g_dst2, g_src, NPIX);
        t1 = qpc_now();
        t[0] = (double)(t1 - t0) / freq;
    }
    /* 计时 (b) */
    {
        unsigned long long t0, t1;
        memcpy(g_dst3, g_dst, NPIX * 4);
        t0 = qpc_now();
        for (k = 0; k < ROUNDS; k++) blend_fma(g_dst3, g_src, NPIX);
        t1 = qpc_now();
        t[1] = (double)(t1 - t0) / freq;
    }
    /* 精度: (b) vs (a) */
    for (i = 0; i < NPIX; i++) {
        int da = 0;
        unsigned int x = g_dst2[i], y = g_dst3[i];
        for (k = 0; k < 4; k++) {
            int cb = (x >> (k*8)) & 0xFF, cc = (y >> (k*8)) & 0xFF;
            int d = cb - cc; if (d < 0) d = -d;
            if (d > da) da = d;
        }
        if (da > 1) diff_cnt++;
        if (da > diff_max) diff_max = da;
    }

    /* (c) 全浮点 */
    {
        unsigned long long t0, t1;
        static unsigned int tmp[NPIX];
        memcpy(tmp, g_dst, NPIX * 4);
        t0 = qpc_now();
        for (k = 0; k < ROUNDS; k++) blend_fullfloat(tmp, g_src, NPIX);
        t1 = qpc_now();
        t[2] = (double)(t1 - t0) / freq;
    }

    for (i = 0; i < 3; i++) nsp[i] = t[i] / (double)ROUNDS / (double)NPIX * 1e9;

    printf("(a) int32      %8.2f ms / pass   %6.3f ns/px   frame-equiv %6.3f ms   [baseline]\n",
           t[0] / ROUNDS * 1000.0, nsp[0], nsp[0] * PER_FRAME / 1e6);
    printf("(b) FMA muladd %8.2f ms / pass   %6.3f ns/px   frame-equiv %6.3f ms   %.2fx\n",
           t[1] / ROUNDS * 1000.0, nsp[1], nsp[1] * PER_FRAME / 1e6, t[0] / t[1]);
    printf("(c) full float %8.2f ms / pass   %6.3f ns/px   frame-equiv %6.3f ms   %.2fx\n",
           t[2] / ROUNDS * 1000.0, nsp[2], nsp[2] * PER_FRAME / 1e6, t[0] / t[2]);

    printf("\nprecision (b) vs (a): max channel delta = %d, pixels off-by-more-than-1 = %d\n",
           diff_max, diff_cnt);
    printf("checksum a=%08X b=%08X c-last=%08X\n", g_dst2[12345], g_dst3[12345],
           ((unsigned int *)g_dst3)[0]);

    _aligned_free(g_src); _aligned_free(g_dst);
    _aligned_free(g_dst2); _aligned_free(g_dst3);
    return 0;
}
