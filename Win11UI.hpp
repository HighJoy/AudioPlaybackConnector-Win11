#pragma once

// Windows 11 styled UI. Everything is self-drawn with system WinUI 2 (XAML
// Islands) controls: the tray menu, the exit flyout and the device catalog all
// live in rounded Fluent cards. The old system DevicePicker is never shown.

using namespace winrt::Windows::UI::Xaml::Controls::Primitives;
using namespace winrt::Windows::UI::Xaml::Media;

void UpdateNotifyIcon();
void ShowPanel();
void InstallMouseHook();
void UninstallMouseHook();

SolidColorBrush MakeBrush(int a, int r, int g, int b)
{
	return SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(
		static_cast<uint8_t>(a), static_cast<uint8_t>(r),
		static_cast<uint8_t>(g), static_cast<uint8_t>(b)));
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
	{
		g_panelRoot.RequestedTheme(g_lightTheme ? ElementTheme::Light : ElementTheme::Dark);
		g_panelRoot.Background(MakeBrush(g_lightTheme ? 248 : 38,
			g_lightTheme ? 255 : 32, g_lightTheme ? 255 : 32, g_lightTheme ? 255 : 32));
	}
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
	button.Padding(Thickness{ 14, 8, 14, 8 });
	button.Margin(Thickness{ 0, 0, 8, 0 });
	button.CornerRadius(CornerRadius{ 8, 8, 8, 8 });
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
	toggle.OnContent(winrt::box_value(text));
	toggle.OffContent(winrt::box_value(text));
	toggle.FontSize(13);
	toggle.Margin(Thickness{ 0, 2, 0, 2 });
	return toggle;
}

// The flyout HOST window is rounded by DWM/SetWindowRgn (ApplyPopupCorners).
// Cards therefore need no stroke and no extra rounded frame - the user asked
// to remove the border that was added while the window itself was still square.
Border MakeCard(CornerRadius radius, int fillAlpha)
{
	Border card;
	card.CornerRadius(radius);
	card.Background(MakeBrush(fillAlpha, g_lightTheme ? 255 : 32, g_lightTheme ? 255 : 32, g_lightTheme ? 255 : 32));
	return card;
}

Button MakeSmallButton(const wchar_t* text, bool accent)
{
	Button button;
	button.Content(winrt::box_value(text));
	button.FontSize(11);
	button.Padding(Thickness{ 8, 4, 8, 4 });
	button.MinWidth(60);
	button.HorizontalAlignment(HorizontalAlignment::Right);
	button.VerticalAlignment(VerticalAlignment::Center);
	button.HorizontalContentAlignment(HorizontalAlignment::Center);
	button.CornerRadius(CornerRadius{ 7, 7, 7, 7 });
	if (accent)
	{
		button.Background(MakeBrush(255, 0, 120, 212));
		button.Foreground(MakeBrush(255, 255, 255, 255));
	}
	return button;
}

winrt::fire_and_forget ConnectCatalogDeviceAsync(std::wstring id)
{
	auto device = FindDeviceInCatalog(id);
	if (device)
		co_await ConnectDeviceCoreAsync(device);
	else
	{
		g_lastError = _(L"Device not found");
		RequestPanelRefresh();
	}
}

// A visible rectangular group so the two device lists read as distinct
// sections. The host window is already rounded by DWM; these are INNER group
// cards, not another frame around the whole panel.
Border MakeSection(const wchar_t* title, StackPanel list)
{
	// Neutral translucent tones work in both light and dark themes and do not
	// need to be rebuilt when the system theme changes.
	Border section;
	section.CornerRadius(CornerRadius{ 10, 10, 10, 10 });
	section.BorderBrush(MakeBrush(90, 128, 128, 128));
	section.BorderThickness(Thickness{ 1, 1, 1, 1 });
	section.Background(MakeBrush(18, 128, 128, 128));
	section.Padding(Thickness{ 12, 10, 12, 10 });
	section.Margin(Thickness{ 0, 0, 0, 10 });

	StackPanel box;
	TextBlock heading = MakeTextBlock(title, 13);
	heading.Margin(Thickness{ 0, 0, 0, 8 });
	box.Children().Append(heading);
	box.Children().Append(list);
	section.Child(box);
	return section;
}

