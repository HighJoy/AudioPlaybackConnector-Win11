#pragma once

// Connection orchestration.
//
// All state transitions happen on the UI thread: async continuations of the
// AudioPlaybackConnection operations run on the apartment that started them,
// and every delayed action goes through the DispatcherTimer. The DeviceWatcher
// callbacks only post a window message, so no shared container is touched from
// a worker thread.
//
// A monotonically increasing "generation" is attached to every attempt. When a
// connection is closed on purpose the generation is bumped first, so the stale
// StateChanged(Closed) callback cannot schedule a spurious reconnect.

// Implemented in Win11UI.hpp / AudioPlaybackConnector.cpp
void RefreshPanel();
winrt::fire_and_forget ReconnectAllAsync();

constexpr UINT WM_CATALOGCHANGED = WM_APP + 7;

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
winrt::Windows::Foundation::IAsyncAction RefreshDeviceCatalogAsync();
winrt::fire_and_forget OnCatalogChangedAsync();
void ProcessTimerTick();

inline void RequestPanelRefresh()
{
	if (g_hWnd)
		PostMessageW(g_hWnd, WM_REFRESHPANEL, 0, 0);
}

IconState ComputeIconState()
{
	bool connected = false;
	bool busy = false;
	for (const auto& kv : g_deviceStates)
	{
		switch (kv.second.phase)
		{
		case ConnectionPhase::Connected:
			connected = true;
			break;
		case ConnectionPhase::Connecting:
		case ConnectionPhase::WarmingUp:
		case ConnectionPhase::Retrying:
			busy = true;
			break;
		default:
			break;
		}
	}
	if (connected) return IconState::Connected;
	if (busy) return IconState::Busy;
	return IconState::Idle;
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

DeviceInformation FindDeviceInCatalog(std::wstring const& id)
{
	for (const auto& device : g_availableDevices)
	{
		if (std::wstring(device.Id()) == id)
			return device;
	}
	return nullptr;
}

// Re-enumerate the devices that can actually serve an AudioPlaybackConnection.
// Anything not in this list is treated as "not discoverable" and must not be
// retried. Called on every panel open, every watcher change and before a retry.
winrt::Windows::Foundation::IAsyncAction RefreshDeviceCatalogAsync()
{
	try
	{
		auto devices = co_await DeviceInformation::FindAllAsync(AudioPlaybackConnection::GetDeviceSelector());
		std::vector<DeviceInformation> fresh;
		for (const auto& device : devices)
			fresh.push_back(device);
		g_availableDevices = std::move(fresh);
	}
	catch (winrt::hresult_error const&)
	{
		LOG_CAUGHT_EXCEPTION();
		g_availableDevices.clear();
	}
	RequestPanelRefresh();
}

void MarkDeviceUnavailable(std::wstring const& id)
{
	auto it = g_deviceStates.find(id);
	if (it == g_deviceStates.end())
		return;
	auto& state = it->second;
	state.pendingAction = PendingAction::None;
	state.phase = ConnectionPhase::Unavailable;
	state.status = _(L"Device unavailable");
	state.waitingForDevice = true;
	state.retryCount = 0;
}

// Called on the UI thread after the DeviceWatcher posts WM_CATALOGCHANGED.
winrt::fire_and_forget OnCatalogChangedAsync()
{
	try
	{
	co_await RefreshDeviceCatalogAsync();

	std::vector<std::wstring> toReconnect;
	for (auto& kv : g_deviceStates)
	{
		auto& state = kv.second;
		const bool present = FindDeviceInCatalog(kv.first) != nullptr;

		if (!present)
		{
			// Device went away: cancel any pending connect attempt immediately.
			if (state.pendingAction != PendingAction::None || state.phase == ConnectionPhase::Retrying)
				MarkDeviceUnavailable(kv.first);
			continue;
		}

		if (state.waitingForDevice && g_autoReconnect && !state.manualDisconnect)
		{
			state.waitingForDevice = false;
			state.retryCount = 0;
			state.pendingAction = PendingAction::None;
			toReconnect.push_back(kv.first);
		}
	}

	for (const auto& id : toReconnect)
	{
		auto it = g_deviceStates.find(id);
		if (it != g_deviceStates.end())
			co_await ConnectDeviceCoreAsync(FindDeviceInCatalog(id));
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

void SetupDeviceWatcher()
{
	g_deviceWatcher = DeviceInformation::CreateWatcher(AudioPlaybackConnection::GetDeviceSelector());
	// Callbacks never touch shared state: they only ask the UI thread to re-check.
	g_deviceWatcher.Added([](auto const&, auto const&) {
		PostMessageW(g_hWnd, WM_CATALOGCHANGED, 0, 0);
	});
	g_deviceWatcher.Removed([](auto const&, auto const&) {
		PostMessageW(g_hWnd, WM_CATALOGCHANGED, 0, 0);
	});
	g_deviceWatcher.Updated([](auto const&, auto const&) {
		PostMessageW(g_hWnd, WM_CATALOGCHANGED, 0, 0);
	});
	g_deviceWatcher.Start();
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

	// 1, 2, 4, 8, 16, 30, 30 ... seconds. 30 s is the hard ceiling, so there is
	// no 60 s step. The only live countdown is the one in the panel hint.
	const int step = (std::min)(state.retryCount, 5);
	const int delayMs = (std::min)(1000 << step, 30000);
	if (state.retryCount < 1000000)
		++state.retryCount;
	state.pendingAction = PendingAction::Retry;
	state.actionAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
	state.phase = ConnectionPhase::Retrying;
	state.status = _(L"Waiting to reconnect");
	g_lastError = reason;
	RequestPanelRefresh();
}

winrt::Windows::Foundation::IAsyncAction ConnectDeviceCoreImplAsync(DeviceInformation device)
{
	auto id = std::wstring(device.Id());
	auto& state = g_deviceStates[id];
	state.device = device;
	state.manualDisconnect = false;
	state.waitingForDevice = false;
	state.pendingAction = PendingAction::None;
	const auto gen = ++g_generationCounter;
	state.generation = gen;
	state.phase = ConnectionPhase::Connecting;
	state.status = _(L"Connecting");
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
		ScheduleRetry(id, gen, current.status);
		RequestPanelRefresh();
		co_return;
	}

	current.everConnected = true;
	current.retryCount = 0;
	g_lastError.clear();

	if (g_autoWarmup && !current.warmupDone && current.warmupAttempts == 0)
	{
		// First successful Open() often reports "Connected" while the A2DP audio
		// endpoint is not streaming yet. Automate the manual "connect twice"
		// workaround once, silently, after a short settle period.
		current.warmupAttempts = 1;
		current.phase = ConnectionPhase::WarmingUp;
		current.status = _(L"Initializing audio (1/2)");
		current.pendingAction = PendingAction::WarmupSecondConnect;
		current.actionAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
	}
	else
	{
		current.warmupDone = true;
		current.phase = ConnectionPhase::Connected;
		current.status = _(L"Connected");
	}
	RequestPanelRefresh();
}

// A fire_and_forget coroutine must never let a WinRT exception escape.
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
	}
	else
	{
		current->second.phase = ConnectionPhase::Failed;
		current->second.status = attempt->error.empty() ? std::wstring(_(L"Connection lost")) : attempt->error;
		g_lastError = current->second.status;
		// A failed warm-up must not turn into an endless connect/disconnect loop.
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

// Only reconnect when the device is currently discoverable.
winrt::fire_and_forget ReconnectAsync(std::wstring id, uint64_t gen, bool manual)
{
	try
	{
	auto it = g_deviceStates.find(id);
	if (it == g_deviceStates.end())
		co_return;
	if (!manual && (it->second.generation != gen || it->second.manualDisconnect || !g_autoReconnect))
		co_return;

	co_await RefreshDeviceCatalogAsync();
	auto device = FindDeviceInCatalog(id);

	auto current = g_deviceStates.find(id);
	if (current == g_deviceStates.end())
		co_return;
	if (!manual && (current->second.generation != gen || current->second.manualDisconnect || !g_autoReconnect))
		co_return;

	if (!device)
	{
		MarkDeviceUnavailable(id);
		g_lastError = _(L"Device not found");
		RequestPanelRefresh();
		co_return;
	}

	current->second.waitingForDevice = false;
	co_await ConnectDeviceCoreAsync(device);
	}
	catch (winrt::hresult_error const& ex)
	{
		LOG_CAUGHT_EXCEPTION();
		g_lastError = FormatHresultError(ex);
		RequestPanelRefresh();
	}
}

// Manual "Reconnect" button: only devices that are discoverable right now.
winrt::fire_and_forget ReconnectAllAsync()
{
	try
	{
	co_await RefreshDeviceCatalogAsync();

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
		PostMessageW(g_hWnd, WM_SHOWDEVICES, 0, 0);
		co_return;
	}

	for (const auto& id : ids)
	{
		auto device = FindDeviceInCatalog(id);
		auto it = g_deviceStates.find(id);
		if (!device)
		{
			if (it != g_deviceStates.end())
				MarkDeviceUnavailable(id);
			continue;
		}

		if (it != g_deviceStates.end())
		{
			it->second.generation = ++g_generationCounter;
			it->second.pendingAction = PendingAction::None;
			it->second.manualDisconnect = false;
			it->second.waitingForDevice = false;
			it->second.retryCount = 0;
			it->second.suppressClosed = true;

			auto connIt = g_audioPlaybackConnections.find(id);
			if (connIt != g_audioPlaybackConnections.end())
			{
				connIt->second.second.Close();
				g_audioPlaybackConnections.erase(connIt);
			}
			it->second.suppressClosed = false;
		}

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
	co_await RefreshDeviceCatalogAsync();
	for (const auto& id : ids)
	{
		auto device = FindDeviceInCatalog(id);
		if (device)
			co_await ConnectDeviceCoreAsync(device);
		else
			MarkDeviceUnavailable(id);
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

void CloseDevice(std::wstring const& id, bool manual)
{
	auto it = g_deviceStates.find(id);
	if (it != g_deviceStates.end())
	{
		auto& state = it->second;
		state.generation = ++g_generationCounter;
		state.pendingAction = PendingAction::None;
		state.manualDisconnect = manual;
		state.waitingForDevice = false;
		state.suppressClosed = true;
		state.phase = ConnectionPhase::Idle;
		state.status = _(L"Disconnected");
	}

	auto connIt = g_audioPlaybackConnections.find(id);
	if (connIt != g_audioPlaybackConnections.end())
	{
		connIt->second.second.Close();
		g_audioPlaybackConnections.erase(connIt);
	}

	if (it != g_deviceStates.end())
		it->second.suppressClosed = false;
	RequestPanelRefresh();
}

void CloseAllConnections()
{
	for (const auto& kv : g_audioPlaybackConnections)
		kv.second.second.Close();
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
