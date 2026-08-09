# Vervormde Skerm Native

作者：**syh**（GitHub：`warmowinter`）

这是长期维护的低负载版本，采用 Win32、Desktop Duplication、Direct3D 11 和 HLSL，运行时不依赖 Electron、Node.js 或 WebView。

## 数据路径

`DXGI Desktop Duplication -> D3D11 GPU texture copy -> desktop + proxy cursor HLSL radial pre-warp -> DXGI swap chain`

桌面帧不会回读到 CPU，也不会上传。桌面默认限制为 30 FPS；代理光标独立按 60 Hz 检查位置，只有移动时才触发额外重绘。桌面画面和光标都没有变化时不会重复执行像素着色和 Present。

帧调度使用 `QueryPerformanceCounter` 和高精度 waitable timer。交换链使用 `SyncInterval=0` 向 DWM 提交，并把最大排队帧数限制为 1，避免垂直同步等待与程序限帧重复叠加。

## 安全设计

- 启动时默认旁路，用户点击按钮后才显示反畸变覆盖层。
- `Esc` 全局紧急退出；`Ctrl+Shift+Alt+F12` 为备用退出热键。
- 控制面板和系统托盘都提供退出入口。
- 覆盖层始终鼠标穿透，不会阻止操作真实桌面。
- 启用代理光标时才隐藏真实系统光标；旁路、退出、离开当前显示器或进入控制面板时立即恢复。
- 全屏 DirectX 覆盖层使用 `WS_EX_LAYERED | WS_EX_TRANSPARENT`，Windows 鼠标命中会跨进程跳过它并直接落到下方程序；只有独立控制面板接收点击。
- 独立看门狗进程在渲染主循环连续失联时终止主进程，让 Windows 立即移除覆盖层，并在主进程来不及清理时恢复系统光标。
- 睡眠、显示模式变化和会话结束时自动旁路。
- 覆盖层与控制面板均从桌面捕获中排除，避免递归反馈。

## 使用

直接运行 `VervormdeSkermNative.exe`。程序启动时处于安全旁路，只显示控制面板：

从 GitHub Release 下载时，选择 `VervormdeSkermNative-v*-win-x64.exe` 可直接运行，或选择 `Vervormde-Skerm-Native-v*-win-x64.zip` 解压后运行。不要选择 GitHub 自动生成的 `Source code` 文件，它只包含源码。

- 点击“启用反畸变”后才显示桌面覆盖层。
- 使用按钮调节 `k1`、`k2`、缩放、畸变中心和校准网格。
- 默认 30 FPS；可切换 60 FPS，但高分辨率显示器上的 GPU 负载会相应增加。
- `Esc` 紧急退出，`Ctrl+Shift+Alt+F12` 备用退出，`Ctrl+Alt+B` 启用/旁路。
- 参数保存在 `%LOCALAPPDATA%\VervormdeSkerm\settings.ini`，但每次启动仍强制从旁路开始。

## 代理光标

Windows 仍按照未变形桌面的真实坐标向目标程序发送鼠标输入。为了让视觉位置与点击位置一致，程序会在源桌面坐标中合成当前 Windows 光标，再让桌面内容和光标一起通过相同的径向 Shader。

代理光标不会移动系统指针，也不会使用 `SendInput` 重新注入鼠标事件，因此不会改变目标程序焦点。程序通过 Windows Magnification API 临时隐藏真实光标，并通过 `GetCursorInfo` 获取当前光标形状和热点位置。若光标纹理准备或系统光标隐藏失败，程序会保留真实光标并停用代理光标，避免双光标或无光标状态。

代理光标位置独立按 60 Hz 检查，即使桌面上限选择 30 FPS，鼠标移动也可以触发最高 60 FPS 的额外重绘。当前版本处理程序所在的单个显示器；鼠标移到其他显示器时使用正常的系统光标。

## 2026-08-02 性能基线

同一台 Windows 机器、1920×1080 桌面、相同自检流程的短时稳态采样：

| 版本 | 进程数 | 工作集 | 私有内存 | CPU |
| --- | ---: | ---: | ---: | ---: |
| Native D3D11 | 2 | 58.34 MiB | 37.49 MiB | 0.097% |
| Electron WebGL | 5 | 323.58 MiB | 130.43 MiB | 0.875% |

本次样本中原生版工作集降低 81.97%，私有内存降低 71.26%，CPU 降低 88.91%。这些数字会随显卡、桌面活动、分辨率和刷新率变化，应使用 `scripts/benchmark-runtime.ps1` 在目标机器复测。

原生 EXE 约 0.31 MiB，便携 ZIP 约 0.16 MiB；对应 Electron ZIP 约 137.6 MiB。

## 编译

需要 Visual Studio 2022 C++ 工具链和 Windows 10/11 SDK：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-native.ps1
```

生成的单文件 x64 程序使用静态 MSVC 运行库，不要求用户安装 VC++ Redistributable。
