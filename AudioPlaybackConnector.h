#pragma once

#include "resource.h"

using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Audio;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Hosting;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Diagnostic logging.
//
// The final release ships with logging OFF. The logging code stays compiled in
// (it is a no-op when the flag is false), so a diagnostic build only needs this
// single flag flipped back to true - no other source changes required.
// ---------------------------------------------------------------------------
bool g_diagnosticLog = false;
std::mutex g_logMutex;
std::wstring g_logFilePath;

inline std::string Utf8FromWide(std::wstring_view text)
{
	if (text.empty())
		return {};
	const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
		static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	if (size <= 0)
		return {};
	std::string out(static_cast<size_t>(size), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
		out.data(), size, nullptr, nullptr);
	return out;
}

inline std::wstring GetLogFilePath()
{
	wchar_t modulePath[MAX_PATH]{};
	GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
	fs::path exe(modulePath);
	return (exe.parent_path() / L"AudioPlaybackConnector.log").wstring();
}

inline void InitDebugLog()
{
	g_logFilePath = GetLogFilePath();
	// Logging stays OFF in the release build. Putting an empty file named
	// "enable-debug-log" next to the exe switches it on without a rebuild, so a
	// real-world failure can be diagnosed on demand.
	std::error_code ec;
	g_diagnosticLog = fs::exists(fs::path(g_logFilePath).parent_path() / L"enable-debug-log", ec);
	if (!g_diagnosticLog)
		return;
	HANDLE file = CreateFileW(g_logFilePath.c_str(), GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
	{
		// The exe may live in a read-only folder (e.g. Program Files).
		wchar_t tempPath[MAX_PATH]{};
		GetTempPathW(MAX_PATH, tempPath);
		g_logFilePath = std::wstring(tempPath) + L"AudioPlaybackConnector.log";
		file = CreateFileW(g_logFilePath.c_str(), GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return;
	}
	static const char banner[] =
		"AudioPlaybackConnector diagnostic log (UTF-8)\r\n"
		"format: [MM-DD hh:mm:ss.mmm] [tag] message\r\n"
		"tags: START WATCHER STATE PROBE RETRY TIMER OPEN CONNECT PANEL\r\n";
	DWORD written = 0;
	WriteFile(file, banner, static_cast<DWORD>(sizeof(banner) - 1), &written, nullptr);
	CloseHandle(file);
}

inline void LogW(const char* tag, std::wstring const& message)
{
	if (!g_diagnosticLog || g_logFilePath.empty())
		return;
	std::lock_guard<std::mutex> lock(g_logMutex);
	SYSTEMTIME time{};
	GetLocalTime(&time);
	char stamp[96];
	sprintf_s(stamp, "[%02u-%02u %02u:%02u:%02u.%03u] [%-13s] ",
		time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
		time.wMilliseconds, tag);
	std::string line(stamp);
	line += Utf8FromWide(message);
	line += "\r\n";
	HANDLE file = CreateFileW(g_logFilePath.c_str(), FILE_APPEND_DATA,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return;
	SetFilePointer(file, 0, nullptr, FILE_END);
	DWORD written = 0;
	WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
	CloseHandle(file);
}

constexpr UINT WM_NOTIFYICON = WM_APP + 1;
constexpr UINT WM_CONNECTDEVICE = WM_APP + 2;
constexpr UINT WM_REFRESHPANEL = WM_APP + 3;
constexpr UINT WM_SHOWPANEL = WM_APP + 4;
constexpr UINT WM_SHOWDEVICES = WM_APP + 5;
constexpr UINT WM_RECONNECTALL = WM_APP + 6;
constexpr UINT WM_CATALOGCHANGED = WM_APP + 7;
constexpr UINT WM_CLOSEPANEL = WM_APP + 8;
constexpr UINT WM_STYLEPOPUP = WM_APP + 9;
constexpr UINT POPUP_STYLE_TIMER = 4711;

enum class ConnectionPhase
{
	Idle,
	Connecting,
	WarmingUp,
	Connected,
	Retrying,
	Failed,
	Unavailable
};

enum class PendingAction
{
	None,
	Retry,
	WarmupSecondConnect,
	WarmupReopen,
	ReappearProbe
};

enum class IconState
{
	Idle,
	Busy,
	Connected
};

struct DeviceState
{
	uint64_t generation = 0;
	ConnectionPhase phase = ConnectionPhase::Idle;
	PendingAction pendingAction = PendingAction::None;
	std::chrono::steady_clock::time_point actionAt{};
	bool manualDisconnect = false;
	bool suppressClosed = false;
	bool warmupDone = false;
	bool everConnected = false;
	bool waitingForDevice = false;
	bool offline = false;
	bool probing = false;
	int warmupAttempts = 0;
	// Delay between the warm-up Close() and the reopen. 0 = immediate (required
	// for the FIRST connection); 1500 = wait (required after the radio was off).
	int warmupReopenDelayMs = 0;
	int retryCount = 0;
	std::wstring status;
	DeviceInformation device = nullptr;
};

HINSTANCE g_hInst;
HWND g_hWnd;
HWND g_hWndXaml;
Canvas g_xamlCanvas = nullptr;
Flyout g_xamlFlyout = nullptr;      // exit confirmation (kept for compatibility)
Flyout g_panelFlyout = nullptr;     // main connection panel
Flyout g_menuFlyout = nullptr;      // self-drawn rounded tray menu

// HWND of the XAML flyout host, discovered after ShowAt so we can round its
// real window frame (not just draw a rounded border inside it).
HWND g_popupHwnd = nullptr;
HHOOK g_mouseHook = nullptr;
bool g_panelOpen = false;
bool g_menuOpen = false;

Border g_panelRoot = nullptr;
StackPanel g_panelDeviceList = nullptr;
StackPanel g_panelCatalogList = nullptr;
TextBlock g_panelSummary = nullptr;
TextBlock g_panelHint = nullptr;
Button g_panelReconnectButton = nullptr;
ToggleSwitch g_panelAutoReconnect = nullptr;
ToggleSwitch g_panelAutoWarmup = nullptr;
ToggleSwitch g_panelStartReconnect = nullptr;
bool g_updatingPanel = false;

// Device presence is decided ONLY by a real-time probe performed right before
// every reconnect attempt. FindAllAsync(GetDeviceSelector()) returns PAIRED
// devices even when their Bluetooth is switched off, so its result must never
// be used as proof that a device is reachable.
//
// A device that is reported Removed by the watcher, or whose Open() timed out,
// is put in g_blockedDeviceIds. A blocked device is never retried until the
// watcher reports an explicit Added event for it.
DeviceWatcher g_deviceWatcher = nullptr;
std::unordered_set<std::wstring> g_visibleDeviceIds;
std::unordered_set<std::wstring> g_blockedDeviceIds;
std::mutex g_watcherMutex;
std::vector<std::pair<std::wstring, bool>> g_watcherEvents;
std::vector<DeviceInformation> g_availableDevices;
uint64_t g_catalogGeneration = 0;

std::unordered_map<std::wstring, std::pair<DeviceInformation, AudioPlaybackConnection>> g_audioPlaybackConnections;
std::unordered_map<std::wstring, DeviceState> g_deviceStates;

// Tray icon variants: idle = original monochrome, busy = amber, connected = green.
HICON g_hIconIdleLight = nullptr;
HICON g_hIconIdleDark = nullptr;
HICON g_hIconBusy = nullptr;
HICON g_hIconConnected = nullptr;
IconState g_iconState = IconState::Idle;

NOTIFYICONDATAW g_nid = {
	.cbSize = sizeof(g_nid),
	.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP,
	.uCallbackMessage = WM_NOTIFYICON,
	.uVersion = NOTIFYICON_VERSION_4
};
NOTIFYICONIDENTIFIER g_niid = {
	.cbSize = sizeof(g_niid)
};
UINT WM_TASKBAR_CREATED = 0;

bool g_reconnect = false;
bool g_autoReconnect = true;
bool g_autoWarmup = true;
int g_maxRetries = 0;
bool g_lightTheme = false;
std::vector<std::wstring> g_lastDevices;

uint64_t g_generationCounter = 0;
uint32_t g_timerTicks = 0;
std::wstring g_lastError;
DispatcherTimer g_retryTimer = nullptr;

#include "Util.hpp"
#include "I18n.hpp"
#include "SettingsUtil.hpp"
#include "Direct2DSvg.hpp"
