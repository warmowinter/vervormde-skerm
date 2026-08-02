# Vervormde Skerm Native

这是长期维护的低负载版本，采用 Win32、Desktop Duplication、Direct3D 11 和 HLSL，运行时不依赖 Electron、Node.js 或 WebView。

## 数据路径

`DXGI Desktop Duplication -> D3D11 GPU texture copy -> HLSL radial pre-warp -> DXGI swap chain`

桌面帧不会回读到 CPU，也不会上传。默认限制为 30 FPS；桌面画面没有变化时不会重复执行像素着色和 Present。

## 安全设计

- 启动时默认旁路，用户点击按钮后才显示反畸变覆盖层。
- `Esc` 全局紧急退出；`Ctrl+Shift+Alt+F12` 为备用退出热键。
- 控制面板和系统托盘都提供退出入口。
- 覆盖层始终鼠标穿透，不会阻止操作真实桌面。
- 独立看门狗进程在渲染主循环连续失联时终止主进程，让 Windows 立即移除覆盖层。
- 睡眠、显示模式变化和会话结束时自动旁路。
- 覆盖层与控制面板均从桌面捕获中排除，避免递归反馈。

## 使用

直接运行 `VervormdeSkermNative.exe`。程序启动时处于安全旁路，只显示控制面板：

- 点击“启用反畸变”后才显示桌面覆盖层。
- 使用按钮调节 `k1`、`k2`、缩放、畸变中心和校准网格。
- 默认 30 FPS；可切换 60 FPS，但高分辨率显示器上的 GPU 负载会相应增加。
- `Esc` 紧急退出，`Ctrl+Shift+Alt+F12` 备用退出，`Ctrl+Alt+B` 启用/旁路。
- 参数保存在 `%LOCALAPPDATA%\VervormdeSkerm\settings.ini`，但每次启动仍强制从旁路开始。

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
