# PopTab Companion — Windows 标题栏置顶图钉

可选的 Windows 小程序。PopTab 扩展继续负责原有标签页迁移；本程序只负责在**系统标题栏**左上角放一个图钉，并使用 Win32 API 切换弹窗系统级置顶。不会往网页里插入按钮，也不注入 Chromium 进程。

## 一个 EXE，双击安装

1. 在 [GitHub Actions 的最新成功构建](https://github.com/snownico0722/PopTab/actions/workflows/companion-windows.yml) 下载 **PopTabCompanion-OneFile-Win64**，解压其中的 `PopTabCompanion.exe`。这是**唯一需要下载的文件**。
2. **双击 `PopTabCompanion.exe`**。Windows 传统界面会显示安装结果；不需要管理员权限、手动输入扩展 ID、PowerShell 或额外运行时。
3. 如果浏览器已装好 PR #5 的新版 PopTab，程序自动识别 Chrome、Edge、Brave 下的 PopTab，复制自身到 `%LOCALAPPDATA%\PopTabCompanion`，创建 Native Messaging 清单并注册到当前用户注册表。此时打开 PopTab 弹窗即可显示标题栏图钉。
4. 如果检测不到扩展，或仅有旧版扩展：程序会把最新版 PopTab 扩展**从 EXE 自身解压**到 `%LOCALAPPDATA%\PopTabCompanion\PopTab-extension`，并引导打开文件夹。请在浏览器扩展页面打开“开发者模式”→“加载已解压的扩展程序”，选择这个固定路径。为避免重复运行两个扩展，可手动关闭旧版；然后再双击同一个 EXE 完成自动绑定。

**重要边界：** Chrome/Edge/Brave 不允许桌面应用静默安装或批准新的浏览器扩展。浏览器尚未安装/授权时，需要用户自己完成扩展加载这一步；本地辅助程序和通信注册都能自动完成。

这个 EXE 是**双模式程序**：用户双击是安装器，浏览器通过 Native Messaging 启动时自动变成后台宿主。无需另外运行服务，也不会再打开安装窗口。浏览器完全重启后仍可正常使用已注册的宿主。

## 开发者：从源码构建

```powershell
cmake -S companion -B companion/build -A x64
cmake --build companion/build --config Release
```

编译生成 `companion/build/Release/PopTabCompanion.exe`，里面已经嵌入最新版浏览器扩展所需的 JS、manifest 和图标资源。可直接复制该 EXE 到其他 Windows 10/11 x64 机器，双击完成安装。

仓库里的旧 `install.ps1`、`Setup.cmd` 等保留作调试和手动安装入口，不再是发布和正常使用的必需文件。

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

当前仍保留仓库里的 `companion/uninstall.ps1`（从仓库运行），它会移除当前用户的浏览器通信注册与安装目录。后续如需要，也可给同一 EXE 增加图形化卸载入口。正常使用**不需要任何脚本**。
