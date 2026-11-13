#pragma once

// Connection orchestration.
//
// All state transitions happen on the UI thread: async continuations of the
// AudioPlaybackConnection operations resume on the apartment that started
// them, and every delayed action goes through the DispatcherTimer. The
// DeviceWatcher callbacks only post a window message, so no shared container
// is touched from a worker thread (except the mutex-protected event queue and
// the diagnostic log, both of which are thread-safe).
//
// A monotonically increasing "generation" is attached to every attempt. When a
// connection is closed on purpose the generation is bumped first, so the stale
// StateChanged(Closed) callback cannot schedule a spurious reconnect.
//
// RECONNECT RULE (diagnostic build):
// System.Devices.Aep.IsPresent turned out to be unreliable - Windows reports
// false for a paired phone whose Bluetooth is ON but whose A2DP link just
// dropped. It must NOT be used to veto a reconnect. Instead a device is treated
// as absent only when (a) the watcher explicitly reported it Removed, or
// (b) an Open() call timed out. A fresh Closed event itself is strong evidence
// that the device was present a moment ago, so it clears the block and allows
// one retry; a timed-out Open() immediately blocks it again. Added/Updated
// watcher events unblock it. Every step is written to the diagnostic log.

// Implemented in Win11UI.hpp / AudioPlaybackConnector.cpp
void RefreshPanel();
winrt::fire_and_forget ReconnectAllAsync();

struct AttemptResult
{
	bool success = false;
	bool unreachable = false;
	std::wstring error;
};

void OnConnectionClosed(std::wstring const& id, uint64_t gen, AudioPlaybackConnection const& sender);
void ScheduleRetry(std::wstring const& id, uint64_t gen, std::wstring const& reason);
winrt::Windows::Foundation::IAsyncAction ConnectDeviceCoreAsync(DeviceInformation device);
winrt::fire_and_forget ReconnectAsync(std::wstring id, uint64_t gen, bool manual);
winrt::fire_and_forget WarmupSecondConnectAsync(std::wstring id, uint64_t gen);
winrt::fire_and_forget WarmupReopenAsync(std::wstring id, uint64_t gen);
winrt::Windows::Foundation::IAsyncAction WarmupReopenCoreAsync(std::wstring id, uint64_t gen);
winrt::Windows::Foundation::IAsyncAction RefreshDeviceCatalogAsync();
winrt::fire_and_forget OnCatalogChangedAsync();
void ProcessWatcherEvents();
void SetupDeviceWatcher();
void ProcessTimerTick();
winrt::Windows::Foundation::IAsyncOperation<bool> ProbeDevicePresentAsync(std::wstring id, bool forceProbe = false);
winrt::fire_and_forget VerifyAndScheduleRetryAsync(std::wstring id, uint64_t gen, std::wstring reason);
winrt::fire_and_forget PassiveProbeAsync(std::wstring id, uint64_t gen);
bool IsUnreachableHresult(int32_t code);

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

// Confirmed from the diagnostic log: with the phone's Bluetooth OFF,
// OpenAsync returns UnknownFailure with ERROR_GEN_FAILURE (0x8007001F) in
// 32-620 ms - NOT a timeout. The watcher reports no Removed event and
// FindAllAsync still lists the paired device, so this HRESULT is the only
// reliable "device is gone" signal on this machine.
bool IsUnreachableHresult(int32_t code)
{
	switch (static_cast<uint32_t>(code))
	{
	case 0x8007001F: // ERROR_GEN_FAILURE - device not functioning (observed)
	case 0x8007048F: // ERROR_DEVICE_NOT_CONNECTED
	case 0x80070015: // ERROR_NOT_READY
	case 0x800701B1: // ERROR_NO_SUCH_DEVICE
	case 0x80070102: // WAIT_TIMEOUT
	case 0x800705B4: // ERROR_TIMEOUT
		return true;
	default:
		return false;
	}
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
		LogW("PANEL", L"catalog refresh: " + std::to_wstring(g_availableDevices.size()) + L" paired audio devices");
	}
	catch (winrt::hresult_error const& ex)
	{
		LogW("PANEL", L"catalog refresh failed: " + FormatHresultError(ex));
		LOG_CAUGHT_EXCEPTION();
		g_availableDevices.clear();
	}
	RequestPanelRefresh();
}

