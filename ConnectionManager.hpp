#pragma once

// Connection orchestration.
//
// All state transitions happen on the UI thread: async continuations of the
// AudioPlaybackConnection operations resume on the apartment that started
// them, and every delayed action goes through the DispatcherTimer. The
// DeviceWatcher callbacks only post a window message, so no shared container
// is touched from a worker thread.
//
// A monotonically increasing "generation" is attached to every attempt. When a
// connection is closed on purpose the generation is bumped first, so the stale
// StateChanged(Closed) callback cannot schedule a spurious reconnect.
//
// RECONNECT RULE (the user-visible contract): when a connection drops, the app
// does NOT immediately reconnect. It first performs a real-time presence probe
// (fresh enumeration + System.Devices.Aep.IsPresent). Only a device that is
// actually present right now gets a retry. Devices reported Removed by the
// watcher, or whose Open() timed out, are blocked until an explicit Added
// event proves they came back.

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
winrt::Windows::Foundation::IAsyncAction RefreshDeviceCatalogAsync();
winrt::fire_and_forget OnCatalogChangedAsync();
void ProcessWatcherEvents();
void SetupDeviceWatcher();
void ProcessTimerTick();
winrt::Windows::Foundation::IAsyncOperation<bool> ProbeDevicePresentAsync(std::wstring id, bool forceProbe = false);
winrt::fire_and_forget VerifyAndScheduleRetryAsync(std::wstring id, uint64_t gen, std::wstring reason);

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
			message.resize(message.size * 2);
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

bool IsDeviceVisible(std::wstring const& id)
{
	return g_blockedDeviceIds.find(id) == g_blockedDeviceIds.end() &&
		g_visibleDeviceIds.find(id) != g_visibleDeviceIds.end();
}

// Refresh ONLY the list shown in the panel. This deliberately does not touch
// the visible/blocked sets: FindAllAsync reports paired-but-powered-off phones,
// so it can never prove a device is currently reachable.
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

// Real-time presence probe. This is the ONLY gate that allows a reconnect.
// It re-enumerates the AEP devices with the IsPresent property requested and
// looks at the CURRENT value, instead of trusting a cached id set.
// forceProbe is used by the manual "Reconnect" button, which must be able to
// re-check a device that was blocked by an earlier timeout.
winrt::Windows::Foundation::IAsyncOperation<bool> ProbeDevicePresentAsync(std::wstring id, bool forceProbe)
{
	// Hard stop: the watcher reported the device gone, or a previous Open()
	// timed out. Only an explicit Added event can unblock it again.
	if (!forceProbe && g_blockedDeviceIds.find(id) != g_blockedDeviceIds.end())
		co_return false;

	try
	{
		std::vector<winrt::hstring> properties{
			winrt::hstring(L"System.Devices.Aep.IsPresent")
		};
		auto devices = co_await DeviceInformation::FindAllAsync(
			AudioPlaybackConnection::GetDeviceSelector(), properties);

		for (const auto& device : devices)
		{
			const auto deviceId = std::wstring(device.Id());
			const bool isTarget = deviceId == id;
			const auto map = device.Properties();
			bool present = true;
			if (map.HasKey(L"System.Devices.Aep.IsPresent"))
				present = winrt::unbox_value_or<bool>(map.Lookup(L"System.Devices.Aep.IsPresent"), false);

			// Remember offline devices too, so a later retry cannot slip through.
			if (!present && !isTarget)
				g_blockedDeviceIds.insert(deviceId);
			if (isTarget)
				co_return present;
		}
		// Not enumerated at all right now -> definitely not present.
		co_return false;
	}
	catch (winrt::hresult_error const&)
	{
		LOG_CAUGHT_EXCEPTION();
		co_return false;
	}
}

void MarkDeviceUnavailable(std::wstring const& id)
{
	// A blocked device stays blocked until DeviceWatcher reports Added again.
	g_blockedDeviceIds.insert(id);
	g_visibleDeviceIds.erase(id);
	auto it = g_deviceStates.find(id);
	if (it == g_deviceStates.end())
		return;
	auto& state = it->second;
	state.pendingAction = PendingAction::None;
	state.probing = false;
	state.phase = ConnectionPhase::Unavailable;
	state.status = _(L"Device unavailable");
	state.waitingForDevice = true;
	state.retryCount = 0;
}

