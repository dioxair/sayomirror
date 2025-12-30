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

        while (!appState->stop.load(std::memory_order_relaxed)) {
            if (!appState->dev || appState->srcW == 0 || appState->srcH == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            std::vector<uint8_t> frame;
            sayo::CaptureStats stats{};
            const auto t0 = Clock::now();
            const bool didCaptureFrame = CaptureScreenFrame(
                appState->dev.get(),
                appState->srcW,
                appState->srcH,
                appState->scratchIn,
                frame,
                &stats,
                appState->proto);
            const auto t1 = Clock::now();
            lastFrameMs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

            if (didCaptureFrame) {
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