// Presence check for a reconnect. IsPresent is intentionally NOT consulted:
// Windows reports false for a phone that is powered on but whose A2DP link is
// currently down, which incorrectly killed every legitimate reconnect.
winrt::Windows::Foundation::IAsyncOperation<bool> ProbeDevicePresentAsync(std::wstring id, bool forceProbe)
{
	const bool blocked = g_blockedDeviceIds.find(id) != g_blockedDeviceIds.end();
	LogW("PROBE", L"enter id=" + id + L" force=" + std::to_wstring(forceProbe ? 1 : 0) +
		L" blocked=" + std::to_wstring(blocked ? 1 : 0));

	if (!forceProbe && blocked)
	{
		LogW("PROBE", L"result=ABSENT (blocked by Removed or timed-out Open) id=" + id);
		co_return false;
	}

	try
	{
		// Plain enumeration, the same source as the panel catalog. Requesting
		// AEP properties changes which devices WinRT returns and can hide the
		// very device we are trying to reconnect.
		auto devices = co_await DeviceInformation::FindAllAsync(
			AudioPlaybackConnection::GetDeviceSelector());
		uint32_t count = 0;
		bool found = false;
		for (const auto& device : devices)
		{
			++count;
			if (std::wstring(device.Id()) == id)
				found = true;
		}
		LogW("PROBE", L"enum count=" + std::to_wstring(count) +
			std::wstring(L" target=") + (found ? L"FOUND" : L"NOT_FOUND") + L" id=" + id);
		if (found && forceProbe)
			g_blockedDeviceIds.erase(id);
		LogW("PROBE", std::wstring(L"result=") + (found ? L"PRESENT" : L"ABSENT") + L" id=" + id);
		co_return found;
	}
	catch (winrt::hresult_error const& ex)
	{
		LogW("PROBE", L"enum threw: " + FormatHresultError(ex));
		LOG_CAUGHT_EXCEPTION();
		co_return false;
	}
}

void MarkDeviceUnavailable(std::wstring const& id)
{
	LogW("STATE", L"MarkDeviceUnavailable -> stop fast retry loop, passive probe in 30 s id=" + id);
	g_blockedDeviceIds.insert(id);
	g_visibleDeviceIds.erase(id);
	auto it = g_deviceStates.find(id);
	if (it == g_deviceStates.end())
		return;
	auto& state = it->second;
	state.offline = true;
	// No visible countdown, no yellow "connecting" icon. A silent probe every
	// 30 s is the only way to notice the device coming back, because this
	// machine's DeviceWatcher never reports Added/Removed for Bluetooth power.
	state.pendingAction = PendingAction::ReappearProbe;
	state.actionAt = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	state.probing = false;
	state.phase = ConnectionPhase::Unavailable;
	state.status = _(L"Device unavailable");
	state.waitingForDevice = true;
	state.retryCount = 0;
}

// Drain watcher events on the UI thread. A Removed event (Bluetooth switched
// off / out of range) cancels any pending retry immediately and blocks the
// device. Added/Updated events unblock it.
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
		const bool wasBlocked = g_blockedDeviceIds.find(id) != g_blockedDeviceIds.end();
		LogW("WATCHER", std::wstring(L"apply ") + (event.second ? L"ADDED/UPDATED" : L"REMOVED") +
			L" id=" + id + L" wasBlocked=" + std::to_wstring(wasBlocked ? 1 : 0));
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
	LogW("WATCHER", L"waiting-for-device candidates=" + std::to_wstring(waitingIds.size()));

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
		LogW("WATCHER", L"device reappeared -> reconnect id=" + id);
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
		LogW("WATCHER", L"OnCatalogChanged threw: " + FormatHresultError(ex));
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
		LogW("WATCHER", L"event Added id=" + id);
		{
			std::lock_guard<std::mutex> lock(g_watcherMutex);
			g_watcherEvents.emplace_back(id, true);
		}
		PostMessageW(g_hWnd, WM_CATALOGCHANGED, 0, 0);
	});
	g_deviceWatcher.Updated([](auto const&, auto const& update) {
		auto id = std::wstring(update.Id());
		LogW("WATCHER", L"event Updated id=" + id);
		{
			std::lock_guard<std::mutex> lock(g_watcherMutex);
			g_watcherEvents.emplace_back(id, true);
		}
		PostMessageW(g_hWnd, WM_CATALOGCHANGED, 0, 0);
	});
	g_deviceWatcher.Removed([](auto const&, auto const& update) {
		auto id = std::wstring(update.Id());
		LogW("WATCHER", L"event Removed id=" + id);
		{
			std::lock_guard<std::mutex> lock(g_watcherMutex);
			g_watcherEvents.emplace_back(id, false);
		}
		PostMessageW(g_hWnd, WM_CATALOGCHANGED, 0, 0);
	});
	g_deviceWatcher.Start();
	LogW("WATCHER", L"DeviceWatcher started");
}

