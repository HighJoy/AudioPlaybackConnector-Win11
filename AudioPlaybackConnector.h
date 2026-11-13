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
	WarmupSecondConnect
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
	int warmupAttempts = 0;
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

// Device presence driven by the DeviceWatcher. FindAllAsync returns paired
// devices even when they are powered off, so it must NOT be used to decide
// whether a reconnect is allowed.
DeviceWatcher g_deviceWatcher = nullptr;
std::unordered_set<std::wstring> g_visibleDeviceIds;
std::mutex g_watcherMutex;
std::vector<std::pair<std::wstring, bool>> g_watcherEvents;
bool g_catalogInitialized = false;
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
