#pragma once

// Connection orchestration.
//
// All state transitions happen on the UI thread: async continuations of the
// AudioPlaybackConnection operations run on the apartment that started them,
// and every delayed action goes through the DispatcherTimer instead of a
// thread-pool resume_after. That keeps g_audioPlaybackConnections and
// g_deviceStates free of data races without extra locking.
//
// A monotonically increasing "generation" is attached to every attempt. When a
// connection is closed on purpose (warm-up, manual reconnect, user disconnect)
// the generation is bumped first, so the StateChanged(Closed) callback from the
// old connection is ignored and cannot schedule a spurious reconnect.

// Implemented in Win11UI.hpp / AudioPlaybackConnector.cpp
void RefreshPanel();
winrt::fire_and_forget ReconnectAllAsync();

struct AttemptResult
{
	bool success = false;
	std::wstring error;
};

void OnConnectionClosed(std::wstring const& id, uint64_t gen, AudioPlaybackConnection const& sender);
void ScheduleRetry(std::wstring const& id, uint64_t gen, std::wstring const& reason);
winrt::Windows::Foundation::IAsyncAction ConnectDeviceCoreAsync(DeviceInformation device);
winrt::fire_and_forget ReconnectAsync(std::wstring id, uint64_t gen, bool manual);
winrt::fire_and_forget WarmupSecondConnectAsync(std::wstring id, uint64_t gen);
void ProcessTimerTick();

inline void RequestPanelRefresh()
{
	if (g_hWnd)
		PostMessageW(g_hWnd, WM_REFRESHPANEL, 0, 0);
}

std::wstring FormatHresultError(winrt::hresult_error const& ex)
{
	std::wstring message(64, L'\0');
	while (true)
	{
		auto result = swprintf(message.data(), message.size(), L"%s (0x%08X)", ex.message().c_str(), static_cast<uint32_t>(ex.code()));
		if (result < 0)
		{
			message.resize(message.size() * 2);
		}
		else
		{
			message.resize(static_cast<size_t>(result));
			break;
		}
	}
	return message;
}

// Performs exactly one TryCreateFromId -> StartAsync -> OpenAsync cycle.
winrt::Windows::Foundation::IAsyncAction OpenOnceAsync(
	DeviceInformation device,
	uint64_t gen,
	std::shared_ptr<AttemptResult> result)
{
	auto id = std::wstring(device.Id());
	AudioPlaybackConnection connection = nullptr;

	try
	{
		connection = AudioPlaybackConnection::TryCreateFromId(device.Id());
		if (!connection)
		{
			result->error = _(L"Unknown error");
			co_return;
		}

		// operator[] would require a default-constructible std::pair, which
		// C++/WinRT projection types are not. Follow upstream's emplace pattern.
		g_audioPlaybackConnections.erase(id);
		g_audioPlaybackConnections.emplace(id, std::pair(device, connection));

		connection.StateChanged([id, gen](const auto& sender, const auto&) {
			if (sender.State() == AudioPlaybackConnectionState::Closed)
				OnConnectionClosed(id, gen, sender);
		});

		co_await connection.StartAsync();
		auto open = co_await connection.OpenAsync();

		switch (open.Status())
		{
		case AudioPlaybackConnectionOpenResultStatus::Success:
			result->success = true;
			break;
		case AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
			result->error = _(L"The request timed out");
			break;
		case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
			result->error = _(L"The operation was denied by the system");
			break;
		case AudioPlaybackConnectionOpenResultStatus::UnknownFailure:
			winrt::throw_hresult(open.ExtendedError());
			break;
		}
	}
	catch (winrt::hresult_error const& ex)
	{
		result->success = false;
		result->error = FormatHresultError(ex);
		LOG_CAUGHT_EXCEPTION();
	}

	if (!result->success)
	{
		// Suppress the Closed callback while tearing down the failed attempt;
		// the caller owns the retry decision.
		auto stateIt = g_deviceStates.find(id);
		if (stateIt != g_deviceStates.end())
			stateIt->second.suppressClosed = true;

		auto it = g_audioPlaybackConnections.find(id);
		if (it != g_audioPlaybackConnections.end())
		{
			it->second.second.Close();
			g_audioPlaybackConnections.erase(it);
		}

		if (stateIt != g_deviceStates.end())
			stateIt->second.suppressClosed = false;
	}
}

