
#include "sayomirror_window_utils.h"

#include "sayomirror.h"

#include <algorithm>
#include <cmath>

namespace {
	constexpr UINT kPresentTimerFallbackMs = 16; // ~60fps
}

std::optional<double> sayomirror::window_utils::TryGetMonitorRefreshHz(const HWND hwnd) {
	if (!hwnd) {
		return std::nullopt;
	}

	const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFOEXW mi{};
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoW(monitor, &mi)) {
		return std::nullopt;
	}

	DEVMODEW dm{};
	dm.dmSize = sizeof(dm);
	if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
		return std::nullopt;
	}

	if (dm.dmDisplayFrequency <= 1) {
		return std::nullopt;
	}
	return dm.dmDisplayFrequency;
}

double sayomirror::window_utils::ComputeTargetPresentPeriodMs(const HWND hwnd) {
	const auto hz = TryGetMonitorRefreshHz(hwnd);
	if (!hz || *hz <= 0.0) {
		return 0.0;
	}
	return 1000.0 / *hz;
}

UINT sayomirror::window_utils::ComputeNextPresentDelayMs(sayomirror::AppState* appState) {
	if (!appState || appState->presentTargetPeriodMs <= 0.0) {
		return kPresentTimerFallbackMs;
	}

	const double target = appState->presentTargetPeriodMs;
	const double baseD = (std::max)(1.0, std::floor(target));
	const double frac = (std::max)(0.0, target - baseD);

	appState->presentFracAccumulatorMs += frac;
	UINT delay = static_cast<UINT>(baseD);
	if (appState->presentFracAccumulatorMs >= 1.0) {
		delay += 1;
		appState->presentFracAccumulatorMs -= 1.0;
	}

	return (std::max)(1u, delay);
}

void sayomirror::window_utils::FitWindowToDevice(const HWND hwnd, const uint16_t srcW, const uint16_t srcH, const FitMode mode) {
	if (!hwnd || srcW == 0 || srcH == 0) {
		return;
	}

	const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO monitorInfo{};
	monitorInfo.cbSize = sizeof(monitorInfo);
	if (!GetMonitorInfoW(monitor, &monitorInfo)) {
		return;
	}

	const int workW = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
	const int workH = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;
	if (workW <= 0 || workH <= 0) {
		return;
	}

	const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

	const int maxScaleByClient = (std::max)(
		1,
		(std::min)(workW / static_cast<int>(srcW), workH / static_cast<int>(srcH)));

	int chosenScale = 1;
	if (mode == FitMode::BestIntegerScale) {
		for (int scale = maxScaleByClient; scale >= 1; --scale) {
			RECT windowRect{0, 0, static_cast<LONG>(srcW) * scale, static_cast<LONG>(srcH) * scale};
			if (!AdjustWindowRectEx(&windowRect, static_cast<DWORD>(style), FALSE, static_cast<DWORD>(exStyle))) {
				continue;
			}

			const int winW = windowRect.right - windowRect.left;
			const int winH = windowRect.bottom - windowRect.top;
			if (winW <= workW && winH <= workH) {
				chosenScale = scale;
				break;
			}
		}
	}

	RECT windowRect{0, 0, static_cast<LONG>(srcW) * chosenScale, static_cast<LONG>(srcH) * chosenScale};
	if (!AdjustWindowRectEx(&windowRect, static_cast<DWORD>(style), FALSE, static_cast<DWORD>(exStyle))) {
		return;
	}

	const int winW = windowRect.right - windowRect.left;
	const int winH = windowRect.bottom - windowRect.top;

	RECT currentRect{};
	int x = monitorInfo.rcWork.left;
	int y = monitorInfo.rcWork.top;
	if (GetWindowRect(hwnd, &currentRect)) {
		x = currentRect.left;
		y = currentRect.top;
	}

	const int minX = monitorInfo.rcWork.left;
	const int maxX = monitorInfo.rcWork.right - winW;
	const int minY = monitorInfo.rcWork.top;
	const int maxY = monitorInfo.rcWork.bottom - winH;

	if (x < minX) x = minX;
	if (x > maxX) x = maxX;
	if (y < minY) y = minY;
	if (y > maxY) y = maxY;

	SetWindowPos(hwnd, nullptr, x, y, winW, winH, SWP_NOZORDER | SWP_NOACTIVATE);
}
