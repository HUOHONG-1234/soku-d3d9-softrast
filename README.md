# th123_Softpipe
## 本项目由AI维护，哎呀

把 **东方非想天则**（Touhou Hisoutensoku, `th123.exe`）的 D3D9 绘制调用截下来，
**用 CPU 软光栅渲染一整帧**，再把成品位图交给真正的 D3D9 显示。

GPU 在这条链路里只剩一件事：把一张位图放上屏。几何、纹理、混合全在 CPU 上算完。

**实验性项目**。性能方向有效，渲染正确性尚未收敛（见「已知问题」）。

```
th123.exe
  │  发出 D3D9 绘制调用
  ▼
d3d9.dll            ← 加载器（链式代理 + 可选的模组加载）
  ▼
cpu_avx2.dll        ← 本项目主体：截住所有绘制，自己光栅化
  │  ① 顶点解码（XYZRHW，屏幕坐标，无需矩阵）
  │  ② 三角形光栅化（多线程 / 定点 16.16 / AVX2 或 SSE2）
  │  ③ 写入帧缓冲（ARGB8888）
  ▼
真 d3d9
  │  ④ LockRect 离屏面 → memcpy 帧缓冲
  │  ⑤ UpdateSurface 贴到 backbuffer
  │  ⑥ Present
  ▼
GPU：只负责把这张位图放上屏
```

## 两个 CPU 后端

光栅核心有 AVX2 和 SSE2 两套实现，算法相同，只有内层向量宽度不同：

| 后端 | 要求 | 说明 |
|---|---|---|
| `cpu_avx2.dll` | Haswell（2013）及以后 | 默认。8 路 32 位整数，混合与填充都快一档 |
| `cpu_sse2.dll` | 任意 x64 CPU | 兜底。老机器、虚拟机、核显本用这个 |

不确定就用 SSE2，它不会跑不起来。切换方式：编辑 `soku_loader.ini` 的 `ChainDll=`，
或运行 `soku_config.exe`。

## 构建

需要 **Visual Studio 2019+** 的 MSVC（x86 工具链）与 Windows SDK。
脚本会自动探测 MSVC 与 SDK 路径。

```powershell
# 单个后端，默认 AVX2
.\build.ps1
.\build.ps1 -Arch SSE2 -OutDir .\out_sse2

# 出发布包：两个后端都编，跑完回归再打包
.\package.ps1
```

编译注意事项（改脚本时别丢）：

- 必须 `/source-charset:utf-8 /execution-charset:gbk`。
  否则源码里的窄字符串按 UTF-8 存、Win32 ANSI API 按 GBK 解释 → 中文乱码。
- 目标为 **x86**（游戏是 32 位进程）。
- `/EHa`（COM 异常经 `__try` 处理）与 `/MT`。
- 核心头文件由架构决定：`-Arch AVX2` 编译 `src/softrast_avx2.h`，
  `-Arch SSE2` 编译 `src/softrast_sse2.h`，构建时生成 `src/softrast.h`。
  这个生成物**不进版本库**。

## 性能

`bench\bench_rast.exe`，640×480、400 quad × 60 次、带纹理 alpha 混合 + MODULATE，
实测机 i5-9400F（6 核 6 线程）：

| 线程 | AVX2 | SSE2 |
|---|---|---|
| 1 | 82.1 Mpx/s | 58.4 Mpx/s |
| 4 | 309.6 Mpx/s | 207.7 Mpx/s |
| 6 | 436.2 Mpx/s | 302.4 Mpx/s |

AVX2 单线程比 SSE2 快约 41%，6 线程加速比 5.31x（SSE2 为 5.18x）。
再往上加线程收益有限——`SR_bandH` 已经压到 4 行，唤醒开销开始吃掉并行收益。

对照 2005 年的显卡理论填充率：GeForce 6200 是 1200 Mpx/s，6600LE 也是 1200，
6800U 是 6800。也就是说本项目的**多线程**吞吐落在 6200 那档的 2/5 左右。

`bench\bench_blend.exe` 单独量混合路径。注意里面的 FMA float 分支目前是**坏的**
（307200 像素全数偏差，checksum 对不上），只作参考，不进默认配置。

## 测试

```powershell
.\out_avx2\test_rast.exe
.\out_sse2\test_rast.exe
```

`test_rast.exe` 是**回归线**：它用已知几何量覆盖率，能立刻发现绕序（镜像）、
像素中心取整这类错误。`package.ps1` 会在打包前跑这两个，任一不过就中止。

## 部署

从 `package.ps1` 出来的包解压到游戏目录（与 `th123.exe` 同级），布局是这样：

