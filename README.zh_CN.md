# AudioPlaybackConnector（Windows 11 版）

[English](README.md) | **简体中文**

Windows 10 2004+ / Windows 11 的蓝牙音频接收（A2DP Sink）连接工具。

这是 [ysc3839/AudioPlaybackConnector](https://github.com/ysc3839/AudioPlaybackConnector) 的分支，重点解决
**重连可靠性**、**Windows 11 界面**，以及长期存在的**「显示已连接但没声音」**问题。

> 上游版权说明：原始程序、WinRT 框架和 Direct2D 托盘图标渲染均来自 Richard Yu（MIT 许可）。

# 下载

从 [Releases](../../releases/latest) 下载 `AudioPlaybackConnector-Win11-x64.exe`。免安装、无运行库依赖，单个可执行文件。
需要 Windows 10 2004（19041）或更高版本，x64。

# 使用方法

1. 运行 `AudioPlaybackConnector-Win11-x64.exe`。程序常驻通知区域，没有主窗口。
2. 在**设置 → 蓝牙和其他设备**里配对手机（也可以右键托盘图标 →「蓝牙设置」）。
3. **左键单击**托盘图标打开连接面板，点设备旁的「连接」。面板里的「扫描设备」可以重新枚举，设备出现或消失时列表也会自动刷新。
4. **右键单击**托盘图标只有两项：**蓝牙设置**、**退出**。

点击面板外部、再次点击托盘图标，或点右上角的 ✕，都会关闭面板。

## 托盘图标三态

| 图标 | 含义 |
| --- | --- |
| 单色 | 未连接 |
| 黄色 | 连接中 / 重试中 |
| 绿色 | 已连接 |

# 本分支改了什么

## 1. 修复首次连接「显示已连接但没声音」

Windows 经常在 A2DP 连接建立后就报告成功，但音频端点还没开始推流。手动解决办法是「断开再连一次」——
本分支把这一步自动化了：首次 `OpenAsync()` 成功后等 **1.2 秒**，静默断开并**立即**重开一次。

手机蓝牙关掉后再恢复时也会重做同样的握手，因为那是一次完整的蓝牙重连，端点同样处于未激活状态。

面板里的「优化首次连接」可以关掉这个行为。

## 2. 自动重连 —— 只在设备存在时重连

* 意外断开后按 1s / 2s / 4s / 8s / 16s / 30s 退避重试（30 秒封顶）。
* `OpenAsync()` 返回 `RequestTimedOut`、`UnknownFailure` 或「连到系统上的设备没有发挥作用」这一类
  （`0x8007001F`）时，判定设备不可达：**立刻停止退避重连**，不再对着已经关机的手机反复发请求，面板会明确提示。
* 设备不可达期间，每 30 秒做一次**静默探测**（打开面板时也会立即探测一次）。探测成功就自动接回，失败就再等 30 秒。
  这段时间界面不显示倒计时。
* 开关：面板里的「断开后自动重连」。
* `AudioPlaybackConnection` 不暴露音频是否真的在播，程序只能拿到连接状态，所以偶发的无声仍需要用「重新连接」按钮。

## 3. 手动重连

* 面板里每台设备都有「重新连接」，「重新连接」按钮也会一次性重连所有已知设备。
* 「下次启动时重新连接」会记住上次连接的设备，启动后自动恢复。

## 4. Windows 11 界面

* 托盘面板是带 DWM 圆角的弹出窗口，使用 Windows 11 深色配色（面板 `#202020`、卡片 `#2C2C2C`、细线 `#3D3D3D`）。
  系统 `FlyoutPresenter` 的背景和边框也被接管，所以**窗口边框与主体同色**，不会再出现「浅色边框套深色主体」。
* 两个带标题的分区：**已连接的设备** 和 **可用设备**。每行是 3 列网格（图标 / 名称+状态 / 操作按钮），
  两个分区的标题、设备名、状态文字、按钮都逐列对齐；已连接的设备不会在可用设备区重复出现。
* 系统设备选择器已移除，所有操作都在这个面板里完成。
* 面板关闭按钮在右上角。

## 5. 其他修复

* 上游从未调用 `LoadTranslateData()`，导致自带翻译是死代码。已修复，并为所有新增文案补齐简/繁中文。
* `translate/po2ymo.py` 改写为只依赖 Python 标准库，CI 不再需要下载 translate-toolkit 的第三方分支。
* 设置保存在程序同目录的 `AudioPlaybackConnector.json`，兼容旧的配置文件。

# 设置项

程序同目录的 `AudioPlaybackConnector.json`：

| 键 | 含义 |
| --- | --- |
| `autoReconnect` | 意外断开后自动重连 |
| `autoWarmup` | 优化首次连接（二次握手） |
| `reconnect` | 下次启动时恢复上次连接的设备 |
| `maxRetries` | 连续自动重试的上限（0 表示不限制） |
| `lastDevices` | `reconnect` 开启时用于启动恢复的设备 id |

# 诊断日志

正式版默认不写日志。需要时在 exe 同目录放一个**空文件 `enable-debug-log`** 再启动，就会在该目录生成 UTF-8 的
`AudioPlaybackConnector.log`（目录不可写时回退到 `%TEMP%`，面板会显示实际路径）。日志每次启动清空重来，
记录设备监听事件、存在性探测、重试排队、`OpenAsync` 状态码与耗时、以及二次握手过程。

# 构建

需要 Visual Studio 2022（含「使用 C++ 的桌面开发」和 Windows 11 SDK）：

```powershell
nuget restore AudioPlaybackConnector.sln
msbuild AudioPlaybackConnector.sln /p:Configuration=Release /p:Platform=x64 /m
```

翻译资源由 `python translate/po2ymo.py` 生成到 `translate/generated/`（只依赖标准库）。
本仓库的 GitHub Actions 会还原依赖、生成资源并构建 x64 Release，产物作为工作流 artifact 上传；
发行版为手动发布，不会每次提交都生成 Release。

# 许可证

MIT，与上游一致。见 [LICENSE](LICENSE)。
