#pragma once

#include "framework.h"

#include <cstdint>
#include <optional>

namespace sayomirror {
	struct AppState;
}

namespace sayomirror::window_utils {
	enum class FitMode : uint8_t {
		BestIntegerScale = 0,
		Native1x,
	};

	std::optional<double> TryGetMonitorRefreshHz(HWND hwnd);
	double ComputeTargetPresentPeriodMs(HWND hwnd);
	UINT ComputeNextPresentDelayMs(sayomirror::AppState* appState);
	void FitWindowToDevice(HWND hwnd, uint16_t srcW, uint16_t srcH, FitMode mode);
}
