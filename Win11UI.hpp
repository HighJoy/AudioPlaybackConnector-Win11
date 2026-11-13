#pragma once

// Windows 11 styled UI built with the system WinUI 2 (XAML Islands) controls.
// No new NuGet dependency: MenuFlyout / Flyout already render with the Windows 11
// acrylic + rounded chrome; the content below supplies the Fluent layout,
// accent button, card device rows and light/dark theme following.

using namespace winrt::Windows::UI::Xaml::Controls::Primitives;
using namespace winrt::Windows::UI::Xaml::Media;

void UpdateNotifyIcon();
void ShowDevicePicker();
void ShowPanel();

SolidColorBrush MakeBrush(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
{
	return SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(a, r, g, b));
}

bool QueryLightTheme()
{
	DWORD value = 0, cbValue = sizeof(value);
	LOG_IF_WIN32_ERROR(RegGetValueW(HKEY_CURRENT_USER,
		LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)",
		L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &cbValue));
	return value != 0;
}

void ApplyTheme(bool updateIcon)
{
	g_lightTheme = QueryLightTheme();
	if (g_panelRoot)
		g_panelRoot.RequestedTheme(g_lightTheme ? ElementTheme::Light : ElementTheme::Dark);
	if (updateIcon)
		UpdateNotifyIcon();
	RequestPanelRefresh();
}

FontIcon MakeGlyph(const wchar_t* glyph, double size = 15)
{
	FontIcon icon;
	icon.Glyph(glyph);
	icon.FontSize(size);
	return icon;
}

TextBlock MakeTextBlock(const wchar_t* text, double size, bool secondary = false)
{
	TextBlock block;
	block.Text(text);
	block.FontSize(size);
	if (secondary)
		block.Opacity(0.65);
	return block;
}

Button MakeButton(const wchar_t* text, bool accent)
{
	Button button;
	button.Content(winrt::box_value(text));
	button.FontSize(13);
	button.Padding(Thickness{ 14, 7, 14, 7 });
	button.Margin(Thickness{ 0, 0, 8, 0 });
	button.CornerRadius(CornerRadius{ 6, 6, 6, 6 });
	if (accent)
	{
		button.Background(MakeBrush(255, 0, 120, 212));
		button.Foreground(MakeBrush(255, 255, 255, 255));
	}
	return button;
}

ToggleSwitch MakeSwitch(const wchar_t* text)
{
	ToggleSwitch toggle;
	// ToggleSwitch.OnContent accepts any object; a plain string keeps it simple.
	toggle.OnContent(winrt::box_value(text));
	toggle.OffContent(winrt::box_value(text));
	toggle.FontSize(13);
	toggle.Margin(Thickness{ 0, 2, 0, 2 });
	return toggle;
}

UIElement MakeDeviceRow(const std::wstring& name, const std::wstring& status,
	ConnectionPhase phase, const std::wstring& id)
{
	Border row;
	row.CornerRadius(CornerRadius{ 8, 8, 8, 8 });
	row.Padding(Thickness{ 12, 9, 12, 9 });
	row.Margin(Thickness{ 0, 0, 0, 6 });
	row.Background(MakeBrush(g_lightTheme ? 18 : 26, 128, 128, 128));
	row.BorderBrush(MakeBrush(40, 128, 128, 128));
	row.BorderThickness(Thickness{ 1, 1, 1, 1 });

	StackPanel line;
	line.Orientation(Orientation::Horizontal);
	line.VerticalAlignment(VerticalAlignment::Center);

	FontIcon icon = MakeGlyph(L"\xE7F6", 16); // speaker / audio device
	icon.Margin(Thickness{ 0, 0, 10, 0 });
	icon.Foreground(phase == ConnectionPhase::Connected
		? MakeBrush(255, 16, 124, 16)
		: MakeBrush(255, 0, 120, 212));

	TextBlock nameBlock;
	nameBlock.Text(name);
	nameBlock.FontSize(13);
	nameBlock.Width(160);
	nameBlock.TextTrimming(TextTrimming::CharacterEllipsis);
	nameBlock.VerticalAlignment(VerticalAlignment::Center);

	TextBlock statusBlock;
	statusBlock.Text(status);
	statusBlock.FontSize(12);
	statusBlock.Width(88);
	statusBlock.VerticalAlignment(VerticalAlignment::Center);
	statusBlock.Foreground(phase == ConnectionPhase::Failed || phase == ConnectionPhase::Retrying
		? MakeBrush(255, 196, 92, 46)
		: MakeBrush(255, 128, 128, 128));
	statusBlock.Opacity(0.9);

	Button disconnect;
	disconnect.Content(winrt::box_value(_(L"Disconnect")));
	disconnect.FontSize(11);
	disconnect.Padding(Thickness{ 9, 3, 9, 3 });
	disconnect.CornerRadius(CornerRadius{ 5, 5, 5, 5 });
	disconnect.Click([id](auto const&, auto const&) { CloseDevice(id, true); });

	line.Children().Append(icon);
	line.Children().Append(nameBlock);
	line.Children().Append(statusBlock);
	line.Children().Append(disconnect);
	row.Child(line);
	return row;
}

