# PopTab Companion — Windows 标题栏置顶图钉

可选的 Windows 小程序。PopTab 扩展继续负责原有标签页迁移；本程序只负责在**系统标题栏**左上角放一个图钉，并使用 Win32 API 切换弹窗系统级置顶。不会往网页里插入按钮，也不注入 Chromium 进程。

## 使用前提

- Windows 10/11，x64。
- Chrome、Edge 或 Brave；已安装同一仓库版本的 PopTab 扩展。
- **最简单：**下载 GitHub Actions 的 `PopTabCompanion-Win64` 构建产物，解压后即可运行 `install.ps1`，无需自行编译。
- 如果自行编译：使用 Windows CMake + Visual Studio 2022 C++ 桌面工具。
- 不需要管理员权限，不需要安装 .NET、WPF 或 WinUI。

## 直接使用构建包（推荐）

1. 打开 [Windows 构建记录](https://github.com/snownico0722/PopTab/actions/workflows/companion-windows.yml)，下载最新成功构建的 `PopTabCompanion-Win64` 压缩包并解压。
2. 打开对应浏览器的扩展管理页，复制 PopTab 的扩展 ID。
3. 在解压目录中执行：
   
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\install.ps1 -ExtensionId 你的32位扩展ID -Browser Chrome
   ```
4. 重新加载 PopTab 扩展，新开的 PopTab 弹窗就会显示原生标题栏图钉。

下方编译和安装方式适合直接从仓库源代码运行。

## 编译

在仓库根目录打开 PowerShell：

```powershell
cmake -S companion -B companion/build -A x64
cmake --build companion/build --config Release
```

输出文件：`companion/build/Release/PopTabCompanion.exe`。

## 安装通信（第一次配置）

1. 在浏览器的 `chrome://extensions` 或 `edge://extensions` 打开开发者模式，记下 **PopTab 扩展 ID**（32 位 a–p 字母）。每次更改解压扩展的安装目录或换浏览器，都可能改变 ID。
2. 在仓库根目录运行（将示例 ID 替换成自己的）：

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\companion\install.ps1 -ExtensionId abcdefghijklmnopabcdefghijklmnop -Browser Chrome
   ```

   也可指定 `-Browser Edge`、`-Browser Brave` 或 `-Browser All`。
3. 安装脚本把 EXE 复制至 `%LOCALAPPDATA%\PopTabCompanion`，生成包含允许扩展 ID 的 Native Messaging 清单，并写入当前用户 HKCU 注册表。它不会修改浏览器程序。
4. **重新加载 PopTab 扩展**，打开一个新的 PopTab 无工具栏弹窗。浏览器在需要时自动启动 Companion（无需手动双击 EXE）。

同一台电脑安装到多个浏览器时，可以重复运行脚本，清单会保留此前的扩展 ID。

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

关闭已打开的浏览器，然后执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\companion\uninstall.ps1 -Browser All
```

这会移除当前用户的 Native Messaging 注册表记录和安装文件。只删除指定浏览器的注册时，保留公共安装文件。
