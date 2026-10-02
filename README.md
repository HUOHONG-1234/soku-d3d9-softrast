# th123_Softpipe
## 本项目由AI维护，哎呀

这是一个 **实验性项目** 发生不可预料的问题是**正常的**
## 构建

需要 **Visual Studio 2019+** 的 MSVC（x86 工具链）与 Windows SDK。
或 **检查发行** 那里有**预编译包**
```powershell
.\build.ps1
```
脚本会自动探测 MSVC 与 SDK 路径。


## 许可

本项目源码采用 **MIT** 许可，见 `LICENSE`。

预编译包中**只包含本项目自身的二进制**。DXVK、dgVoodoo2、以及微软的
DirectX 9.0c 运行库（`d3dx9_33.dll`）均为**运行时**动态加载的独立组件，
不随包分发，各自遵循其原有许可。