void SetupStatusPanel()
{
	const double panelWidth = 356;

	StackPanel content;
	content.Width(panelWidth);

	// Header
	StackPanel header;
	header.Orientation(Orientation::Horizontal);
	header.Margin(Thickness{ 0, 0, 0, 2 });
	FontIcon headerIcon = MakeGlyph(L"\xE7F6", 20);
	headerIcon.Margin(Thickness{ 0, 0, 10, 0 });
	headerIcon.Foreground(MakeBrush(255, 0, 120, 212));
	header.Children().Append(headerIcon);
	TextBlock title = MakeTextBlock(_(L"Audio connections"), 18);
	header.Children().Append(title);
	content.Children().Append(header);

	TextBlock subtitle = MakeTextBlock(_(L"Windows Bluetooth audio receiver"), 12, true);
	subtitle.Margin(Thickness{ 30, 0, 0, 0 });
	content.Children().Append(subtitle);

	Border divider;
	divider.Height(1);
	divider.Margin(Thickness{ 0, 12, 0, 12 });
	divider.Background(MakeBrush(45, 128, 128, 128));
	content.Children().Append(divider);

	// Device rows
	g_panelDeviceList = StackPanel();
	content.Children().Append(g_panelDeviceList);

	g_panelSummary = MakeTextBlock(L"", 12, true);
	g_panelSummary.Margin(Thickness{ 0, 4, 0, 10 });
	content.Children().Append(g_panelSummary);

	// Actions
	StackPanel actions;
	actions.Orientation(Orientation::Horizontal);
	Button connectButton = MakeButton(_(L"Connect a device"), false);
	connectButton.Click([](auto const&, auto const&) { ShowDevicePicker(); });
	g_panelReconnectButton = MakeButton(_(L"Reconnect"), true);
	g_panelReconnectButton.Click([](auto const&, auto const&) { ReconnectAllAsync(); });
	actions.Children().Append(connectButton);
	actions.Children().Append(g_panelReconnectButton);
	content.Children().Append(actions);

	Border divider2;
	divider2.Height(1);
	divider2.Margin(Thickness{ 0, 12, 0, 8 });
	divider2.Background(MakeBrush(45, 128, 128, 128));
	content.Children().Append(divider2);

	TextBlock settingsTitle = MakeTextBlock(_(L"Options"), 12, true);
	content.Children().Append(settingsTitle);

	g_panelAutoReconnect = MakeSwitch(_(L"Auto reconnect after disconnect"));
	g_panelAutoReconnect.Toggled([](auto const& sender, auto const&) {
		if (g_updatingPanel) return;
		g_autoReconnect = sender.as<ToggleSwitch>().IsOn();
		SaveSettings();
		RequestPanelRefresh();
	});
	content.Children().Append(g_panelAutoReconnect);

	g_panelAutoWarmup = MakeSwitch(_(L"Optimize first connection (fixes silent first connect)"));
	g_panelAutoWarmup.Toggled([](auto const& sender, auto const&) {
		if (g_updatingPanel) return;
		g_autoWarmup = sender.as<ToggleSwitch>().IsOn();
		SaveSettings();
	});
	content.Children().Append(g_panelAutoWarmup);

	g_panelStartReconnect = MakeSwitch(_(L"Reconnect on next start"));
	g_panelStartReconnect.Toggled([](auto const& sender, auto const&) {
		if (g_updatingPanel) return;
		g_reconnect = sender.as<ToggleSwitch>().IsOn();
		SaveSettings();
	});
	content.Children().Append(g_panelStartReconnect);

	g_panelHint = MakeTextBlock(L"", 11, true);
	g_panelHint.Margin(Thickness{ 0, 8, 0, 0 });
	g_panelHint.TextWrapping(TextWrapping::Wrap);
	content.Children().Append(g_panelHint);

	g_panelRoot = Border();
	g_panelRoot.CornerRadius(CornerRadius{ 10, 10, 10, 10 });
	g_panelRoot.Padding(Thickness{ 18, 16, 18, 16 });
	g_panelRoot.Child(content);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Placement(FlyoutPlacementMode::Top);
	flyout.Content(g_panelRoot);
	flyout.Closed([](auto const&, auto const&) { ShowWindow(g_hWnd, SW_HIDE); });
	g_panelFlyout = flyout;

	ApplyTheme(false);
	RefreshPanel();
}