// Performs exactly one TryCreateFromId -> StartAsync -> OpenAsync cycle.
winrt::Windows::Foundation::IAsyncAction OpenOnceAsync(
	DeviceInformation device,
	uint64_t gen,
	std::shared_ptr<AttemptResult> result)
{
	auto id = std::wstring(device.Id());
	AudioPlaybackConnection connection = nullptr;
	const auto started = std::chrono::steady_clock::now();
	LogW("OPEN", L"begin id=" + id + L" gen=" + std::to_wstring(gen));

	try
	{
		connection = AudioPlaybackConnection::TryCreateFromId(device.Id());
		LogW("OPEN", std::wstring(L"TryCreateFromId -> ") + (connection ? L"ok" : L"NULL"));
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
			LogW("STATE", L"StateChanged state=" + std::to_wstring(static_cast<int>(sender.State())) +
				L" id=" + id + L" gen=" + std::to_wstring(gen));
			if (sender.State() == AudioPlaybackConnectionState::Closed)
				OnConnectionClosed(id, gen, sender);
		});

		co_await connection.StartAsync();
		LogW("OPEN", L"StartAsync completed id=" + id);
		auto open = co_await connection.OpenAsync();
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - started).count();
		LogW("OPEN", L"OpenAsync status=" + std::to_wstring(static_cast<int>(open.Status())) +
			L" elapsedMs=" + std::to_wstring(elapsed) + L" id=" + id);

		switch (open.Status())
		{
		case AudioPlaybackConnectionOpenResultStatus::Success:
			result->success = true;
			break;
		case AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
			result->unreachable = true;
			result->error = _(L"The request timed out");
			break;
		case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
			result->error = _(L"The operation was denied by the system");
			break;
		case AudioPlaybackConnectionOpenResultStatus::UnknownFailure:
			// The log shows status=3 with 0x8007001F when Bluetooth is OFF.
			result->unreachable = true;
			winrt::throw_hresult(open.ExtendedError());
			break;
		}
	}
	catch (winrt::hresult_error const& ex)
	{
		result->success = false;
		if (IsUnreachableHresult(static_cast<int32_t>(ex.code())))
			result->unreachable = true;
		result->error = FormatHresultError(ex);
		LogW("OPEN", L"threw unreachable=" + std::to_wstring(result->unreachable ? 1 : 0) +
			L": " + result->error + L" id=" + id);
		LOG_CAUGHT_EXCEPTION();
	}

	if (!result->success)
	{
		LogW("OPEN", L"failed, closing connection. error=" + result->error + L" id=" + id);
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
	{
		LogW("STATE", L"Closed IGNORED (stale/suppressed) id=" + id + L" gen=" + std::to_wstring(gen));
		return;
	}

	auto& state = stateIt->second;
	const bool wasBlocked = g_blockedDeviceIds.find(id) != g_blockedDeviceIds.end();
	LogW("STATE", L"Closed DECISION id=" + id + L" gen=" + std::to_wstring(gen) +
		L" manual=" + std::to_wstring(state.manualDisconnect ? 1 : 0) +
		L" autoReconnect=" + std::to_wstring(g_autoReconnect ? 1 : 0) +
		L" everConnected=" + std::to_wstring(state.everConnected ? 1 : 0) +
		L" wasBlocked=" + std::to_wstring(wasBlocked ? 1 : 0));

	auto it = g_audioPlaybackConnections.find(id);
	if (it != g_audioPlaybackConnections.end() && it->second.second == sender)
		g_audioPlaybackConnections.erase(it);
	sender.Close();

	if (state.manualDisconnect || !g_autoReconnect || !state.everConnected)
	{
		LogW("STATE", L"-> no reconnect (manual / disabled / never connected) id=" + id);
		state.phase = ConnectionPhase::Idle;
		state.status = _(L"Disconnected");
	}
	else
	{
		// A connection that just dropped proves the device was present a moment
		// ago. Clear any stale block so the reconnect is not wrongly vetoed.
		// If Bluetooth was switched off, the next Open() times out and blocks
		// it again immediately.
		g_blockedDeviceIds.erase(id);
		g_visibleDeviceIds.insert(id);
		state.offline = false;
		state.pendingAction = PendingAction::None;
		LogW("STATE", L"-> probe then retry id=" + id);
		VerifyAndScheduleRetryAsync(std::wstring(id), gen, _(L"Connection lost"));
	}
	RequestPanelRefresh();
}