UIElement MakeDeviceRow(const std::wstring& name, const std::wstring& status,
	ConnectionPhase phase, const std::wstring& id, bool catalogRow)
{
	Border row = MakeCard(CornerRadius{ 10, 10, 10, 10 }, g_lightTheme ? 14 : 24);
	row.Padding(Thickness{ 12, 9, 12, 9 });
	row.Margin(Thickness{ 0, 0, 0, 6 });

	// Two columns: a vertical stack (name on the first line, status on its own
	// second line) on the left, and a compact action button on the right.
	// Putting the status on a separate line is what prevents long messages such
	// as "device unavailable" from overlapping the button.
	Grid line;
	ColumnDefinition leftColumn;
	leftColumn.Width(GridLength{ 1.0, GridUnitType::Star });
	ColumnDefinition rightColumn;
	rightColumn.Width(GridLength{ 0.0, GridUnitType::Auto });
	line.ColumnDefinitions().Append(leftColumn);
	line.ColumnDefinitions().Append(rightColumn);

	StackPanel info;
	info.VerticalAlignment(VerticalAlignment::Center);

	StackPanel nameLine;
	nameLine.Orientation(Orientation::Horizontal);
	nameLine.VerticalAlignment(VerticalAlignment::Center);

	FontIcon icon = MakeGlyph(catalogRow ? L"\xE710" : L"\xE7F6", 16);
	icon.Margin(Thickness{ 0, 0, 10, 0 });
	icon.Foreground(phase == ConnectionPhase::Connected
		? MakeBrush(255, 16, 150, 72)
		: (phase == ConnectionPhase::Unavailable
			? MakeBrush(255, 150, 150, 150)
			: MakeBrush(255, 0, 120, 212)));

	TextBlock nameBlock;
	nameBlock.Text(name);
	nameBlock.FontSize(13);
	nameBlock.MaxWidth(176);
	nameBlock.TextTrimming(TextTrimming::CharacterEllipsis);
	nameBlock.VerticalAlignment(VerticalAlignment::Center);
	nameLine.Children().Append(icon);
	nameLine.Children().Append(nameBlock);
	info.Children().Append(nameLine);

	if (!catalogRow)
	{
		TextBlock statusBlock;
		statusBlock.Text(status);
		statusBlock.FontSize(11);
		statusBlock.MaxWidth(176);
		statusBlock.Margin(Thickness{ 26, 2, 6, 0 });
		statusBlock.TextWrapping(TextWrapping::Wrap);
		statusBlock.Foreground(phase == ConnectionPhase::Failed ||
			phase == ConnectionPhase::Retrying || phase == ConnectionPhase::Unavailable
			? MakeBrush(255, 210, 150, 30)
			: MakeBrush(255, 128, 128, 128));
		statusBlock.Opacity(0.9);
		info.Children().Append(statusBlock);
	}

	const bool active = phase == ConnectionPhase::Connected || phase == ConnectionPhase::Connecting ||
		phase == ConnectionPhase::WarmingUp || phase == ConnectionPhase::Retrying;
	Button action = MakeSmallButton(active && !catalogRow ? _(L"Disconnect") : _(L"Connect"),
		catalogRow || !active);
	if (catalogRow || !active)
	{
		action.Click([id](auto const&, auto const&) { ConnectCatalogDeviceAsync(id); });
	}
	else
	{
		action.Click([id](auto const&, auto const&) { CloseDevice(id, true); });
	}

	Grid::SetColumn(info, 0);
	Grid::SetColumn(action, 1);
	line.Children().Append(info);
	line.Children().Append(action);
	row.Child(line);
	return row;
}

