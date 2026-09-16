# 软光栅核心

`src/softrast.h` —— 全部在 CPU 上完成的几何与着色。

## 数据结构

```c
SR_buf, SR_w, SR_h      帧缓冲（ARGB8888，按行存储）
SR_st                   SR_State：当前渲染状态快照
SR_cmds[SR_MAX_CMD]     绘制命令队列（SR_MAX_CMD = 16384）
SR_tris[SR_MAX_CMD]     展开后的三角形
```

状态快照随每条命令记录，而非全局共享 —— 因为命令要等到 `Present` 才执行，
中途游戏可能已经改过状态。

## 混合

默认状态：

```
srcBlend = 5   D3DBLEND_SRCALPHA
dstBlend = 6   D3DBLEND_INVSRCALPHA
blendOn  = 0
```

`SR_BlendPixel(src, dst)` 走几条快路径：

```
blendOn == 0                       → 直接返回 src
srcBlend==5 && dstBlend==6         → 标准 alpha 混合
srcBlend==2 && dstBlend==1         → 返回 src
srcBlend==1 && dstBlend==1         → 返回 dst
其余                              → 通用 switch
```

除法用 `SR_Div255`（乘法 + 移位近似），避免每像素一次整数除法。

## 光栅化

```
SR_TriSetup(a, b, c)   定点 16.16 求边方程；退化三角形在此被丢弃
SR_TriRows(t, yFrom, yTo, stats)
                       按行扫描，每条扫描线内插属性
```

定点而非浮点：跨行内插要保证同一像素在任意行带切分下结果一致，
浮点累加的误差会随行数漂移。

## 并行

```
主线程与工作线程抢行带：
  b = InterlockedIncrement(&SR_bandIdx) - 1;
  yFrom = b * SR_bandH;  yTo = yFrom + SR_bandH;

SR_bandH 下限 4 行 —— 再细分下去，线程唤醒开销超过收益。
```

`SR_TotalThreads()` 返回主线程 + 工作线程总数，供统计口径使用。

## 统计计数器

```c
g_triIn          入队三角数
g_triDegenerate  退化被丢弃
g_triOffscreen   完全在屏外
g_spans          扫描段数
g_spanPix        覆盖像素数
g_whiteSample    采样到纯白的像素（调试用）
g_texNoBlend     有纹理但混合关闭
g_texNoAlpha     纹理 alpha 全不透明
```

这些计数器是**给回归测试用的仪表**。它们曾经报过与事实相反的数
（见「仪表可信度」一节），任何结论都要先用已知量喂进去验证。

## 帧缓冲导出

```c
SR_DumpBMP(path)   // 输出 24 位 BMP，用于和屏幕截图逐像素比对
```

## 仪表可信度

本项目在调试期出过三次仪表错误，每次都导致方向完全相反的建议：

1. 用「每 120 帧刷新一次的文件」相减算帧率 → 36 fps（实际 49）
2. 「窗口增量 ÷ 全程帧数」→ 每帧耗时低估 60 倍 → 连续数轮认为光栅只占 1%
3. 时间戳自举死锁：守卫要求非零、赋值又只在守卫内 → 耗时分解从未写入文件

**规矩：** 先验证仪表本身，再拿它下结论。
