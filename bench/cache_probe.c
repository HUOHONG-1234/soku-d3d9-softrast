/* cache_probe.c -- 实测缓存层级 + 不同工作集的读写带宽
   目的: 验证「帧缓冲能塞进 L2/L3」这个判断到底成立不成立。 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static double g_freq;

static double T(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / g_freq;
}

int main(void)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = (double)f.QuadPart;

    /* ---------- 1) 权威缓存层级 ---------- */
    {
        DWORD len = 0, i, cnt;
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION *buf;
        GetLogicalProcessorInformation(NULL, &len);
        buf = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION *)malloc(len + 1024);
        if (buf && GetLogicalProcessorInformation(buf, &len)) {
            cnt = len / sizeof(*buf);
            printf("=== cache hierarchy (GetLogicalProcessorInformation) ===\n");
            for (i = 0; i < cnt; i++) {
                if (buf[i].Relationship == RelationCache) {
                    DWORD lv = buf[i].Cache.Level;
                    DWORD sz = buf[i].Cache.Size;
                    int   ty = buf[i].Cache.Type;
                    unsigned long long mask = (unsigned long long)buf[i].ProcessorMask;
                    int pop = 0; unsigned long long m = mask;
                    while (m) { pop += (int)(m & 1u); m >>= 1; }
                    printf("  L%u  %6u KB  type=%d  covers %d cores\n",
                           lv, sz / 1024u, ty, pop);
                }
            }
        }
        free(buf);
    }

    /* ---------- 2) 顺序 R/W 带宽 vs 工作集 ---------- */
    printf("\n=== sequential R/W bandwidth vs working set (single thread) ===\n");
    {
        static const unsigned int KB[] = { 16, 32, 64, 128, 256, 512,
                                           1024, 1229, 2048, 4096, 9216,
                                           16384, 65536 };
        unsigned int k;
        for (k = 0; k < (unsigned int)(sizeof(KB) / sizeof(KB[0])); k++) {
            size_t bytes = (size_t)KB[k] * 1024;
            unsigned int *p = (unsigned int *)malloc(bytes);
            size_t n, i;
            int iters, it;
            double t0, dt, gbs;
            if (!p) continue;
            n = bytes / 4;
            for (i = 0; i < n; i++) p[i] = (unsigned int)(i & 0xFFFF);
            iters = (int)(134217728ULL / bytes);
            if (iters < 3) iters = 3;
            if (iters > 20000) iters = 20000;
            t0 = T();
            for (it = 0; it < iters; it++) {
                unsigned int a0 = 0, a1 = 0, a2 = 0, a3 = 0;
                for (i = 0; i + 4 <= n; i += 4) {
                    a0 += p[i]; a1 += p[i+1]; a2 += p[i+2]; a3 += p[i+3];
                    p[i] = 0x11223344u + (a0 & 1u);
                    p[i+1] = 0x11223344u + (a1 & 1u);
                    p[i+2] = 0x11223344u + (a2 & 1u);
                    p[i+3] = 0x11223344u + (a3 & 1u);
                }
            }
            dt = T() - t0;
            gbs = (double)bytes * (double)iters * 2.0 / dt / 1073741824.0;
            {
                const char *note = "";
                if (KB[k] <= 32)        note = "L1d (32KB/core)";
                else if (KB[k] <= 256)  note = "L2 (256KB/core)";
                else if (KB[k] <= 9216) note = "L3 (9MB shared)";
                else                    note = "DRAM";
                if (KB[k] == 1229) note = "** 640x480x4 frame buffer **";
                printf("  %7u KB  %8.1f GB/s   %s\n", KB[k], gbs, note);
            }
            free(p);
        }
    }

    /* ---------- 3) 全帧缓冲 vs 分块 ---------- */
    printf("\n=== frame buffer vs tiles (same total pixels) ===\n");
    {
        static const unsigned int SIDE[] = { 64, 128, 256, 640 };   /* tile side */
        unsigned int k;
        for (k = 0; k < (unsigned int)(sizeof(SIDE) / sizeof(SIDE[0])); k++) {
            size_t px = (size_t)SIDE[k] * SIDE[k];
            unsigned int *p = (unsigned int *)malloc(px * 4);
            size_t n, i;
            int iters, it;
            double t0, dt, gbs;
            if (!p) continue;
            n = px;
            for (i = 0; i < n; i++) p[i] = (unsigned int)(i & 0xFFFF);
            /* normalize total work to 128 MB of pixels */
            iters = (int)(33554432ULL / (px ? px : 1));
            if (iters < 3) iters = 3;
            if (iters > 20000) iters = 20000;
            t0 = T();
            for (it = 0; it < iters; it++) {
                unsigned int a0 = 0, a1 = 0;
                for (i = 0; i + 2 <= n; i += 2) {
                    a0 += p[i]; a1 += p[i+1];
                    p[i] = 0x55667788u + (a0 & 1u);
                    p[i+1] = 0x55667788u + (a1 & 1u);
                }
            }
            dt = T() - t0;
            gbs = (double)px * 4.0 * (double)iters * 2.0 / dt / 1073741824.0;
            printf("  tile %3ux%-3u (%6u KB, %6zu px)  %8.1f GB/s\n",
                   SIDE[k], SIDE[k], (unsigned)(px * 4 / 1024), px, gbs);
            free(p);
        }
    }

    printf("\n=== 注: 帧缓冲 640x480x4 = 1229 KB ===\n");
    return 0;
}
