#include "pch.h"
#include "AudioPlaybackConnector.h"
#include "ConnectionManager.hpp"
#include "Win11UI.hpp"

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance,
	_In_ PWSTR lpCmdLine, _In_ int nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

	g_hInst = hInstance;
	winrt::init_apartment();
	LoadTranslateData();

	bool supported = false;
	try
	{
		using namespace winrt::Windows::Foundation::Metadata;
		supported = ApiInformation::IsTypePresent(winrt::name_of<DesktopWindowXamlSource>()) &&
			ApiInformation::IsTypePresent(winrt::name_of<AudioPlaybackConnection>());
	}
	catch (winrt::hresult_error const&)
	{
		supported = false;
		LOG_CAUGHT_EXCEPTION();
	}
	if (!supported)
	{
		TaskDialog(nullptr, nullptr, _(L"Unsupported Operating System"), nullptr, _(L"AudioPlaybackConnector is not supported on this operating system version."), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
		return EXIT_FAILURE;
	}

	WNDCLASSEXW wcex = {
		.cbSize = sizeof(wcex),
		.lpfnWndProc = WndProc,
		.hInstance = hInstance,
		.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_AUDIOPLAYBACKCONNECTOR)),
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.lpszClassName = L"AudioPlaybackConnector",
		.hIconSm = wcex.hIcon
	};
	RegisterClassExW(&wcex);

	// When parent window size is 0x0 or invisible, the dpi scale of menu is incorrect.
	// Keep the 1x1 layered invisible window exactly as upstream did.
	g_hWnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST, L"AudioPlaybackConnector", nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
	FAIL_FAST_LAST_ERROR_IF_NULL(g_hWnd);
	FAIL_FAST_IF_WIN32_BOOL_FALSE(SetLayeredWindowAttributes(g_hWnd, 0, 0, LWA_ALPHA));

	DesktopWindowXamlSource desktopSource;
	auto desktopSourceNative2 = desktopSource.as<IDesktopWindowXamlSourceNative2>();
	winrt::check_hresult(desktopSourceNative2->AttachToWindow(g_hWnd));
	winrt::check_hresult(desktopSourceNative2->get_WindowHandle(&g_hWndXaml));

	g_xamlCanvas = Canvas();
	desktopSource.Content(g_xamlCanvas);

	LoadSettings();
	SetupSvgIcon();          // icons must exist before any RefreshPanel call
	SetupFlyout();
	SetupMenu();
	SetupStatusPanel();
	SetupDeviceWatcher();
	SetupRetryTimer();
	RefreshDeviceCatalogAsync();

	g_nid.hWnd = g_niid.hWnd = g_hWnd;
	wcscpy_s(g_nid.szTip, _(L"AudioPlaybackConnector"));
	UpdateNotifyIcon();

	WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
	LOG_LAST_ERROR_IF(WM_TASKBAR_CREATED == 0);

	PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		BOOL processed = FALSE;
		winrt::check_hresult(desktopSourceNative2->PreTranslateMessage(&msg, &processed));
		if (!processed)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}
	return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_DESTROY:
		if (g_retryTimer) g_retryTimer.Stop();
		if (g_deviceWatcher) g_deviceWatcher.Stop();
		CloseAllConnections();
		if (g_reconnect)
		{
			SaveSettings();
			g_audioPlaybackConnections.clear();
		}
		else
		{
			g_audioPlaybackConnections.clear();
			SaveSettings();
		}
		Shell_NotifyIconW(NIM_DELETE, &g_nid);
		PostQuitMessage(0);
		return 0;

	case WM_SETTINGCHANGE:
		if (lParam && CompareStringOrdinal(reinterpret_cast<LPCWCH>(lParam), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL)
			ApplyTheme(true);
		break;

	case WM_REFRESHPANEL:
		RefreshPanel();
		return 0;
	case WM_SHOWPANEL:
		ShowPanel();
		return 0;
	case WM_SHOWDEVICES:
		ShowDevices();
		return 0;
	case WM_RECONNECTALL:
		ReconnectAllAsync();
		return 0;
	case WM_CATALOGCHANGED:
		OnCatalogChangedAsync();
		return 0;
	case WM_CONNECTDEVICE:
		if (g_reconnect)
			RestoreLastDevices();
		return 0;

	case WM_NOTIFYICON:
		switch (LOWORD(lParam))
		{
		case NIN_SELECT:
		case NIN_KEYSELECT:
			PostMessageW(hWnd, WM_SHOWPANEL, 0, 0);
			return 0;
		case WM_CONTEXTMENU:
		{
			POINT pt = { GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam) };
			if (pt.x == -1 && pt.y == -1)
			{
				RECT iconRect;
				if (GetIconRect(iconRect))
				{
					pt.x = iconRect.left;
					pt.y = iconRect.top;
				}
			}
			SetWindowPos(g_hWndXaml, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_SHOWWINDOW);
			SetWindowPos(g_hWnd, HWND_TOPMOST, pt.x, pt.y, 1, 1, SWP_SHOWWINDOW);
			g_xamlCanvas.Width(1);
			g_xamlCanvas.Height(1);
			SetForegroundWindow(hWnd);
			ShowMenu();
			return 0;
		}
		}
		break;

	default:
		if (WM_TASKBAR_CREATED && message == WM_TASKBAR_CREATED)
			UpdateNotifyIcon();
		break;
	}
	return DefWindowProcW(hWnd, message, wParam, lParam);
}

void SetupSvgIcon()
{
	auto hRes = FindResourceW(g_hInst, MAKEINTRESOURCEW(1), L"SVG");
	FAIL_FAST_LAST_ERROR_IF_NULL(hRes);
	auto size = SizeofResource(g_hInst, hRes);
	auto hResData = LoadResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF_NULL(hResData);
	auto svgData = reinterpret_cast<const char*>(LockResource(hResData));
	FAIL_FAST_IF_NULL_ALLOC(svgData);
	const std::string_view svg(svgData, size);
	const int width = GetSystemMetrics(SM_CXSMICON), height = GetSystemMetrics(SM_CYSMICON);

	g_hIconIdleLight = SvgTohIcon(svg, width, height, { 0.0f, 0.0f, 0.0f, 1.0f });
	g_hIconIdleDark = SvgTohIcon(svg, width, height, { 1.0f, 1.0f, 1.0f, 1.0f });
	g_hIconBusy = SvgTohIcon(svg, width, height, { 1.0f, 0.76f, 0.12f, 1.0f });      // amber
	g_hIconConnected = SvgTohIcon(svg, width, height, { 0.10f, 0.78f, 0.36f, 1.0f }); // green
}

void UpdateNotifyIcon()
{
	g_lightTheme = QueryLightTheme();
	HICON icon = nullptr;
	switch (g_iconState)
	{
	case IconState::Busy:
		icon = g_hIconBusy;
		break;
	case IconState::Connected:
		icon = g_hIconConnected;
		break;
	default:
		icon = g_lightTheme ? g_hIconIdleLight : g_hIconIdleDark;
		break;
	}
	if (!icon)
		return; // icons not created yet during early panel setup

	g_nid.hIcon = icon;
	if (!Shell_NotifyIconW(NIM_MODIFY, &g_nid))
	{
		g_nid.uFlags |= NIF_MESSAGE | NIF_TIP;
		Shell_NotifyIconW(NIM_ADD, &g_nid);
	}
	Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}
