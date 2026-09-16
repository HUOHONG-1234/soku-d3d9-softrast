# soku-d3d9-softrast

把 **东方非想天则**（Touhou Hisoutensoku, `th123.exe`）的 D3D9 绘制调用截下来，
**用 CPU 软光栅自己画完一整帧**，再把成品位图交给真正的 D3D9 显示。

定位：**实验性项目**。性能方向有效，渲染正确性尚未收敛（见「已知问题」）。

```
th123.exe
  │  发出 D3D9 绘制调用 (DrawPrimitiveUP 等)
  ▼
d3d9.dll            ← 加载器（链式代理 + 可选的模组加载）
  ▼
cpu_d3d9.dll        ← 本项目主体：截住所有绘制，自己光栅化
  │  ① 顶点解码（XYZRHW，屏幕坐标，无需矩阵）
  │  ② 三角形光栅化（6 线程，定点 16.16，SSE2）
  │  ③ 写入帧缓冲（ARGB8888）
  ▼
真 d3d9（DXVK / 原生 / dgVoodoo2 …）
  │  ④ LockRect 离屏面 → memcpy 帧缓冲
  │  ⑤ UpdateSurface 贴到 backbuffer
  │  ⑥ Present
  ▼
GPU：只负责把这张位图放上屏
```

## 实测数据

在 i5-9400F（6 核 / 3.9GHz）+ RX 580 上测得：

| 指标 | 数值 |
|---|---|
| 光栅（6 线程） | **2.2 ms/帧** |
| 单线程吞吐（带纹理+混合） | 60.4 M px/s |
| 6 线程吞吐 | 265.6 M px/s（**4.40x**） |
| 每帧填充像素 | 1,002,225（屏幕的 3.26 倍，过度绘制） |
| 每像素成本 | 37.7 cycles（单线程） |
| 帧间隔 | 16.81 ms（**59.5 fps**，轻场景） |
| DXVK 那一跳（Present） | 0.082 ms/帧（**不是瓶颈**） |

关键结论：优化光栅对帧率影响有限 —— 游戏自身逻辑占 86%，光栅只占 14%。
但光栅优化解决了**符卡效果卡顿**（尖峰 67ms → 12ms，超预算帧 17% → 0%）。

## 构建

需要 **Visual Studio 2019+** 的 MSVC（x86 工具链）与 Windows SDK。

```powershell
.\build.ps1
```

脚本会自动探测 MSVC 与 SDK 路径（不写死安装位置）。

编译注意事项：

- 必须 `/source-charset:utf-8 /execution-charset:gbk`。
  否则源码里的窄字符串按 UTF-8 存、Win32 ANSI API 按 GBK 解释 → 中文乱码。
- 目标为 **x86**（游戏是 32 位进程）。
- `/EHa`（COM 异常经 `__try` 处理）与 `/MT`（静态 CRT，免运行库）。

## 测试

```powershell
# 光栅覆盖率回归测试（8 项，改光栅前后都跑一遍）
cl /I src test\test_rast.cpp /link /OUT:test_rast.exe
.\test_rast.exe

# 独立压测（含多线程加速比）
cl /I src bench\bench_rast.c /link /OUT:bench_rast.exe
.\bench_rast.exe
```

`test_rast.exe` 是**回归防线**：它用已知几何量覆盖率，
能立刻发现绕序（镜像精灵）、像素中心取整这类错误。

## 配置

游戏目录下 `soku_shim.ini`：

```ini
[soku_shim]
; 真 d3d9 的位置。留空 = 自动探测
;   backends\dxvk -> backends\dgvoodoo2 -> backends\wined3d -> 系统
RealD3D9=

; 冒充的显卡型号（让游戏的设备创建永不失败）
;   gf5200 | gma900 | weak
Gpu=gf5200

; 帧延迟环长度（帧）。0 = 关闭
;   600 帧 = 10 秒 @60fps，占 703 MB
;   注意：th123.exe 是 32 位且未设 LAA → 地址空间 2GB
;        实测上限约 1747 帧（29 秒），超了会分配失败并自动关闭
FrameDelay=0
```

## 运行时的同伴组件

本项目**不打包**下列运行时依赖，请自行获取：

- **DXVK**（推荐 2.x；3.x 需要 `VK_KHR_load_store_op_none`，老驱动不支持）
- **dgVoodoo2**（免费但闭源，请从其官网下载，勿二次分发）
- **WineD3D**（实测白屏，仅作参考）

加载器 `loader/` 是配套的链式代理，负责把游戏对 `d3d9.dll` 的调用转到
真正的后端上，并可选加载 SWRSToys 风格的模组。

## 已知问题

1. **黑色背景/黑边未消除** —— 未抠透的素材仍会露出底色。
   已排除：纹理内容（导出验证正常）、混合模式（直方图只有普通 alpha）。
   怀疑方向：帧级批处理改变了指令顺序。
2. **不支持全屏模式** —— `IDirect3DDevice9::Reset` 已补实现，未经充分验证。
3. **帧率** —— 轻场景 60fps，重场景约 35fps（瓶颈在游戏自身逻辑）。

## 源码结构

```
src/
  shim.cpp            D3D9 设备/接口实现，绘制命令拦截与批处理
  softrast.h          CPU 软光栅核心（跨行扫描线 / 定点 / SSE2 / 行带并行）
  d3d9out.h           D3D9 输出端（离屏面 → backbuffer → Present）
  d3dx9_proxy.cpp     D3DXCreateTexture 代理（绕开 D3DX 内部校验）
  stubs_*.h           COM 接口桩（由 tools/gen_stubs.py 生成，勿手改）
  core_proto.h        自动生成的 Core_* 原型
loader/               配套的 d3d9 链式代理与 Win32 配置器
test/                 光栅回归测试
bench/                独立压测台
tools/                代码生成器
scripts/              诊断/测量脚本
```

### 几条不能违反的约定

- **COM 虚表顺序 = d3d9.h 的声明顺序**。类体里方法一个都不能挪，
  手写方法插在 `#include "stubs_*.h"` 之前会让整张表错位。
- **绝不对纹理对象整体 `memset`** —— 首 4 字节是 vptr，清零后任何虚调用都会跳空指针。
- **纹理释放必须延迟** —— `Release()` 到 0 时不能立刻 free 像素，
  因为游戏可能仍绑着它（Clear 之后不会重新 `SetTexture`）。

## 许可

本项目源码采用 **MIT** 许可，见 `LICENSE`。

其中的 Glide 常量（`GR_RESOLUTION_640x480`、`GR_LFB_*` 等）参照 3dfx Glide SDK
头文件的事实性定义。

DXVK、dgVoodoo2、WineD3D 均为**运行时**动态加载的独立项目，
不包含在本仓库内，各自遵循其原有许可。
