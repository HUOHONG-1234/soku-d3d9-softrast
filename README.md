# th123_Softpipe
## 本项目由AI维护，哎呀
把 **东方非想天则**（Touhou Hisoutensoku, `th123.exe`）的 D3D9 绘制调用截下来，
**用 CPU 软光栅渲染一整帧**，再把成品位图交给真正的 D3D9 显示。

**实验性项目**。性能方向有效，渲染正确性尚未收敛（见「已知问题」）。

```
th123.exe
  │  发出 D3D9 绘制调用
  ▼
d3d9.dll            ← 加载器（链式代理 + 可选的模组加载）
  ▼
cpu_d3d9.dll        ← 本项目主体：截住所有绘制，自己光栅化
  │  ① 顶点解码（XYZRHW，屏幕坐标，无需矩阵）
  │  ② 三角形光栅化（6 线程，定点 16.16，SSE2）
  │  ③ 写入帧缓冲（ARGB8888）
  ▼
真 d3d9
  │  ④ LockRect 离屏面 → memcpy 帧缓冲
  │  ⑤ UpdateSurface 贴到 backbuffer
  │  ⑥ Present
  ▼
GPU：只负责把这张位图放上屏
```
## 构建

需要 **Visual Studio 2019+** 的 MSVC（x86 工具链）与 Windows SDK。

```powershell
.\build.ps1
```

脚本会自动探测 MSVC 与 SDK 路径。

编译注意事项：

- 必须 `/source-charset:utf-8 /execution-charset:gbk`。
  否则源码里的窄字符串按 UTF-8 存、Win32 ANSI API 按 GBK 解释 → 中文乱码。
- 目标为 **x86**（游戏是 32 位进程）。
- `/EHa`（COM 异常经 `__try` 处理）与 `/MT`。

## 测试

```powershell
# 光栅覆盖率回归测试
cl /I src test\test_rast.cpp /link /OUT:test_rast.exe
.\test_rast.exe

# 独立压测
cl /I src bench\bench_rast.c /link /OUT:bench_rast.exe
.\bench_rast.exe
```

`test_rast.exe` 是**回归线**：它用已知几何量覆盖率，
能立刻发现绕序（镜像）、像素中心取整这类错误。

## 配置

游戏目录下 `soku_shim.ini`：

```ini
[soku_shim]
; d3d9 的位置。留空 = 自动探测
;   backends\dxvk -> backends\dgvoodoo2 -> backends\wined3d -> 系统
RealD3D9=

; 冒充的显卡型号
;   gf5200 | gma900 | weak
Gpu=gf5200
```

## 部署

从源码构建后（或直接使用 releases 里的预编译包），把下列文件拷到游戏目录
（与 `th123.exe` 同级）：

```
d3d9.dll          加载器
cpu_d3d9.dll      CPU 软光栅主体
d3dx9_33.dll      D3DX 代理
soku_config.exe   配置器（可选）
soku_loader.ini    预设 ChainDll=cpu_d3d9.dll
soku_shim.ini      预设 Gpu=gf5200
```

运行 `soku_config.exe` 可切换渲染后端、管理模组加载。

### 运行时依赖

**不随包分发**，需自行获取或已存在：

- **DirectX 9.0c 运行库** —— 必须。`d3dx9_33.dll` 代理在运行时从系统目录
  （`SysWOW64`）加载真正的 D3DX；缺失时游戏会提示找不到组件。
  这正是**不打包**它的原因：微软的 DirectX 组件不允许再分发。
- **DXVK**（可选，推荐 2.x；3.x 需要较新的 Vulkan 驱动）
- **dgVoodoo2**（可选，免费但闭源 —— 请从官网下载，勿二次分发）

没有第三方后端时，`RealD3D9=` 留空即可回退到系统自带的 d3d9。

加载器 `loader/` 是配套的链式代理，负责把游戏对 `d3d9.dll` 的调用转到
真正的后端上，并可选加载 SWRSToys 风格的模组。

## 已知问题

1. **黑色背景/黑边未消除** —— 未抠透的素材仍会露出底色。
   已排除：纹理内容（导出验证正常）、混合模式（直方图只有普通 alpha）。
   
   疑似：帧级批处理改变了指令顺序。
2. **全屏不可用** —— `IDirect3DDevice9::Reset` 已补实现，未经充分验证。

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

### 注意

- **COM 虚表顺序 = d3d9.h 的声明顺序**。类体里方法一个都不能挪，
  手写方法插在 `#include "stubs_*.h"` 之前会让整张表错位。
- **不能对纹理对象整体 `memset`** —— 首 4 字节是 vptr，清零后任何虚调用都会跳空指针。
- **纹理释放需要延迟** —— `Release()` 到 0 时不能立刻 free 像素，
  因为游戏可能仍绑着它（Clear 之后不会重新 `SetTexture`）。

## 许可

本项目源码采用 **MIT** 许可，见 `LICENSE`。

预编译包中**只包含本项目自身的二进制**。DXVK、dgVoodoo2、以及微软的
DirectX 9.0c 运行库（`d3dx9_33.dll`）均为**运行时**动态加载的独立组件，
不随包分发，各自遵循其原有许可。
