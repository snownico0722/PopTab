# PopTab Companion v0.2.1

Windows 10/11 x64 单文件原生置顶辅助程序（`PopTabCompanion.exe`）。

本版改进了 PopTab 弹窗左上角标题栏图钉的位置，并修复第一次点击置顶后图钉暂时消失、需要切换焦点才能重新出现的问题。新增 Windows 托盘菜单：**开机自启动（首次完整安装默认开启）**、**打开文件位置**和**退出**。

安装器现在仅在检测到兼容的 PopTab 浏览器扩展并完成 Native Messaging 注册后，才启用托盘和开机自启动。托盘退出时如置顶宿主未能正常关闭，会保留菜单并提示重试；已安装但托盘退出后，重新双击 EXE 可选择 **启动托盘**，无需重复安装。

**使用方法：** 下载 `PopTabCompanion.exe`，双击安装／修复即可。首次加载或升级浏览器扩展时，仍需在 Chrome、Edge 或 Brave 的扩展页面确认。无需管理员权限或单独安装 VC++ 运行库。

[源码与说明](https://github.com/snownico0722/PopTab/tree/main/companion)

验证：Windows CI、原生通信、安装／退出／自启动行为、真实 Chromium 弹窗及图钉点击回归测试。不同浏览器主题和混合 DPI 的最终视觉位置仍建议在本机确认。