void ScheduleRetry(std::wstring const& id, uint64_t gen, std::wstring const& reason)
{
	auto it = g_deviceStates.find(id);
	if (it == g_deviceStates.end() || it->second.generation != gen)
	{
		LogW("RETRY", L"ScheduleRetry aborted (state/gen changed) id=" + id);
		return;
	}

	auto& state = it->second;
	if (state.manualDisconnect || !g_autoReconnect)
	{
		LogW("RETRY", L"ScheduleRetry aborted (manual/disabled) id=" + id);
		return;
	}

	if (g_blockedDeviceIds.find(id) != g_blockedDeviceIds.end())
	{
		LogW("RETRY", L"ScheduleRetry aborted (blocked) id=" + id);
		MarkDeviceUnavailable(id);
		return;
	}

	if (g_maxRetries > 0 && state.retryCount >= g_maxRetries)
	{
		LogW("RETRY", L"retry limit reached id=" + id);
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
	LogW("RETRY", L"SCHEDULED id=" + id + L" attempt=" + std::to_wstring(state.retryCount) +
		L" delayMs=" + std::to_wstring(delayMs) + L" reason=" + reason);
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
		it->second.manualDisconnect || !g_autoReconnect || it->second.probing || it->second.offline)
	{
		LogW("RETRY", L"verify skipped id=" + id + L" gen=" + std::to_wstring(gen));
		co_return;
	}

	it->second.probing = true;
	it->second.pendingAction = PendingAction::None;
	it->second.phase = ConnectionPhase::Retrying;
	it->second.status = _(L"Waiting to reconnect");
	LogW("RETRY", L"verify start id=" + id + L" reason=" + reason);
	RequestPanelRefresh();

	const bool present = co_await ProbeDevicePresentAsync(id);

	// Re-validate after the await: a Removed event or user action may have
	// arrived while the probe was running.
	auto current = g_deviceStates.find(id);
	if (current == g_deviceStates.end())
		co_return;
	current->second.probing = false;
	if (current->second.generation != gen || current->second.manualDisconnect || !g_autoReconnect)
	{
		LogW("RETRY", L"verify aborted (state changed) id=" + id);
		RequestPanelRefresh();
		co_return;
	}
	if (g_blockedDeviceIds.find(id) != g_blockedDeviceIds.end())
	{
		LogW("RETRY", L"verify aborted (Removed arrived during probe) id=" + id);
		RequestPanelRefresh();
		co_return;
	}

	if (!present)
	{
		LogW("RETRY", L"verify FAILED -> unavailable id=" + id);
		MarkDeviceUnavailable(id);
		g_lastError = _(L"Device not found");
		RequestPanelRefresh();
		co_return;
	}
	LogW("RETRY", L"verify OK -> schedule id=" + id);
	ScheduleRetry(id, gen, reason);
	}
	catch (winrt::hresult_error const& ex)
	{
		LogW("RETRY", L"verify threw: " + FormatHresultError(ex));
		LOG_CAUGHT_EXCEPTION();
		g_lastError = FormatHresultError(ex);
		RequestPanelRefresh();
	}
}

void ReschedulePassiveProbe(std::wstring const& id, uint64_t gen)
{
	auto it = g_deviceStates.find(id);
	if (it == g_deviceStates.end() || !it->second.offline || it->second.generation != gen)
		return;
	it->second.pendingAction = PendingAction::ReappearProbe;
	it->second.actionAt = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	LogW("PROBE", L"still offline, next silent probe in 30 s id=" + id);
}