void RefreshPanel()
{
	if (!g_panelDeviceList)
		return;

	g_updatingPanel = true;
	g_panelDeviceList.Children().Clear();

	int connected = 0;
	int retrySeconds = 0;
	for (const auto& kv : g_deviceStates)
	{
		const auto& state = kv.second;
		if (state.phase == ConnectionPhase::Connected)
			++connected;

		if (state.pendingAction == PendingAction::Retry)
		{
			const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
				state.actionAt - std::chrono::steady_clock::now()).count() + 1;
			if (remaining > 0 && (retrySeconds == 0 || remaining < retrySeconds))
				retrySeconds = static_cast<int>(remaining);
		}

		std::wstring name = kv.first;
		if (state.device)
			name = std::wstring(state.device.Name());
		g_panelDeviceList.Children().Append(
			MakeDeviceRow(name, state.status.empty() ? std::wstring(_(L"Disconnected")) : state.status,
				state.phase, kv.first));
	}

	if (g_deviceStates.empty())
	{
		TextBlock empty = MakeTextBlock(_(L"No devices connected yet"), 13, true);
		empty.Margin(Thickness{ 0, 4, 0, 10 });
		g_panelDeviceList.Children().Append(empty);
	}

	g_panelAutoReconnect.IsOn(g_autoReconnect);
	g_panelAutoWarmup.IsOn(g_autoWarmup);
	g_panelStartReconnect.IsOn(g_reconnect);
	g_panelReconnectButton.IsEnabled(!g_deviceStates.empty() || !g_lastDevices.empty());

	g_panelSummary.Text(std::to_wstring(connected) + L" " + _(L"active") + L" / " +
		std::to_wstring(g_deviceStates.size()) + L" " + _(L"known devices"));

	std::wstring hint;
	if (retrySeconds > 0)
		hint = std::wstring(_(L"Next retry in ")) + std::to_wstring(retrySeconds) + L" s";
	if (!g_lastError.empty())
	{
		if (!hint.empty()) hint += L"  |  ";
		hint += g_lastError;
	}
	if (hint.empty())
		hint = _(L"The first connection is re-established once automatically to activate audio.");
	g_panelHint.Text(hint);

	g_updatingPanel = false;
}

void SetupFlyout()
{
	StackPanel stackPanel;
	stackPanel.Width(268);

	TextBlock heading = MakeTextBlock(_(L"Exit AudioPlaybackConnector"), 16);
	heading.Margin(Thickness{ 0, 0, 0, 8 });
	stackPanel.Children().Append(heading);

	TextBlock textBlock;
	textBlock.Text(_(L"All connections will be closed.\nExit anyway?"));
	textBlock.TextWrapping(TextWrapping::Wrap);
	textBlock.FontSize(13);
	textBlock.Margin(Thickness{ 0, 0, 0, 12 });
	stackPanel.Children().Append(textBlock);

	static CheckBox checkbox;
	checkbox.IsChecked(g_reconnect);
	checkbox.Content(winrt::box_value(_(L"Reconnect on next start")));
	stackPanel.Children().Append(checkbox);

	Button button = MakeButton(_(L"Exit"), true);
	button.HorizontalAlignment(HorizontalAlignment::Right);
	button.Margin(Thickness{ 0, 12, 0, 0 });
	button.Click([](auto const&, auto const&) {
		g_reconnect = checkbox.IsChecked().Value();
		PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
	});
	stackPanel.Children().Append(button);

	Border border;
	border.CornerRadius(CornerRadius{ 8, 8, 8, 8 });
	border.Padding(Thickness{ 16, 14, 16, 14 });
	border.Child(stackPanel);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(border);
	g_xamlFlyout = flyout;
}

MenuFlyoutItem MakeMenuItem(const wchar_t* text, const wchar_t* glyph)
{
	MenuFlyoutItem item;
	item.Text(text);
	FontIcon icon = MakeGlyph(glyph, 15);
	item.Icon(icon);
	return item;
}

