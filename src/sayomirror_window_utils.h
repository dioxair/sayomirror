#pragma once

#include "framework.h"

#include <cstdint>
#include <optional>

namespace sayomirror {
	struct AppState;
}

namespace sayomirror::window_utils {
	std::optional<double> TryGetMonitorRefreshHz(HWND hwnd);
	double ComputeTargetPresentPeriodMs(HWND hwnd);
	UINT ComputeNextPresentDelayMs(sayomirror::AppState* appState);
	void FitWindowToDevice(HWND hwnd, uint16_t srcW, uint16_t srcH);
}