void SetupStatusPanel()
{
	const double panelWidth = 360;

	StackPanel content;
	content.Width(panelWidth);

	Grid header;
	ColumnDefinition titleColumn;
	titleColumn.Width(GridLength{ 1.0, GridUnitType::Star });
	ColumnDefinition closeColumn;
	closeColumn.Width(GridLength{ 0.0, GridUnitType::Auto });
	header.ColumnDefinitions().Append(titleColumn);
	header.ColumnDefinitions().Append(closeColumn);
	header.Margin(Thickness{ 0, 0, 0, 2 });

	StackPanel titleRow;
	titleRow.Orientation(Orientation::Horizontal);
	titleRow.VerticalAlignment(VerticalAlignment::Center);
	FontIcon headerIcon = MakeGlyph(L"\xE7F6", 20);
	headerIcon.Margin(Thickness{ 0, 0, 10, 0 });
	headerIcon.Foreground(MakeBrush(255, 0, 120, 212));
	titleRow.Children().Append(headerIcon);
	TextBlock headerTitle = MakeTextBlock(_(L"Audio connections"), 18);
	headerTitle.VerticalAlignment(VerticalAlignment::Center);
	titleRow.Children().Append(headerTitle);
	Grid::SetColumn(titleRow, 0);

	Button closeButton;
	closeButton.Content(MakeGlyph(L"\xE8BB", 12));
	closeButton.FontSize(12);
	closeButton.Padding(Thickness{ 9, 6, 9, 6 });
	closeButton.CornerRadius(CornerRadius{ 8, 8, 8, 8 });
	closeButton.Background(MakeBrush(0, 0, 0, 0));
	closeButton.HorizontalAlignment(HorizontalAlignment::Right);
	closeButton.VerticalAlignment(VerticalAlignment::Top);
	closeButton.Margin(Thickness{ 8, -2, -2, 0 });
	closeButton.Click([](auto const&, auto const&) {
		PostMessageW(g_hWnd, WM_CLOSEPANEL, 0, 0);
	});
	Grid::SetColumn(closeButton, 1);

	header.Children().Append(titleRow);
	header.Children().Append(closeButton);
	content.Children().Append(header);

	TextBlock subtitle = MakeTextBlock(_(L"Windows Bluetooth audio receiver"), 12, true);
	subtitle.Margin(Thickness{ 30, 0, 0, 0 });
	content.Children().Append(subtitle);

	Border divider;
	divider.Height(1);
	divider.Margin(Thickness{ 0, 10, 0, 10 });
	divider.Background(MakeBrush(45, 128, 128, 128));
	content.Children().Append(divider);

	g_panelDeviceList = StackPanel();
	content.Children().Append(MakeSection(_(L"Connected devices"), g_panelDeviceList));

	g_panelCatalogList = StackPanel();
	content.Children().Append(MakeSection(_(L"Available devices"), g_panelCatalogList));

	g_panelSummary = MakeTextBlock(L"", 12, true);
	g_panelSummary.Margin(Thickness{ 0, 4, 0, 10 });
	content.Children().Append(g_panelSummary);

	StackPanel actions;
	actions.Orientation(Orientation::Horizontal);
	g_panelReconnectButton = MakeButton(_(L"Reconnect"), true);
	g_panelReconnectButton.Click([](auto const&, auto const&) { ReconnectAllAsync(); });
	Button scanButton = MakeButton(_(L"Scan devices"), false);
	scanButton.Click([](auto const&, auto const&) { RefreshDeviceCatalogAsync(); });
	actions.Children().Append(g_panelReconnectButton);
	actions.Children().Append(scanButton);
	content.Children().Append(actions);

	Border divider2;
	divider2.Height(1);
	divider2.Margin(Thickness{ 0, 12, 0, 8 });
	divider2.Background(MakeBrush(45, 128, 128, 128));
	content.Children().Append(divider2);

	content.Children().Append(MakeTextBlock(_(L"Options"), 12, true));

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

	if (g_diagnosticLog)
	{
		TextBlock logHint = MakeTextBlock(
			L"Diagnostic build - log file: AudioPlaybackConnector.log (same folder as this exe)", 11, true);
		logHint.Margin(Thickness{ 0, 6, 0, 0 });
		logHint.TextWrapping(TextWrapping::Wrap);
		logHint.Opacity(0.6);
		content.Children().Append(logHint);
	}

	// The host window already has Windows 11 rounded corners. The panel is just
	// a flat full-bleed surface - no second rounded frame, no stroke.
	g_panelRoot = Border();
	g_panelRoot.Background(MakeBrush(g_lightTheme ? 248 : 38,
		g_lightTheme ? 255 : 32, g_lightTheme ? 255 : 32, g_lightTheme ? 255 : 32));
	g_panelRoot.Padding(Thickness{ 20, 14, 20, 16 });
	g_panelRoot.Child(content);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Placement(FlyoutPlacementMode::Top);
	flyout.Content(g_panelRoot);
	flyout.Closed([](auto const&, auto const&) {
		g_panelOpen = false;
		UninstallMouseHook();
		ShowWindow(g_hWnd, SW_HIDE);
	});
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
	g_panelCatalogList.Children().Clear();

	int connected = 0;
	int retrySeconds = 0;
	const auto now = std::chrono::steady_clock::now();
	for (const auto& kv : g_deviceStates)
	{
		const auto& state = kv.second;
		if (state.phase == ConnectionPhase::Connected)
			++connected;

		// The panel hint is the ONLY live countdown. Device rows show a static
		// status text so the number never freezes in two places at once.
		if (state.pendingAction == PendingAction::Retry)
		{
			const auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				state.actionAt - now).count();
			const int seconds = static_cast<int>((remainingMs + 999) / 1000);
			if (seconds > 0 && (retrySeconds == 0 || seconds < retrySeconds))
				retrySeconds = seconds;
		}

		std::wstring name = kv.first;
		if (state.device)
			name = std::wstring(state.device.Name());
		g_panelDeviceList.Children().Append(MakeDeviceRow(name,
			state.status.empty() ? std::wstring(_(L"Disconnected")) : state.status,
			state.phase, kv.first, false));
	}

	if (g_deviceStates.empty())
	{
		TextBlock empty = MakeTextBlock(_(L"No devices connected yet"), 13, true);
		empty.Margin(Thickness{ 0, 4, 0, 10 });
		g_panelDeviceList.Children().Append(empty);
	}

	int catalogRows = 0;
	for (const auto& device : g_availableDevices)
	{
		auto id = std::wstring(device.Id());
		// Already-connected devices are listed in the section above: do not
		// repeat them (and never print a "Connected" label here).
		if (g_audioPlaybackConnections.find(id) != g_audioPlaybackConnections.end())
			continue;
		g_panelCatalogList.Children().Append(
			MakeDeviceRow(std::wstring(device.Name()), L"", ConnectionPhase::Idle, id, true));
		++catalogRows;
	}

	if (catalogRows == 0)
	{
		TextBlock empty = MakeTextBlock(_(L"No devices found"), 12, true);
		empty.Margin(Thickness{ 0, 4, 0, 8 });
		g_panelCatalogList.Children().Append(empty);
	}

	if (g_panelAutoReconnect) g_panelAutoReconnect.IsOn(g_autoReconnect);
	if (g_panelAutoWarmup) g_panelAutoWarmup.IsOn(g_autoWarmup);
	if (g_panelStartReconnect) g_panelStartReconnect.IsOn(g_reconnect);
	if (g_panelReconnectButton)
		g_panelReconnectButton.IsEnabled(!g_deviceStates.empty() || !g_lastDevices.empty());

	if (g_panelSummary)
		g_panelSummary.Text(std::to_wstring(connected) + L" " + _(L"active") + L" / " +
			std::to_wstring(g_availableDevices.size()) + L" " + _(L"known devices"));

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
	if (g_panelHint) g_panelHint.Text(hint);

	// Tray icon follows the aggregate connection state.
	const auto iconState = ComputeIconState();
	if (iconState != g_iconState)
	{
		g_iconState = iconState;
		UpdateNotifyIcon();
	}

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

	Border border = MakeCard(CornerRadius{ 0, 0, 0, 0 }, g_lightTheme ? 245 : 38);
	border.Padding(Thickness{ 16, 14, 16, 14 });
	border.Child(stackPanel);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(border);
	g_xamlFlyout = flyout;
}

