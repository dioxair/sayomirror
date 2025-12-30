#include "sayomirror_capture.h"
#include "sayomirror.h"
#include "sayomirror_logging.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <thread>
#include <vector>

void sayomirror::capture::StopCaptureThread(sayomirror::AppState* appState) {
    if (!appState) {
        return;
    }
    appState->stop.store(true, std::memory_order_relaxed);
    if (appState->captureThread.joinable()) {
        appState->captureThread.join();
    }
}

void sayomirror::capture::StartCaptureThread(sayomirror::AppState* appState, const HWND hwnd) {
    if (!appState) {
        return;
    }
    appState->stop.store(false, std::memory_order_relaxed);
    appState->captureThread = std::thread([appState, hwnd] {
        using Clock = std::chrono::steady_clock;

        auto lastLog = Clock::now();
        auto windowStart = lastLog;
        uint32_t framesInWindow = 0;
        uint32_t lastFrameMs = 0;
        sayo::CaptureStats lastStats{};

        std::vector<uint8_t> frame;

        while (!appState->stop.load(std::memory_order_relaxed)) {
            bool isReady = false;
            {
                std::lock_guard<std::mutex> lock(appState->stateMutex);
                isReady = (appState->dev != nullptr) && (appState->srcW != 0) && (appState->srcH != 0);
            }

            if (!isReady) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            // should be exactly 25600 bytes on 160x80 displays!!!
            const size_t expectedFrameBytes =
                static_cast<size_t>(appState->srcW) * static_cast<size_t>(appState->srcH) * static_cast<size_t>(2);
            if (frame.capacity() < expectedFrameBytes) {
                frame.reserve(expectedFrameBytes);
            }
            frame.clear();

            sayo::CaptureStats stats{};
            const auto t0 = Clock::now();

            sayo::CaptureFrameResult captureResult = sayo::CaptureFrameResult::NoData;
            bool shouldNotifyDisconnect = false;
            {
                std::lock_guard<std::mutex> lock(appState->stateMutex);
                if (!appState->dev) {
                    captureResult = sayo::CaptureFrameResult::NoData;
                }
                else {
                    captureResult = sayo::CaptureScreenFrame(
                        appState->dev.get(),
                        appState->srcW,
                        appState->srcH,
                        appState->scratchIn, // reference
                        frame, // reference
                        &stats,
                        appState->proto);

                    if (captureResult == sayo::CaptureFrameResult::DeviceError) {
                        shouldNotifyDisconnect = true;
                        appState->stop.store(true, std::memory_order_relaxed);
                    }
                }
            }

            const auto t1 = Clock::now();
            lastFrameMs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

            if (captureResult == sayo::CaptureFrameResult::DeviceError) {
                if (shouldNotifyDisconnect) {
                    PostMessageW(hwnd, sayomirror::WM_APP_SAYODEVICE_DISCONNECTED, 0, 0);
                }
                break;
            }

            if (captureResult == sayo::CaptureFrameResult::Ok) {
                framesInWindow++;
                lastStats = stats;

                {
                    std::lock_guard<std::mutex> lock(appState->latestMutex);
                    appState->latestRgb565.swap(frame);
                }
                InvalidateRect(hwnd, nullptr, FALSE);

                const auto now = Clock::now();
                if (now - lastLog >= std::chrono::seconds(1)) {
                    const double secs = std::chrono::duration<double>(now - windowStart).count();
                    int fps = 0;
                    if (secs > 0.0) {
                        fps = static_cast<int>(std::lround(static_cast<double>(framesInWindow) / secs));
                    }
                    const unsigned long long expectedBytes =
                        static_cast<size_t>(appState->srcW) * static_cast<size_t>(appState->srcH) * 2ull;

                    sayomirror::logging::LogLine(std::format(
                        L"screen cap stats: {} fps, last={}ms, packets={}, bytes={}/{}",
                        fps,
                        lastFrameMs,
                        lastStats.packets,
                        lastStats.bytesCovered,
                        expectedBytes));

                    lastLog = now;
                    windowStart = now;
                    framesInWindow = 0;
                }
            }
            else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    });
}