void SetupMenu()
{
	MenuFlyoutItem panelItem = MakeMenuItem(_(L"Open connection panel"), L"\xE8A7");
	panelItem.Click([](auto const&, auto const&) { PostMessageW(g_hWnd, WM_SHOWPANEL, 0, 0); });

	MenuFlyoutItem connectItem = MakeMenuItem(_(L"Connect a device"), L"\xE710");
	connectItem.Click([](auto const&, auto const&) { PostMessageW(g_hWnd, WM_SHOWPICKER, 0, 0); });

	MenuFlyoutItem reconnectItem = MakeMenuItem(_(L"Reconnect"), L"\xE72C");
	reconnectItem.Click([](auto const&, auto const&) { PostMessageW(g_hWnd, WM_RECONNECTALL, 0, 0); });

	g_menuAutoReconnect = MakeMenuItem(_(L"Auto reconnect"), L"\xE895");
	g_menuAutoReconnect.Click([](auto const&, auto const&) {
		g_autoReconnect = !g_autoReconnect;
		SaveSettings();
	});

	g_menuStartReconnect = MakeMenuItem(_(L"Reconnect on next start"), L"\xE7F6");
	g_menuStartReconnect.Click([](auto const&, auto const&) {
		g_reconnect = !g_reconnect;
		SaveSettings();
	});

	MenuFlyoutItem settingsItem = MakeMenuItem(_(L"Bluetooth Settings"), L"\xE713");
	settingsItem.Click([](auto const&, auto const&) {
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
	});

	MenuFlyoutItem exitItem = MakeMenuItem(_(L"Exit"), L"\xE8BB");
	exitItem.Click([](auto const&, auto const&) {
		if (g_audioPlaybackConnections.empty())
		{
			PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
			return;
		}

		RECT iconRect;
		auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
		if (FAILED(hr))
		{
			LOG_HR(hr);
			return;
		}

		auto dpi = GetDpiForWindow(g_hWnd);
		SetWindowPos(g_hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, 0, 0, SWP_HIDEWINDOW);
		g_xamlCanvas.Width(static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi));
		g_xamlCanvas.Height(static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi));
		g_xamlFlyout.ShowAt(g_xamlCanvas);
	});

	MenuFlyout menu;
	menu.Items().Append(panelItem);
	menu.Items().Append(connectItem);
	menu.Items().Append(MenuFlyoutSeparator());
	menu.Items().Append(reconnectItem);
	menu.Items().Append(g_menuAutoReconnect);
	menu.Items().Append(g_menuStartReconnect);
	menu.Items().Append(MenuFlyoutSeparator());
	menu.Items().Append(settingsItem);
	menu.Items().Append(exitItem);

	menu.Opened([](auto const& sender, auto const&) {
		g_menuAutoReconnect.Text(std::wstring(g_autoReconnect ? L"\x2713  " : L"\x25CB  ") + _(L"Auto reconnect"));
		g_menuStartReconnect.Text(std::wstring(g_reconnect ? L"\x2713  " : L"\x25CB  ") + _(L"Reconnect on next start"));

		auto menuItems = sender.as<MenuFlyout>().Items();
		auto itemsCount = menuItems.Size();
		if (itemsCount > 0)
			menuItems.GetAt(itemsCount - 1).Focus(g_menuFocusState);
		g_menuFocusState = FocusState::Unfocused;
	});
	menu.Closed([](auto const&, auto const&) { ShowWindow(g_hWnd, SW_HIDE); });

	g_xamlMenu = menu;
}

bool GetIconRect(RECT& iconRect)
{
	auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
	if (FAILED(hr))
	{
		LOG_HR(hr);
		return false;
	}
	return true;
}

void ShowPanel()
{
	RECT iconRect;
	if (!GetIconRect(iconRect))
		return;

	auto dpi = GetDpiForWindow(g_hWnd);
	SetWindowPos(g_hWndXaml, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_SHOWWINDOW);
	SetWindowPos(g_hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, 1, 1, SWP_SHOWWINDOW);
	g_xamlCanvas.Width(static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi));
	g_xamlCanvas.Height(static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi));
	SetForegroundWindow(g_hWnd);
	ApplyTheme(false);
	RefreshPanel();
	g_panelFlyout.ShowAt(g_xamlCanvas);
}

void ShowDevicePicker()
{
	using namespace winrt::Windows::UI::Popups;

	RECT iconRect;
	auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
	if (FAILED(hr))
	{
		LOG_HR(hr);
		return;
	}

	auto dpi = GetDpiForWindow(g_hWnd);
	Rect rect = {
		static_cast<float>(iconRect.left * USER_DEFAULT_SCREEN_DPI / dpi),
		static_cast<float>(iconRect.top * USER_DEFAULT_SCREEN_DPI / dpi),
		static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi),
		static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi)
	};

	SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_HIDEWINDOW);
	SetForegroundWindow(g_hWnd);
	g_devicePicker.Show(rect, Placement::Above);
}