Button MakeMenuButton(const wchar_t* text, const wchar_t* glyph)
{
	Button button;
	button.FontSize(13);
	button.Padding(Thickness{ 12, 9, 12, 9 });
	button.Margin(Thickness{ 0, 0, 0, 2 });
	button.CornerRadius(CornerRadius{ 8, 8, 8, 8 });
	button.Background(MakeBrush(0, 0, 0, 0));
	button.HorizontalAlignment(HorizontalAlignment::Stretch);
	button.HorizontalContentAlignment(HorizontalAlignment::Left);

	StackPanel row;
	row.Orientation(Orientation::Horizontal);
	FontIcon icon = MakeGlyph(glyph, 15);
	icon.Margin(Thickness{ 0, 0, 12, 0 });
	icon.Foreground(MakeBrush(255, 0, 120, 212));
	row.Children().Append(icon);
	TextBlock label;
	label.Text(text);
	label.FontSize(13);
	label.VerticalAlignment(VerticalAlignment::Center);
	row.Children().Append(label);
	button.Content(row);
	return button;
}

Border MakeMenuSeparator()
{
	Border separator;
	separator.Height(1);
	separator.Margin(Thickness{ 8, 5, 8, 5 });
	separator.Background(MakeBrush(45, 128, 128, 128));
	return separator;
}

