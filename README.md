# 桌面黑洞

这是一个 Windows 桌面特效：透明覆盖层实时捕获当前显示器，将捕获画面送进 WebGL 着色器，只在黑洞附近重映射屏幕像素。它会扭曲桌面、资源管理器、浏览器、视频等实际显示内容，不是普通网页背景。

## 运行

首次使用：

```powershell
npm.cmd install
npm.cmd start
```

从 GitHub 克隆后先执行上述安装命令。安装完成后，可以执行 `npm.cmd start` 或双击 `start.bat`。

## 反畸变模式

项目也包含可调节的桌面反畸变模式，用于初步补偿弯曲镜面造成的桶形或枕形畸变：

```powershell
npm.cmd run undistort
```

也可以双击 `start-undistort.bat`。该模式带可点击控制面板、安全旁路、托盘退出和渲染失联自动退出保护，详细说明见 [README-undistort.md](README-undistort.md)。

## 快捷键

| 快捷键 | 功能 |
| --- | --- |
| `Ctrl+Alt+B` | 显示或隐藏黑洞 |
| `Ctrl+Alt+F` | 固定位置或恢复跟随鼠标 |
| `Ctrl+Alt+↑ / ↓` | 增强或减弱画面扭曲 |
| `Ctrl+Alt+← / →` | 缩小或放大黑洞 |
| `Ctrl+Alt+M` | 把特效移动到鼠标所在显示器 |
| `Ctrl+Alt+0` | 重置参数 |
| `Ctrl+Alt+H` | 显示快捷键帮助 |
| `Ctrl+Alt+Q` | 完全退出程序 |

覆盖层会忽略全部鼠标点击，因此不会妨碍正常使用桌面。

## 工作原理

- Electron 的桌面捕获 API 获取显示器画面。
- 透明、置顶的 BrowserWindow 只绘制黑洞附近的区域。
- `setContentProtection(true)` 在 Windows 10 2004 及以上把覆盖层排除在捕获之外，避免无限镜像。
- Fragment Shader 根据到事件视界的距离改变 UV，模拟引力透镜；吸积盘、光子环、色差和流动细丝均由 GLSL 实时生成。

## 已知限制

- 当前版本针对 Windows 10 2004 及以上。
- 带 DRM/硬件保护的视频可能在桌面捕获中显示为黑色，这是系统限制。
- 某些全屏独占游戏会绕过桌面合成器；使用无边框窗口模式通常可以显示覆盖层。
- 运行时会持续进行屏幕捕获和 GPU 渲染，笔记本电脑耗电会增加。
