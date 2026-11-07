# PopTab Companion — Windows 标题栏置顶图钉

可选的 Windows 小程序。PopTab 扩展继续负责原有标签页迁移；本程序只负责在**系统标题栏**左上角放一个图钉，并使用 Win32 API 切换弹窗系统级置顶。不会往网页里插入按钮，也不注入 Chromium 进程。

## 使用前提

- Windows 10/11，x64。
- Chrome、Edge 或 Brave；已安装同一仓库版本的 PopTab 扩展。
- **最简单：**下载 GitHub Actions 的 `PopTabCompanion-Win64` 构建产物，解压后双击 `Setup.cmd`，无需编译或手动填写扩展 ID。
- 如果自行编译：使用 Windows CMake + Visual Studio 2022 C++ 桌面工具。
- 不需要管理员权限，不需要安装 .NET、WPF 或 WinUI。

## 自动安装（推荐）

1. 下载 [PopTab Windows Companion 最新成功构建包](https://github.com/snownico0722/PopTab/actions/workflows/companion-windows.yml)，解压。
2. 已经装好 **PR #5 的新版 PopTab 扩展**：直接**双击 `Setup.cmd`** 即可。不需要复制扩展 ID、不需要选择浏览器、不需要管理员权限。
3. 如果浏览器里还是旧版 PopTab，先在扩展管理页面打开“开发者模式”，选择“加载已解压的扩展程序”，指定安装包里的 `PopTab-extension` 文件夹。为避免同时运行两个版本，可手动停用旧版扩展；然后双击 `Setup.cmd`。
4. 安装程序会自动查找 Chrome、Edge、Brave 的本机配置文件，识别 PopTab 的扩展 ID，复制原生 EXE 并注册通信。安装后如刚更新扩展，请在扩展管理页重新加载一次 PopTab，打开新弹窗即可使用左上角图钉。

**为什么有一步不能全自动？** Chromium 不允许普通桌面程序静默安装或授权一个新的浏览器扩展。这一步必须由用户在浏览器里完成；其余本机配置可以自动完成。

若找不到 PopTab，安装程序会提示并停止，**不会擅自安装其他扩展或把普通弹窗当成 PopTab**。扫描范围是标准位置的 Chrome、Edge、Brave 用户配置文件；便携版浏览器或自定义 User Data 路径可能需要手动指定扩展 ID。

## 从源码编译（可选）

在仓库根目录运行：

```powershell
cmake -S companion -B companion/build -A x64
cmake --build companion/build --config Release
```

输出位于 `companion/build/Release/PopTabCompanion.exe`，然后直接运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\companion\install.ps1
```

如使用特殊目录导致自动发现失败，仍可用手动兼容命令：

```powershell
powershell -ExecutionPolicy Bypass -File .\companion\install.ps1 -ExtensionId 你的32位扩展ID -Browser Chrome
```

## 操作和状态

- 标题栏左上角系统图标右侧显示图钉，点击一次置顶，再点击取消。
- 图钉是弹窗拥有的原生小窗口：随移动、缩放、最小化和关闭而变化，默认不会抢走网页输入焦点。
- 多个 PopTab 弹窗分别控制。切换上/下一个 PopTab 标签会把旧弹窗的置顶状态传给新弹窗。
- 没装 Companion 或清单/权限不正确时，PopTab 原有功能照常可用；不会弹出错误窗口。
- 不强制开机自启。最后一个 PopTab 弹窗退出约 10 秒后，通信宿主自动结束。
- 浏览器完全重启后，原有 PopTab 回收票据按扩展旧逻辑失效；不要期望恢复此前的图钉状态。

## 实现边界

扩展只发送 **PopTab 确认存在的弹窗 ID 和矩形**，不会传任意 Windows 句柄。Native Messaging 宿主只考虑启动它的浏览器进程中，类名为 `Chrome_WidgetWin_1` 的顶层窗口；根据矩形和前台焦点进行保守匹配。若与其他窗口完全重合且无法区分，**宁可不显示图钉，也不绑定错误窗口**。不依赖网页标题，更不改动网页内容。

Win32 使用 `SetWindowPos(HWND_TOPMOST / HWND_NOTOPMOST)`；按钮由 GDI+ 绘制在有主窗口所有权关系的 `WS_EX_NOACTIVATE` 分层小窗口中。窗口位置由系统事件监听，并每 500 毫秒作一次轻量校正；不高频遍历网页。

此实现需要 Windows 实机检查 Chrome / Edge / Brave 的不同主题、缩放、弹窗标题栏高度，特别是多显示器混合 DPI。编译测试不能替代实机视觉检查。系统管理员权限窗口或浏览器特殊模式可能受到 Windows 权限边界限制。

## 卸载

关闭浏览器后双击解压包中的 **`Uninstall.cmd`**。从源码运行时也可执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\companion\uninstall.ps1 -Browser All
```

这会移除当前用户的 Native Messaging 注册表记录和安装文件。只删除指定浏览器的注册时，保留公共安装文件。