UIElement BuildMenuContent()
{
	StackPanel items;
	items.Width(220);

	Button settingsItem = MakeMenuButton(_(L"Bluetooth Settings"), L"\xE713");
	settingsItem.Click([](auto const&, auto const&) {
		g_menuFlyout.Hide();
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
	});
	items.Children().Append(settingsItem);

	Button exitItem = MakeMenuButton(_(L"Exit"), L"\xE8BB");
	exitItem.Click([](auto const&, auto const&) {
		g_menuFlyout.Hide();
		PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
	});
	items.Children().Append(exitItem);

	Border card = MakeCard(CornerRadius{ 0, 0, 0, 0 }, g_lightTheme ? 245 : 38);
	card.Padding(Thickness{ 6, 6, 6, 6 });
	card.Child(items);
	return card;
}

void SetupMenu()
{
	Flyout menu;
	menu.ShouldConstrainToRootBounds(false);
	menu.Placement(FlyoutPlacementMode::Top);
	menu.Closed([](auto const&, auto const&) {
		g_menuOpen = false;
		UninstallMouseHook();
		ShowWindow(g_hWnd, SW_HIDE);
	});
	g_menuFlyout = menu;
}

void ShowMenu()
{
	if (g_menuFlyout)
	{
		g_menuFlyout.Content(BuildMenuContent());
		g_menuFlyout.ShowAt(g_xamlCanvas);
		g_menuOpen = true;
		g_panelOpen = false;
		InstallMouseHook();
		SetTimer(g_hWnd, POPUP_STYLE_TIMER, 120, nullptr);
	}
}

void ClosePanel()
{
	g_panelOpen = false;
	g_menuOpen = false;
	if (g_panelFlyout) g_panelFlyout.Hide();
	if (g_menuFlyout) g_menuFlyout.Hide();
	UninstallMouseHook();
	ShowWindow(g_hWnd, SW_HIDE);
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
	LogW("PANEL", L"panel opened");
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
	RefreshDeviceCatalogAsync();
	RefreshPanel();
	g_panelFlyout.ShowAt(g_xamlCanvas);
	g_panelOpen = true;
	g_menuOpen = false;
	InstallMouseHook();
	// The flyout host HWND is created asynchronously; round it shortly after.
	SetTimer(g_hWnd, POPUP_STYLE_TIMER, 120, nullptr);
}

void ShowDevices()
{
	RefreshDeviceCatalogAsync();
	PostMessageW(g_hWnd, WM_SHOWPANEL, 0, 0);
}
