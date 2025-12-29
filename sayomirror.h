#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "resource.h"

#include "sayo_screen_capture.h"

struct hid_device;

namespace sayomirror {
	struct HidDeviceDeleter {
		void operator()(hid_device* d) const noexcept;
	};

	struct AppState {
		sayo::DeviceIds ids{};
		sayo::ProtocolConstants proto{};
		std::unique_ptr<hid_device, HidDeviceDeleter> dev;

		uint16_t srcW = 0;
		uint16_t srcH = 0;

		std::vector<uint8_t> scratchIn;
		std::vector<uint8_t> latestRgb565;
		std::mutex latestMutex;

		std::atomic<bool> stop{false};
		std::thread captureThread;
	};
}