void OnConnectionClosed(std::wstring const& id, uint64_t gen, AudioPlaybackConnection const& sender)
{
	auto stateIt = g_deviceStates.find(id);
	if (stateIt == g_deviceStates.end() || stateIt->second.generation != gen || stateIt->second.suppressClosed)
		return;

	auto& state = stateIt->second;
	auto it = g_audioPlaybackConnections.find(id);
	if (it != g_audioPlaybackConnections.end() && it->second.second == sender)
		g_audioPlaybackConnections.erase(it);
	sender.Close();

	if (state.device)
		g_devicePicker.SetDisplayStatus(state.device, winrt::hstring{}, DevicePickerDisplayStatusOptions::None);

	if (state.manualDisconnect || !g_autoReconnect || !state.everConnected)
	{
		state.phase = ConnectionPhase::Idle;
		state.status = _(L"Disconnected");
	}
	else
	{
		ScheduleRetry(id, gen, _(L"Connection lost"));
	}
	RequestPanelRefresh();
}

void ScheduleRetry(std::wstring const& id, uint64_t gen, std::wstring const& reason)
{
	auto it = g_deviceStates.find(id);
	if (it == g_deviceStates.end() || it->second.generation != gen)
		return;

	auto& state = it->second;
	if (state.manualDisconnect || !g_autoReconnect)
		return;

	if (g_maxRetries > 0 && state.retryCount >= g_maxRetries)
	{
		state.phase = ConnectionPhase::Failed;
		state.status = _(L"Automatic reconnect stopped");
		g_lastError = reason;
		return;
	}

	const int step = (std::min)(state.retryCount, 5);
	const int delayMs = (std::min)(1000 << step, 30000);
	++state.retryCount;
	state.pendingAction = PendingAction::Retry;
	state.actionAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
	state.phase = ConnectionPhase::Retrying;
	state.status = std::wstring(_(L"Reconnecting in ")) + std::to_wstring((delayMs + 999) / 1000) + L" s";
	g_lastError = reason;
	RequestPanelRefresh();
}

