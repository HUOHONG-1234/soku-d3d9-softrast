# 架构

本文说明 `th123_Softpipe` 的运行路径：从游戏发出的一次绘制调用，到屏幕上出现像素，
中间经过哪些组件、各自负责什么。

## 分层

```
th123.exe                     32 位进程，D3D9 固定管线，顶点为屏幕坐标（XYZRHW）
  │
  │  LoadLibrary("d3d9.dll")
  ▼
d3d9.dll                      加载器（loader/）
  │   · 读取 soku_loader.ini
  │   · 把游戏对 d3d9 的调用转给 ChainDll 指定的后端
  │   · 可选加载 SWRSToys 风格的模组
  ▼
cpu_d3d9.dll                  本项目主体（src/shim.cpp）
  │   · 实现 IDirect3D9 / IDirect3DDevice9 / 纹理 / 表面 / 交换链
  │   · 截住绘制调用 → 交给软光栅
  ▼
softrast.h                    CPU 软光栅核心
  │   顶点解码 → 三角形光栅化 → 混合 → 写帧缓冲（ARGB8888）
  ▼
d3d9out.h                     输出端
  │   帧缓冲 → LockRect 离屏面 → UpdateSurface → Present
  ▼
真正的 d3d9（DXVK / dgVoodoo2 / 系统）
      GPU 只负责把成品位图搬上屏
```

**关键点：GPU 不参与几何与着色。** 所有顶点变换、光栅化、纹理采样、alpha 混合
都在 CPU 上算完，交给 GPU 的只有一张已经画好的位图。

## 一次绘制的生命周期

```
DrawPrimitiveUP(type, count, pVtx, stride)
  │
  ├─ ① 解码顶点：XYZRHW + 可选 DIFFUSE / TEX1
  │     屏幕坐标，无需矩阵变换
  │
  ├─ ② 入队 SR_Queue()，不立即光栅化
  │     记录三角 + 当时的渲染状态快照 + 绑定的纹理
  │
  …游戏继续画下一批…
  │
  ├─ ③ Present 触发 SR_Flush()
  │     整帧统一光栅化，按行带切分到多个工作线程
  │
  ├─ ④ 帧缓冲交给 d3d9out
  │     LockRect(离屏面) → memcpy → UpdateSurface → Present
  │
  └─ ⑤ TexReleaseHeld() 释放本帧持有的纹理引用
```

**为什么要入队而不是就地画：** 实测每次 `DrawPrimitiveUP` 平均只有 2 个三角形，
逐次开线程的开销远大于光栅本身。整帧入队、`Present` 时一次性并行是唯一可行的粒度。

## 线程模型

```
SR_Flush()
  ├─ SR_nBands = CPU 核数 × 4
  ├─ SR_bandH  = 帧高 / 行带数（下限 4 行）
  └─ 工作线程各自抢行带（InterlockedIncrement 取号），画完自增计数器
     主线程亦参与，画完自己的行带后等待其余线程
```

**行带切分不改变混合顺序。** 一个像素只属于一个行带，同一条竖直方向的
重叠三角仍按入队顺序处理，与单线程结果一致。

## 纹理

```
CreateTexture
  └─ 从静态池 g_tex[MAX_TEX] 取一个槽位（MAX_TEX = 4096）
      不接触 Glide/D3D 的 TMU —— 像素完全由 CPU 持有

LockRect / UnlockRect
  └─ 游戏写入的像素转成 ARGB8888 存进 CPU 侧缓冲

SetTexture
  └─ 绑定到光栅状态

Release → 引用计数归零
  └─ 只置 inUse = 0（延迟释放），像素缓冲不立即 free
```

**为什么释放要延迟：** 游戏调用 `Clear` 之后不会重新 `SetTexture`，纹理可能仍被
上一帧的命令引用着。立即 free 会产生野指针。槽位在下次被复用时才真正清理。

表面（`g_texSurf[MAX_TEXSURF]`）走同一套逻辑。

## 输出格式

离屏面固定为 `D3DFMT_X8R8G8B8`，帧缓冲为 ARGB8888，两者逐像素对应，
拷贝不需要转换。

## 已知的降级路径

- `DrawPrimitive` / `DrawIndexedPrimitive` / `SetStreamSource` / `SetIndices` 目前是**桩**。
  游戏实际走的是 `DrawPrimitiveUP`，所以画面正确 —— 但这意味着**任何使用顶点缓冲的
  绘制路径都不会被渲染**。
- 顶点着色器与像素着色器同样是桩（11 个 pixel + 3 个 vertex）。
  画面正确说明游戏对这些状态存在不依赖它们的路径。
- `SetVertexShader` 在**对局内**出现约 2000 次，主菜单/角色选择则没有。
  这条路径尚未查明。
