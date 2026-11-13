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
constexpr UINT WM_SHOWPICKER = WM_APP + 5;
constexpr UINT WM_RECONNECTALL = WM_APP + 6;

// A single retry / warm-up action is scheduled on the UI-thread DispatcherTimer.
// Keeping every state transition on the UI thread avoids locking the connection map.
enum class ConnectionPhase
{
	Idle,
	Connecting,
	WarmingUp,
	Connected,
	Retrying,
	Failed
};

enum class PendingAction
{
	None,
	Retry,
	WarmupSecondConnect
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
	int warmupAttempts = 0;
	int retryCount = 0;
	std::wstring status;
	DeviceInformation device = nullptr;
};

HINSTANCE g_hInst;
HWND g_hWnd;
HWND g_hWndXaml;
Canvas g_xamlCanvas = nullptr;
Flyout g_xamlFlyout = nullptr;
Flyout g_panelFlyout = nullptr;
MenuFlyout g_xamlMenu = nullptr;
MenuFlyoutItem g_menuAutoReconnect = nullptr;
MenuFlyoutItem g_menuStartReconnect = nullptr;
Border g_panelRoot = nullptr;
StackPanel g_panelDeviceList = nullptr;
TextBlock g_panelSummary = nullptr;
TextBlock g_panelHint = nullptr;
Button g_panelReconnectButton = nullptr;
ToggleSwitch g_panelAutoReconnect = nullptr;
ToggleSwitch g_panelAutoWarmup = nullptr;
ToggleSwitch g_panelStartReconnect = nullptr;
bool g_updatingPanel = false;
FocusState g_menuFocusState = FocusState::Unfocused;
DevicePicker g_devicePicker = nullptr;
std::unordered_map<std::wstring, std::pair<DeviceInformation, AudioPlaybackConnection>> g_audioPlaybackConnections;
std::unordered_map<std::wstring, DeviceState> g_deviceStates;
HICON g_hIconLight = nullptr;
HICON g_hIconDark = nullptr;
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

// Settings (see SettingsUtil.hpp)
bool g_reconnect = false;        // restore last devices on next start
bool g_autoReconnect = true;     // reconnect after an unexpected disconnect
bool g_autoWarmup = true;        // silent second handshake on the first successful connect
int g_maxRetries = 0;            // 0 = keep retrying until stopped manually
bool g_lightTheme = false;
std::vector<std::wstring> g_lastDevices;

// Runtime bookkeeping
uint64_t g_generationCounter = 0;
uint32_t g_timerTicks = 0;
std::wstring g_lastError;
DispatcherTimer g_retryTimer = nullptr;

#include "Util.hpp"
#include "I18n.hpp"
#include "SettingsUtil.hpp"
#include "Direct2DSvg.hpp"