// Silent, low-frequency probe while a device is offline. It deliberately does
// NOT change the visible phase and does not show a countdown: the UI stays at
// "device unavailable". A single successful Open() restores the connection;
// a failure just schedules the next probe 30 s later. This is required because
// the DeviceWatcher on this machine reports no events when Bluetooth is turned
// off/on, so without it the device could never come back automatically.
winrt::fire_and_forget PassiveProbeAsync(std::wstring id, uint64_t gen)
{
	try
	{
	auto it = g_deviceStates.find(id);
	if (it == g_deviceStates.end() || !it->second.offline || it->second.generation != gen)
		co_return;
	if (it->second.manualDisconnect || !g_autoReconnect)
		co_return;

	auto device = FindDeviceInCatalog(id);
	if (!device)
		device = it->second.device;
	if (!device)
	{
		ReschedulePassiveProbe(id, gen);
		co_return;
	}

	LogW("PROBE", L"passive reappearance probe id=" + id);
	auto attempt = std::make_shared<AttemptResult>();
	co_await OpenOnceAsync(device, gen, attempt);

	auto current = g_deviceStates.find(id);
	if (current == g_deviceStates.end() || !current->second.offline ||
		current->second.generation != gen)
		co_return;

	if (attempt->success && g_audioPlaybackConnections.find(id) != g_audioPlaybackConnections.end())
	{
		LogW("CONNECT", L"device REAPPEARED -> connection restored id=" + id);
		current->second.offline = false;
		current->second.waitingForDevice = false;
		current->second.pendingAction = PendingAction::None;
		current->second.retryCount = 0;
		current->second.everConnected = true;
		g_blockedDeviceIds.erase(id);
		g_visibleDeviceIds.insert(id);
		g_lastError.clear();

		// Turning the radio back on is a COMPLETE Bluetooth reconnection, so it
		// needs the same second handshake as a first connect - otherwise the
		// A2DP endpoint reports "connected" but stays silent.
		if (g_autoWarmup)
		{
			current->second.warmupDone = false;
			current->second.warmupAttempts = 1;
			current->second.warmupReopenDelayMs = 1500;
			current->second.phase = ConnectionPhase::WarmingUp;
			current->second.status = _(L"Initializing audio (1/2)");
			current->second.pendingAction = PendingAction::WarmupSecondConnect;
			current->second.actionAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
			LogW("CONNECT", L"recovery after offline needs warm-up handshake id=" + id);
		}
		else
		{
			current->second.warmupDone = true;
			current->second.phase = ConnectionPhase::Connected;
			current->second.status = _(L"Connected");
		}
		RequestPanelRefresh();
		co_return;
	}

	ReschedulePassiveProbe(id, gen);
	}
	catch (winrt::hresult_error const& ex)
	{
		LogW("PROBE", L"passive probe threw: " + FormatHresultError(ex));
		LOG_CAUGHT_EXCEPTION();
		ReschedulePassiveProbe(id, gen);
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
	LogW("CONNECT", L"core begin id=" + id + L" gen=" + std::to_wstring(gen));
	RequestPanelRefresh();

	auto attempt = std::make_shared<AttemptResult>();
	co_await OpenOnceAsync(device, gen, attempt);

	auto it = g_deviceStates.find(id);
	if (it == g_deviceStates.end() || it->second.generation != gen)
	{
		LogW("CONNECT", L"core aborted (gen changed) id=" + id);
		co_return;
	}
	auto& current = it->second;

	const bool stillOpen = g_audioPlaybackConnections.find(id) != g_audioPlaybackConnections.end();
	LogW("CONNECT", L"core result id=" + id + L" success=" + std::to_wstring(attempt->success ? 1 : 0) +
		L" stillOpen=" + std::to_wstring(stillOpen ? 1 : 0) + L" error=" + attempt->error);
	if (!attempt->success || !stillOpen)
	{
		current.status = attempt->error.empty() ? std::wstring(_(L"Connection lost")) : attempt->error;
		g_lastError = current.status;
		const bool unreachable = attempt->unreachable;
		if (unreachable)
		{
			// Confirmed unreachable (timed out or 0x8007001F): stop the fast
			// backoff loop immediately and switch to the silent 30 s probe.
			LogW("CONNECT", L"device UNREACHABLE -> stop retry loop id=" + id);
			MarkDeviceUnavailable(id);
			RequestPanelRefresh();
			co_return;
		}
		VerifyAndScheduleRetryAsync(id, gen, current.status);
		RequestPanelRefresh();
		co_return;
	}

	const bool recoveredFromOffline = current.offline;
	current.everConnected = true;
	current.retryCount = 0;
	current.offline = false;
	current.waitingForDevice = false;
	current.pendingAction = PendingAction::None;
	g_blockedDeviceIds.erase(id);
	g_lastError.clear();
	LogW("CONNECT", L"CONNECTED id=" + id + L" recoveredFromOffline=" +
		std::to_wstring(recoveredFromOffline ? 1 : 0));

	if (recoveredFromOffline)
	{
		// Coming back after the radio was switched off needs the handshake
		// again, exactly like a first connection.
		current.warmupDone = false;
		current.warmupAttempts = 0;
	}

	if (g_autoWarmup && !current.warmupDone && current.warmupAttempts == 0)
	{
		// First successful Open() often reports "Connected" while the A2DP audio
		// endpoint is not streaming yet. Automate the manual "connect twice"
		// workaround once, silently, after a short settle period.
		current.warmupAttempts = 1;
		current.warmupReopenDelayMs = recoveredFromOffline ? 1500 : 600;
		current.phase = ConnectionPhase::WarmingUp;
		current.status = _(L"Initializing audio (1/2)");
		current.pendingAction = PendingAction::WarmupSecondConnect;
		// Give the freshly opened link 1.5 s to settle before tearing it down for
		// the handshake. 1.2 s was occasionally too short and left no audio.
		current.actionAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
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
		LogW("CONNECT", L"core threw: " + FormatHresultError(ex));
		LOG_CAUGHT_EXCEPTION();
		g_lastError = FormatHresultError(ex);
		RequestPanelRefresh();
	}
}

// Shared body of the warm-up reopen: open the connection again and finish.
winrt::Windows::Foundation::IAsyncAction WarmupReopenCoreAsync(std::wstring id, uint64_t gen)
{
	try
	{
	auto it = g_deviceStates.find(id);
	if (it == g_deviceStates.end() || it->second.generation != gen)
		co_return;
	auto device = it->second.device;
	if (!device)
		co_return;

	auto attempt = std::make_shared<AttemptResult>();
	co_await OpenOnceAsync(device, gen, attempt);

	auto current = g_deviceStates.find(id);
	if (current == g_deviceStates.end() || current->second.generation != gen)
		co_return;

	if (attempt->success && g_audioPlaybackConnections.find(id) != g_audioPlaybackConnections.end())
	{
		current->second.warmupDone = true;
		current->second.phase = ConnectionPhase::Connected;
		current->second.status = _(L"Connected");
		current->second.retryCount = 0;
		LogW("CONNECT", L"warm-up OK id=" + id);
	}
	else
	{
		current->second.phase = ConnectionPhase::Failed;
		current->second.status = attempt->error.empty() ? std::wstring(_(L"Connection lost")) : attempt->error;
		g_lastError = current->second.status;
		const bool unreachable = attempt->unreachable;
		LogW("CONNECT", L"warm-up FAILED unreachable=" + std::to_wstring(unreachable ? 1 : 0) +
			L" error=" + current->second.status + L" id=" + id);
		if (unreachable)
		{
			MarkDeviceUnavailable(id);
			RequestPanelRefresh();
			co_return;
		}
		if (current->second.retryCount < 3)
			VerifyAndScheduleRetryAsync(id, gen, current->second.status);
		else
			current->second.status = _(L"Automatic reconnect stopped");
	}
	RequestPanelRefresh();
	}
	catch (winrt::hresult_error const& ex)
	{
		LogW("CONNECT", L"warm-up reopen threw: " + FormatHresultError(ex));
		LOG_CAUGHT_EXCEPTION();
		g_lastError = FormatHresultError(ex);
		RequestPanelRefresh();
	}
}

// Warm-up stage 1: close the connection, then reopen it.
//
// The reopen timing matters and differs per scenario, which is why it is
// configurable (DeviceState::warmupReopenDelayMs):
//   * FIRST connection needs an IMMEDIATE reopen - waiting 1.5 s here made the
//     very first connection silent (the endpoint had fully torn down).
//   * Recovery after the Bluetooth radio was switched off needs a PAUSE, so the
//     stale A2DP endpoint is released before reopening.
winrt::fire_and_forget WarmupSecondConnectAsync(std::wstring id, uint64_t gen)
{
	try
	{
	auto it = g_deviceStates.find(id);
	if (it == g_deviceStates.end() || it->second.generation != gen)
		co_return;
	if (!it->second.device)
		co_return;

	auto& state = it->second;
	const int reopenDelay = state.warmupReopenDelayMs;
	LogW("CONNECT", L"warm-up stage 1: close, reopenDelay=" + std::to_wstring(reopenDelay) + L" id=" + id);

	const auto newGen = ++g_generationCounter;
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

	if (reopenDelay <= 0)
	{
		LogW("CONNECT", L"warm-up stage 2: immediate reopen id=" + id);
		co_await WarmupReopenCoreAsync(id, newGen);
		co_return;
	}

	state.pendingAction = PendingAction::WarmupReopen;
	state.actionAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(reopenDelay);
	}
	catch (winrt::hresult_error const& ex)
	{
		LogW("CONNECT", L"warm-up stage 1 threw: " + FormatHresultError(ex));
		LOG_CAUGHT_EXCEPTION();
		g_lastError = FormatHresultError(ex);
		RequestPanelRefresh();
	}
}

// Warm-up stage 2 (delayed path): reopen after the pause.
winrt::fire_and_forget WarmupReopenAsync(std::wstring id, uint64_t gen)
{
	try
	{
	co_await WarmupReopenCoreAsync(id, gen);
	}
	catch (winrt::hresult_error const& ex)
	{
		LogW("CONNECT", L"warm-up stage 2 threw: " + FormatHresultError(ex));
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
	{
		LogW("RETRY", L"ReconnectAsync skipped (state changed) id=" + id);
		co_return;
	}
	const bool offline = it->second.offline;
	LogW("RETRY", L"ReconnectAsync enter manual=" + std::to_wstring(manual ? 1 : 0) +
		L" offline=" + std::to_wstring(offline ? 1 : 0) + L" id=" + id);

	const bool present = co_await ProbeDevicePresentAsync(id, offline);

	auto current = g_deviceStates.find(id);
	if (current == g_deviceStates.end())
		co_return;
	if (!manual && (current->second.generation != gen || current->second.manualDisconnect || !g_autoReconnect))
		co_return;

	if (!present)
	{
		LogW("RETRY", L"ReconnectAsync probe failed -> unavailable id=" + id);
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
		LogW("RETRY", L"ReconnectAsync no DeviceInformation -> unavailable id=" + id);
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
		LogW("RETRY", L"ReconnectAsync threw: " + FormatHresultError(ex));
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
	LogW("CONNECT", L"manual ReconnectAll begin, known=" + std::to_wstring(g_deviceStates.size()) +
		L" saved=" + std::to_wstring(g_lastDevices.size()));
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
		LogW("CONNECT", L"ReconnectAll threw: " + FormatHresultError(ex));
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
	LogW("START", L"RestoreLastDevices count=" + std::to_wstring(ids.size()));
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
		LogW("START", L"RestoreLastDevices threw: " + FormatHresultError(ex));
		LOG_CAUGHT_EXCEPTION();
		g_lastError = FormatHresultError(ex);
		RequestPanelRefresh();
	}
}

void CloseDevice(std::wstring const& id, bool manual)
{
	LogW("CONNECT", L"CloseDevice manual=" + std::to_wstring(manual ? 1 : 0) + L" id=" + id);
	auto it = g_deviceStates.find(id);
	if (it != g_deviceStates.end())
	{
		auto& state = it->second;
		state.generation = ++g_generationCounter;
		state.pendingAction = PendingAction::None;
		state.probing = false;
		state.offline = false;
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
		const wchar_t* actionName = L"Unknown";
		if (item.action == PendingAction::Retry)
			actionName = L"Retry";
		else if (item.action == PendingAction::WarmupSecondConnect)
			actionName = L"WarmupSecondConnect";
		else if (item.action == PendingAction::WarmupReopen)
			actionName = L"WarmupReopen";
		else if (item.action == PendingAction::ReappearProbe)
			actionName = L"ReappearProbe";
		LogW("TIMER", std::wstring(L"fires ") + actionName +
			L" id=" + item.id + L" gen=" + std::to_wstring(item.gen));
		if (item.action == PendingAction::Retry)
			ReconnectAsync(item.id, item.gen, false);
		else if (item.action == PendingAction::WarmupSecondConnect)
			WarmupSecondConnectAsync(item.id, item.gen);
		else if (item.action == PendingAction::WarmupReopen)
			WarmupReopenAsync(item.id, item.gen);
		else if (item.action == PendingAction::ReappearProbe)
			PassiveProbeAsync(item.id, item.gen);
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
	LogW("START", L"retry timer started (250 ms)");
}