```
d3d9.dll                    加载器
d3dx9_33.dll                D3DX 代理
soku_config.exe             配置器
soku_loader.ini             加载链配置（ChainDll 在这里）
soku_shim.ini               渲染配置（Gpu / 抗锯齿在这里）
backends\cpu\cpu_avx2.dll   AVX2 光栅核心
backends\cpu\cpu_sse2.dll   SSE2 光栅核心
backends\lang\*.ini         语言包
```

注意光栅核心**不在根目录**，是放在 `backends\cpu\` 下由加载器按名字去取的。
手动部署时照上面的结构摆，别只把 `cpu_avx2.dll` 丢到根目录。

### 运行时依赖

**不随包分发**，需自行获取或已存在：

- **DirectX 9.0c 运行库** —— 必须。`d3dx9_33.dll` 代理在运行时从系统目录
  （`SysWOW64`）加载真正的 D3DX；缺失时游戏会提示找不到组件。
  这正是**不打包**它的原因：微软的 DirectX 组件不允许再分发。
- **DXVK**（可选，推荐 2.x；3.x 需要较新的 Vulkan 驱动）
- **dgVoodoo2**（可选，免费但闭源 —— 请从官网下载，勿二次分发）

没有第三方后端时，`RealD3D9=` 留空即可回退到系统自带的 d3d9。

## 配置

两个 ini 分工不同，别混了。

`soku_loader.ini` —— 加载器：链到哪个后端、加载哪些模组。

```ini
[soku_loader]
Verbose=1
LoadModules=1
; cpu_avx2.dll = 快，需要 Haswell+（默认）
; cpu_sse2.dll = 任意 x64 CPU，老机器用这个
ChainDll=cpu_avx2.dll
```

`soku_shim.ini` —— 渲染核心：冒充的显卡型号、抗锯齿。

```ini
[soku_shim]
; 真正的 d3d9 位置。留空 = 自动探测
;   backends\dxvk -> backends\dgvoodoo2 -> backends\wined3d -> 系统
RealD3D9=
; 冒充的显卡型号： gf5200 | gma900 | weak
Gpu=gf5200
; off | ssaa | fxaa
AAMode=off
SuperSample=2
```

抗锯齿默认 `off`，最保险。`ssaa` 是真覆盖采样，代价是 `SuperSample²` 倍光栅工作量；
`fxaa` 便宜，但会让 2D 立绘发软。

## 已知问题

1. **黑色背景/黑边未消除** —— 未抠透的素材仍会露出底色。
   已排除：纹理内容（导出验证正常）、混合模式（直方图只有普通 alpha）。

   疑似：帧级批处理改变了指令顺序。
2. **全屏不可用** —— `IDirect3DDevice9::Reset` 已补实现，未经充分验证。
   目前请自备超分工具强制全屏。

## 源码结构

```
src/
  shim.cpp            D3D9 设备/接口实现，绘制命令拦截与批处理
  softrast_avx2.h     CPU 软光栅核心（AVX2，8 路）
  softrast_sse2.h     CPU 软光栅核心（SSE2，4 路）
  d3d9out.h           D3D9 输出端（离屏面 → backbuffer → Present）
  d3dx9_proxy.cpp     D3DXCreateTexture 代理（绕开 D3DX 内部校验）
  stubs_*.h           COM 接口桩（由 tools/gen_stubs.py 生成，勿手改）
  core_proto.h        自动生成的 Core_* 原型
loader/               配套的 d3d9 链式代理与 Win32 配置器
test/                 光栅回归测试
bench/                独立压测台（bench_rast / bench_blend / cache_probe）
tools/                代码生成器
scripts/              诊断/测量脚本
```

### 注意

- **COM 虚表顺序 = d3d9.h 的声明顺序**。类体里方法一个都不能挪，
  手写方法插在 `#include "stubs_*.h"` 之前会让整张表错位。
- **不能对纹理对象整体 `memset`** —— 首 4 字节是 vptr，清零后任何虚调用都会跳空指针。
- **纹理释放需要延迟** —— `Release()` 到 0 时不能立刻 free 像素，
  因为游戏可能仍绑着它（Clear 之后不会重新 `SetTexture`）。
- 两个后端头文件必须**同步改**。改了一边不补另一边，SSE2 用户会看到另一套画面。

## 许可

本项目源码采用 **MIT** 许可，见 `LICENSE`。

预编译包中**只包含本项目自身的二进制**。DXVK、dgVoodoo2、以及微软的
DirectX 9.0c 运行库（`d3dx9_33.dll`）均为**运行时**动态加载的独立组件，
不随包分发，各自遵循其原有许可。