// Drain watcher events on the UI thread. A Removed event (Bluetooth switched
// off / out of range) cancels any pending retry immediately and blocks the
// device. An Added event is the ONLY thing that can unblock it.
void ProcessWatcherEvents()
{
	std::vector<std::pair<std::wstring, bool>> events;
	{
		std::lock_guard<std::mutex> lock(g_watcherMutex);
		events.swap(g_watcherEvents);
	}

	for (const auto& event : events)
	{
		const auto& id = event.first;
		if (event.second)
		{
			g_blockedDeviceIds.erase(id);
			g_visibleDeviceIds.insert(id);
		}
		else
		{
			g_visibleDeviceIds.erase(id);
			g_blockedDeviceIds.insert(id);
			auto it = g_deviceStates.find(id);
			if (it != g_deviceStates.end() &&
				(it->second.pendingAction != PendingAction::None ||
				 it->second.phase == ConnectionPhase::Retrying ||
				 it->second.phase == ConnectionPhase::Connecting ||
				 it->second.phase == ConnectionPhase::Unavailable))
				MarkDeviceUnavailable(id);
		}
	}
}

// Watcher events only wake us up. Recovery still has to pass a fresh probe.
winrt::fire_and_forget OnCatalogChangedAsync()
{
	try
	{
	ProcessWatcherEvents();
	co_await RefreshDeviceCatalogAsync();

	std::vector<DeviceInformation> resume;
	std::vector<std::wstring> waitingIds;
	for (const auto& kv : g_deviceStates)
	{
		if (kv.second.waitingForDevice && g_autoReconnect && !kv.second.manualDisconnect &&
			g_blockedDeviceIds.find(kv.first) == g_blockedDeviceIds.end())
			waitingIds.push_back(kv.first);
	}

	for (const auto& id : waitingIds)
	{
		const bool present = co_await ProbeDevicePresentAsync(id);
		auto it = g_deviceStates.find(id);
		if (it == g_deviceStates.end() || !it->second.waitingForDevice || it->second.manualDisconnect)
			continue;
		if (!present)
		{
			MarkDeviceUnavailable(id);
			continue;
		}
		auto device = FindDeviceInCatalog(id);
		if (!device)
			device = it->second.device;
		if (!device)
			continue;
		it->second.waitingForDevice = false;
		it->second.pendingAction = PendingAction::None;
		it->second.retryCount = 0;
		resume.push_back(device);
	}
	for (const auto& device : resume)
		co_await ConnectDeviceCoreAsync(device);
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
	// Watcher callbacks may arrive on a worker thread: only queue the id under
	// a lock, then let the UI thread apply the state change.
	g_deviceWatcher.Added([](auto const&, auto const& info) {
		auto id = std::wstring(info.Id());
		{
			std::lock_guard<std::mutex> lock(g_watcherMutex);
			g_watcherEvents.emplace_back(id, true);
		}
		PostMessageW(g_hWnd, WM_CATALOGCHANGED, 0, 0);
	});
	g_deviceWatcher.Removed([](auto const&, auto const& update) {
		auto id = std::wstring(update.Id());
		{
			std::lock_guard<std::mutex> lock(g_watcherMutex);
			g_watcherEvents.emplace_back(id, false);
		}
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
		// Never schedule a retry directly: check that the device is really
		// still present first, then decide.
		VerifyAndScheduleRetryAsync(std::wstring(id), gen, _(L"Connection lost"));
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

	// Defence in depth: a blocked device can never get a retry slot.
	if (g_blockedDeviceIds.find(id) != g_blockedDeviceIds.end())
	{
		MarkDeviceUnavailable(id);
		return;
	}

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

// The single entry point after a drop. It performs the presence check BEFORE
// any retry is queued, so a phone with Bluetooth turned off stops immediately.
winrt::fire_and_forget VerifyAndScheduleRetryAsync(std::wstring id, uint64_t gen, std::wstring reason)
{
	try
	{
	auto it = g_deviceStates.find(id);
	if (it == g_deviceStates.end() || it->second.generation != gen ||
		it->second.manualDisconnect || !g_autoReconnect || it->second.probing)
		co_return;

	it->second.probing = true;
	it->second.pendingAction = PendingAction::None;
	it->second.phase = ConnectionPhase::Retrying;
	it->second.status = _(L"Waiting to reconnect");
	RequestPanelRefresh();

	const bool present = co_await ProbeDevicePresentAsync(id);

	// Re-validate after the await: the user may have disconnected meanwhile.
	auto current = g_deviceStates.find(id);
	if (current == g_deviceStates.end())
		co_return;
	current->second.probing = false;
	if (current->second.generation != gen || current->second.manualDisconnect || !g_autoReconnect)
	{
		RequestPanelRefresh();
		co_return;
	}

	if (!present)
	{
		MarkDeviceUnavailable(id);
		g_lastError = _(L"Device not found");
		RequestPanelRefresh();
		co_return;
	}
	ScheduleRetry(id, gen, reason);
	}
	catch (winrt::hresult_error const& ex)
	{
		LOG_CAUGHT_EXCEPTION();
		g_lastError = FormatHresultError(ex);
		RequestPanelRefresh();
	}
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
		current.status = attempt->error.empty() ? std::wstring(_(L"Connection lost")) : attempt->error;
		g_lastError = current.status;
		const bool timedOut = attempt->error.find(_(L"The request timed out")) != std::wstring::npos;
		if (timedOut)
		{
			// Open() timed out: the source is unreachable. Block it immediately,
			// no further attempts until the watcher sees it again.
			MarkDeviceUnavailable(id);
			RequestPanelRefresh();
			co_return;
		}
		VerifyAndScheduleRetryAsync(id, gen, current.status);
		RequestPanelRefresh();
		co_return;
	}

	current.everConnected = true;
	current.retryCount = 0;
	g_blockedDeviceIds.erase(id);
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
		const bool timedOut = attempt->error.find(_(L"The request timed out")) != std::wstring::npos;
		if (timedOut)
			MarkDeviceUnavailable(id);
		else if (current->second.retryCount < 3)
			VerifyAndScheduleRetryAsync(id, newGen, current->second.status);
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

// Every retry that fires from the timer passes through another fresh probe.
winrt::fire_and_forget ReconnectAsync(std::wstring id, uint64_t gen, bool manual)
{
	try
	{
	auto it = g_deviceStates.find(id);
	if (it == g_deviceStates.end())
		co_return;
	if (!manual && (it->second.generation != gen || it->second.manualDisconnect || !g_autoReconnect))
		co_return;

	const bool present = co_await ProbeDevicePresentAsync(id);

	auto current = g_deviceStates.find(id);
	if (current == g_deviceStates.end())
		co_return;
	if (!manual && (current->second.generation != gen || current->second.manualDisconnect || !g_autoReconnect))
		co_return;

	if (!present)
	{
		MarkDeviceUnavailable(id);
		g_lastError = _(L"Device not found");
		RequestPanelRefresh();
		co_return;
	}

	auto device = FindDeviceInCatalog(id);
	if (!device)
		device = current->second.device;
	if (!device)
	{
		co_await RefreshDeviceCatalogAsync();
		device = FindDeviceInCatalog(id);
	}
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
		// Manual recheck ignores the block set but still requires a fresh
		// "device is present" result from the enumeration.
		const bool present = co_await ProbeDevicePresentAsync(id, true);
		auto it = g_deviceStates.find(id);
		if (!present)
		{
			if (it != g_deviceStates.end())
				MarkDeviceUnavailable(id);
			g_lastError = _(L"Device not found");
			continue;
		}
		auto device = FindDeviceInCatalog(id);
		if (!device && it != g_deviceStates.end())
			device = it->second.device;
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
		const bool present = co_await ProbeDevicePresentAsync(id, true);
		if (!present)
		{
			MarkDeviceUnavailable(id);
			continue;
		}
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
		state.probing = false;
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