winrt::Windows::Foundation::IAsyncAction ConnectDeviceCoreImplAsync(DeviceInformation device)
{
	auto id = std::wstring(device.Id());
	auto& state = g_deviceStates[id];
	state.device = device;
	state.manualDisconnect = false;
	state.pendingAction = PendingAction::None;
	const auto gen = ++g_generationCounter;
	state.generation = gen;
	state.phase = ConnectionPhase::Connecting;
	state.status = _(L"Connecting");

	g_devicePicker.SetDisplayStatus(device, _(L"Connecting"),
		DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton);
	RequestPanelRefresh();

	auto attempt = std::make_shared<AttemptResult>();
	co_await OpenOnceAsync(device, gen, attempt);

	auto it = g_deviceStates.find(id);
	if (it == g_deviceStates.end() || it->second.generation != gen)
		co_return;
	auto& current = it->second;

	const bool stillOpen = g_audioPlaybackConnections.find(id) != g_audioPlaybackConnections.end();
	if (!attempt->success || !stillOpen)
	{
		current.phase = ConnectionPhase::Failed;
		current.status = attempt->error.empty() ? std::wstring(_(L"Connection lost")) : attempt->error;
		g_lastError = current.status;
		g_devicePicker.SetDisplayStatus(device, current.status, DevicePickerDisplayStatusOptions::ShowRetryButton);
		ScheduleRetry(id, gen, current.status);
		RequestPanelRefresh();
		co_return;
	}

	current.everConnected = true;
	current.retryCount = 0;
	g_lastError.clear();

	if (g_autoWarmup && !current.warmupDone && current.warmupAttempts == 0)
	{
		// Windows often reports the first A2DP Sink Open() as successful while the
		// audio endpoint is not streaming yet. Automate the usual workaround
		// ("disconnect and connect a second time") once, silently, after a short
		// settle period. This is the fix for "connected but no sound".
		current.warmupAttempts = 1;
		current.phase = ConnectionPhase::WarmingUp;
		current.status = _(L"Initializing audio (1/2)");
		current.pendingAction = PendingAction::WarmupSecondConnect;
		current.actionAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
		g_devicePicker.SetDisplayStatus(device, _(L"Initializing audio"),
			DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton);
	}
	else
	{
		current.warmupDone = true;
		current.phase = ConnectionPhase::Connected;
		current.status = _(L"Connected");
		g_devicePicker.SetDisplayStatus(device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
	}
	RequestPanelRefresh();
}

// A fire_and_forget coroutine must never let a WinRT exception escape, otherwise
// the process terminates. Funnel every connection path through guarded wrappers.
winrt::Windows::Foundation::IAsyncAction ConnectDeviceCoreAsync(DeviceInformation device)
{
	try
	{
		co_await ConnectDeviceCoreImplAsync(device);
	}
	catch (winrt::hresult_error const& ex)
	{
		LOG_CAUGHT_EXCEPTION();
		g_lastError = FormatHresultError(ex);
		RequestPanelRefresh();
	}
}

winrt::fire_and_forget WarmupSecondConnectAsync(std::wstring id, uint64_t gen)
{
	try
	{
	auto it = g_deviceStates.find(id);
	if (it == g_deviceStates.end() || it->second.generation != gen)
		co_return;

	auto device = it->second.device;
	if (!device)
		co_return;

	// Bump the generation before closing so the old StateChanged callback is stale.
	const auto newGen = ++g_generationCounter;
	auto& state = it->second;
	state.generation = newGen;
	state.suppressClosed = true;

	auto connIt = g_audioPlaybackConnections.find(id);
	if (connIt != g_audioPlaybackConnections.end())
	{
		connIt->second.second.Close();
		g_audioPlaybackConnections.erase(connIt);
	}
	state.suppressClosed = false;

	state.phase = ConnectionPhase::WarmingUp;
	state.status = _(L"Initializing audio (2/2)");
	g_devicePicker.SetDisplayStatus(device, _(L"Initializing audio (2/2)"),
		DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton);
	RequestPanelRefresh();

	auto attempt = std::make_shared<AttemptResult>();
	co_await OpenOnceAsync(device, newGen, attempt);

	auto current = g_deviceStates.find(id);
	if (current == g_deviceStates.end() || current->second.generation != newGen)
		co_return;

	if (attempt->success && g_audioPlaybackConnections.find(id) != g_audioPlaybackConnections.end())
	{
		current->second.warmupDone = true;
		current->second.phase = ConnectionPhase::Connected;
		current->second.status = _(L"Connected");
		current->second.retryCount = 0;
		g_devicePicker.SetDisplayStatus(device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
	}
	else
	{
		current->second.phase = ConnectionPhase::Failed;
		current->second.status = attempt->error.empty() ? std::wstring(_(L"Connection lost")) : attempt->error;
		g_lastError = current->second.status;
		g_devicePicker.SetDisplayStatus(device, current->second.status, DevicePickerDisplayStatusOptions::ShowRetryButton);
		// A failed warm-up must not turn into an endless connect/disconnect loop
		// when the source device does not re-initiate the A2DP stream.
		if (current->second.retryCount < 3)
			ScheduleRetry(id, newGen, current->second.status);
		else
			current->second.status = _(L"Automatic reconnect stopped");
	}
	RequestPanelRefresh();
	}
	catch (winrt::hresult_error const& ex)
	{
		LOG_CAUGHT_EXCEPTION();
		g_lastError = FormatHresultError(ex);
		RequestPanelRefresh();
	}
}

winrt::fire_and_forget ReconnectAsync(std::wstring id, uint64_t gen, bool manual)
{
	try
	{
	auto it = g_deviceStates.find(id);
	if (it == g_deviceStates.end())
		co_return;
	if (!manual && (it->second.generation != gen || it->second.manualDisconnect || !g_autoReconnect))
		co_return;

	auto device = it->second.device;
	if (!device)
	{
		try
		{
			device = co_await DeviceInformation::CreateFromIdAsync(id);
		}
		catch (winrt::hresult_error const&)
		{
			LOG_CAUGHT_EXCEPTION();
		}
	}

	if (!device)
	{
		ScheduleRetry(id, gen, _(L"Device not found"));
		RequestPanelRefresh();
		co_return;
	}

	co_await ConnectDeviceCoreAsync(device);
	}
	catch (winrt::hresult_error const& ex)
	{
		LOG_CAUGHT_EXCEPTION();
		g_lastError = FormatHresultError(ex);
		RequestPanelRefresh();
	}
}

// Manual "Reconnect" button: close every known connection and open it again.
// This mirrors the user's own workaround and does not run the warm-up again.
winrt::fire_and_forget ReconnectAllAsync()
{
	try
	{
	std::vector<std::wstring> ids;
	for (const auto& kv : g_deviceStates)
		ids.push_back(kv.first);
	for (const auto& id : g_lastDevices)
	{
		if (std::find(ids.begin(), ids.end(), id) == ids.end())
			ids.push_back(id);
	}

	if (ids.empty())
	{
		PostMessageW(g_hWnd, WM_SHOWPICKER, 0, 0);
		co_return;
	}

	for (const auto& id : ids)
	{
		DeviceInformation device = nullptr;
		auto it = g_deviceStates.find(id);
		if (it != g_deviceStates.end())
		{
			it->second.generation = ++g_generationCounter;
			it->second.pendingAction = PendingAction::None;
			it->second.manualDisconnect = false;
			it->second.retryCount = 0;
			it->second.suppressClosed = true;
			device = it->second.device;

			auto connIt = g_audioPlaybackConnections.find(id);
			if (connIt != g_audioPlaybackConnections.end())
			{
				connIt->second.second.Close();
				g_audioPlaybackConnections.erase(connIt);
			}
			it->second.suppressClosed = false;
		}

		if (!device)
		{
			try
			{
				device = co_await DeviceInformation::CreateFromIdAsync(id);
			}
			catch (winrt::hresult_error const&)
			{
				LOG_CAUGHT_EXCEPTION();
			}
		}

		if (device)
			co_await ConnectDeviceCoreAsync(device);
	}
	RequestPanelRefresh();
	}
	catch (winrt::hresult_error const& ex)
	{
		LOG_CAUGHT_EXCEPTION();
		g_lastError = FormatHresultError(ex);
		RequestPanelRefresh();
	}
}

winrt::fire_and_forget RestoreLastDevices()
{
	try
	{
	auto ids = g_lastDevices;
	g_lastDevices.clear();
	for (const auto& id : ids)
	{
		try
		{
			auto device = co_await DeviceInformation::CreateFromIdAsync(id);
			if (device)
				co_await ConnectDeviceCoreAsync(device);
		}
		catch (winrt::hresult_error const&)
		{
			LOG_CAUGHT_EXCEPTION();
		}
	}
	}
	catch (winrt::hresult_error const& ex)
	{
		LOG_CAUGHT_EXCEPTION();
		g_lastError = FormatHresultError(ex);
		RequestPanelRefresh();
	}
}

void CloseDevice(std::wstring const& id, bool manual)
{
	auto it = g_deviceStates.find(id);
	if (it != g_deviceStates.end())
	{
		auto& state = it->second;
		state.generation = ++g_generationCounter;
		state.pendingAction = PendingAction::None;
		state.manualDisconnect = manual;
		state.suppressClosed = true;
		state.phase = ConnectionPhase::Idle;
		state.status = _(L"Disconnected");
	}

	auto connIt = g_audioPlaybackConnections.find(id);
	if (connIt != g_audioPlaybackConnections.end())
	{
		auto device = connIt->second.first;
		connIt->second.second.Close();
		g_audioPlaybackConnections.erase(connIt);
		g_devicePicker.SetDisplayStatus(device, winrt::hstring{}, DevicePickerDisplayStatusOptions::None);
	}

	if (it != g_deviceStates.end())
		it->second.suppressClosed = false;
	RequestPanelRefresh();
}

void CloseAllConnections()
{
	for (const auto& kv : g_audioPlaybackConnections)
	{
		kv.second.second.Close();
		g_devicePicker.SetDisplayStatus(kv.second.first, winrt::hstring{}, DevicePickerDisplayStatusOptions::None);
	}
}

void ProcessTimerTick()
{
	struct DueAction
	{
		std::wstring id;
		PendingAction action;
		uint64_t gen;
	};

	std::vector<DueAction> due;
	const auto now = std::chrono::steady_clock::now();
	for (auto& kv : g_deviceStates)
	{
		auto& state = kv.second;
		if (state.pendingAction != PendingAction::None && now >= state.actionAt)
		{
			due.push_back({ kv.first, state.pendingAction, state.generation });
			state.pendingAction = PendingAction::None;
		}
	}

	for (const auto& item : due)
	{
		if (item.action == PendingAction::Retry)
			ReconnectAsync(item.id, item.gen, false);
		else if (item.action == PendingAction::WarmupSecondConnect)
			WarmupSecondConnectAsync(item.id, item.gen);
	}

	if ((++g_timerTicks % 4) == 0)
		RequestPanelRefresh();
}

void SetupRetryTimer()
{
	g_retryTimer = DispatcherTimer();
	g_retryTimer.Interval(std::chrono::milliseconds(250));
	g_retryTimer.Tick([](auto const&, auto const&) { ProcessTimerTick(); });
	g_retryTimer.Start();
}

// Compatibility entry points used by WndProc and the device picker.
winrt::fire_and_forget ConnectDevice(DevicePicker, DeviceInformation device)
{
	try
	{
	co_await ConnectDeviceCoreAsync(device);
	}
	catch (winrt::hresult_error const& ex)
	{
		LOG_CAUGHT_EXCEPTION();
		g_lastError = FormatHresultError(ex);
		RequestPanelRefresh();
	}
}

winrt::fire_and_forget ConnectDevice(DevicePicker, std::wstring_view deviceId)
{
	try
	{
	auto id = std::wstring(deviceId);
	DeviceInformation device = nullptr;
	try
	{
		device = co_await DeviceInformation::CreateFromIdAsync(id);
	}
	catch (winrt::hresult_error const&)
	{
		LOG_CAUGHT_EXCEPTION();
	}
	if (device)
		co_await ConnectDeviceCoreAsync(device);
	}
	catch (winrt::hresult_error const& ex)
	{
		LOG_CAUGHT_EXCEPTION();
		g_lastError = FormatHresultError(ex);
		RequestPanelRefresh();
	}
}
