# AudioPlaybackConnector (Windows 11 edition)

**English** | [简体中文](README.zh_CN.md)

Bluetooth audio playback (A2DP Sink) connector for Windows 10 2004+ / Windows 11.

This is a fork of [ysc3839/AudioPlaybackConnector](https://github.com/ysc3839/AudioPlaybackConnector) focused on
reconnect reliability, a real Windows 11 interface, and the long-standing "connected but no sound" problem.

> Upstream credit: the original app, its WinRT plumbing and the Direct2D tray icon renderer are by Richard Yu (MIT).

# Download

Grab `AudioPlaybackConnector-Win11-x64.exe` from [Releases](../../releases/latest). No installer, no dependencies —
it is a single portable executable. Requires Windows 10 2004 (build 19041) or newer, x64.

# Usage

1. Run `AudioPlaybackConnector-Win11-x64.exe`. It lives in the notification area; there is no main window.
2. Pair your phone in **Settings → Bluetooth & devices** (or right click the tray icon → *Bluetooth Settings*).
3. **Left click** the tray icon to open the connection panel, then press *Connect* next to your device.
   *Scan devices* re-enumerates, but the list also refreshes itself when devices appear or disappear.
4. **Right click** the tray icon for a two-item menu: *Bluetooth Settings* and *Exit*.

Clicking outside the panel, clicking the tray icon again, or the ✕ button in the top-right corner closes it.

## Tray icon states

| Icon | Meaning |
| --- | --- |
| Monochrome | No connection |
| Amber | Connecting / retrying |
| Green | Connected |

# What this fork changes

## 1. Fixes "connected but no sound" on the first connection

Windows often reports the A2DP connection as established while the audio endpoint is not streaming yet. The manual
workaround is "disconnect and connect again" — this fork automates exactly that: 1.2 s after the first successful
`OpenAsync()` the connection is closed and immediately reopened once.

The same handshake is re-applied when the connection comes back after the phone's Bluetooth radio was switched off,
because that is a complete Bluetooth reconnection and leaves the endpoint in the same inactive state.

*Can be turned off* with *Optimize first connection* in the panel.

## 2. Automatic reconnect — only while the device is actually there

* Unexpected disconnects retry with 1 s / 2 s / 4 s / 8 s / 16 s / 30 s backoff (30 s is the cap).
* An `OpenAsync()` that returns `RequestTimedOut`, `UnknownFailure` or the "device is not functioning" HRESULT
  family (`0x8007001F`) means the device is unreachable. The backoff stops immediately instead of hammering a
  powered-off phone, and the panel says so.
* While unreachable, a single **silent probe** runs every 30 s (and immediately when you open the panel). If the
  phone comes back it is reconnected automatically; if not, the probe just waits another 30 s. Nothing is shown as a
  countdown while the device is gone.
* Toggle: *Auto reconnect after disconnect* in the panel.
* `AudioPlaybackConnection` does not expose whether audio is actually flowing, so the only detectable signal is the
  connection state. A rare silent connection still needs the *Reconnect* button.

## 3. Manual reconnect

* The panel has a *Reconnect* button per device, plus the tray menu keeps a one-click reconnect for all known devices.
* *Reconnect on next start* remembers the last devices and restores them when the app starts.

## 4. Windows 11 interface

* The tray panel is a rounded, DWM-corrected flyout using the Windows 11 dark palette (`#202020` surface,
  `#2C2C2C` cards, `#3D3D3D` hairlines). The system `FlyoutPresenter` background/border is taken over so the window
  frame and the body are the same colour — no light frame around a dark body.
* Two grouped sections with headings: **Connected devices** and **Available devices**. Each row is a 3-column grid
  (glyph / name + status / action button), so headings, names, status lines and buttons line up between both
  sections. Already-connected devices are not repeated in the available list.
* The system device picker is gone — everything happens inside this panel.
* Per-row close button is at the top-right corner of the panel.

## 5. Other fixes

* Upstream never called `LoadTranslateData()`, so the bundled translations were dead code. Fixed, and all new strings
  are translated (zh_CN / zh_TW).
* `translate/po2ymo.py` was rewritten to use only the Python standard library, so CI no longer downloads a fork of
  translate-toolkit.
* Settings are written to `AudioPlaybackConnector.json` next to the executable and stay compatible with old files.

# Settings

`AudioPlaybackConnector.json`, created next to the executable:

| Key | Meaning |
| --- | --- |
| `autoReconnect` | Reconnect automatically after an unexpected disconnect |
| `autoWarmup` | Optimize the first connection (second handshake) |
| `reconnect` | Restore the last devices on the next start |
| `maxRetries` | Upper bound for consecutive automatic retries (0 = unlimited) |
| `lastDevices` | Device ids restored on start when `reconnect` is on |

# Diagnostics

The release build does not write logs. To enable them, create an **empty file named `enable-debug-log`** next to the
executable and restart the app. A UTF-8 `AudioPlaybackConnector.log` is written to the same folder (falls back to
`%TEMP%` if that folder is read-only); the panel shows the actual path. The log is reset on every start and records
watcher events, presence probes, retry scheduling, `OpenAsync` status codes and timings, and the warm-up handshake.

# Build

Requires Visual Studio 2022 with the "Desktop development with C++" workload and a Windows 11 SDK:

```powershell
nuget restore AudioPlaybackConnector.sln
msbuild AudioPlaybackConnector.sln /p:Configuration=Release /p:Platform=x64 /m
```

Translation resources are generated by `python translate/po2ymo.py` (standard library only) into `translate/generated/`.
The GitHub Actions workflow restores packages, generates those resources and builds x64 Release, uploading the
executable as a workflow artifact. Releases are published manually, not per commit.

# License

MIT, same as upstream. See [LICENSE](LICENSE).
