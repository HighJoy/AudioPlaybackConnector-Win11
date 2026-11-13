# AudioPlaybackConnector
[English](https://github.com/ysc3839/AudioPlaybackConnector/blob/master/README.md) | **简体中文**

Windows 10 2004+ 蓝牙音频接收 (A2DP Sink) 连接工具。

微软在 Windows 10 2004 加入了蓝牙 A2DP Sink 支持。但是需要第三方软件来管理连接。\
已经有了一个可以实现此功能的 app。但是它不可以隐藏到通知区域，而且不开源。\
所以我写了这个 app，提供一个简单的，现代且开源的替代品。

# 预览
![预览](https://cdn.jsdelivr.net/gh/ysc3839/AudioPlaybackConnector@master/AudioPlaybackConnector.gif)

# 使用方法
* 从 [releases](https://github.com/ysc3839/AudioPlaybackConnector/releases) 下载并运行 AudioPlaybackConnector。
* 在系统蓝牙设置中添加蓝牙设备。你可以右键点击通知区域的 AudioPlaybackConnector 图标然后选择“蓝牙设置”。
* 点击 AudioPlaybackConnector 图标然后选择想要连接的设备。
* 尽情享受吧！

# 本分支的改进（Win11 版）
* **修复首次连接没声音**：第一次 `OpenAsync()` 成功后，程序会静默断开并重连一次（相当于你手动连第二下），用于激活 A2DP 音频端点；可在面板中关闭。
* **断线自动重连**：意外断开后按 1s / 2s / 4s … 最长 30s 的退避策略自动重连；可在菜单或面板中开关。
* **一键重连**：托盘右键菜单和连接面板都提供“重新连接”，会关闭并重新打开所有已知设备。
* **启动时恢复**：可选“下次启动时重新连接”，记住上次连接的设备。
* **Windows 11 风格界面**：左键点击托盘图标打开圆角连接面板（设备状态、重连按钮、三个开关）；右键菜单、退出确认浮层改为 Win11 Fluent 风格，跟随系统明暗主题。系统设备选择器通过面板里的“连接新设备”打开。
* 顺带修复了上游从未调用 `LoadTranslateData()` 导致中文/繁中翻译不生效的问题，并补齐新增文案的翻译。
* 设置保存在程序同目录的 `AudioPlaybackConnector.json`，兼容旧配置文件。

# 构建
需要 Visual Studio 2022（含「使用 C++ 的桌面开发」和 Windows 11 SDK）：

```powershell
nuget restore AudioPlaybackConnector.sln
msbuild AudioPlaybackConnector.sln /p:Configuration=Release /p:Platform=x64 /m
```

翻译资源由 `python translate/po2ymo.py` 生成到 `translate/generated/`，生成脚本只依赖 Python 标准库；本仓库的 GitHub Actions 会自动完成还原、生成资源和构建。
