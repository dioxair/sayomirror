
#include "framework.h"
#include "sayomirror_logging.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace {
    std::mutex g_logMutex;
    std::filesystem::path g_logPath;

    std::string ToUtf8(const std::wstring_view str) {
        if (str.empty()) {
            return {};
        }
        const int needed = WideCharToMultiByte(
            CP_UTF8,
            0,
            str.data(),
            static_cast<int>(str.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (needed <= 0) {
            return {};
        }
        std::string out;
        out.resize(static_cast<size_t>(needed));
        WideCharToMultiByte(
            CP_UTF8,
            0,
            str.data(),
            static_cast<int>(str.size()),
            out.data(),
            needed,
            nullptr,
            nullptr);
        return out;
    }

    std::filesystem::path GetExeDirectory() {
        std::wstring modulePath;
        modulePath.resize(MAX_PATH);
        const DWORD modulePathLen = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
        if (modulePathLen == 0) {
            return std::filesystem::current_path();
        }
        modulePath.resize(modulePathLen);
        std::filesystem::path exePath(modulePath);
        return exePath.has_parent_path() ? exePath.parent_path() : std::filesystem::current_path();
    }

    std::string BuildTimestampPrefix() {
        const auto now = std::chrono::system_clock::now();
        const auto nowMs = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
        const auto msPart = static_cast<int>(nowMs.time_since_epoch().count() % 1000);
        const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
        std::tm local{};
        (void)localtime_s(&local, &nowTime);

        std::ostringstream timestampStream;
        timestampStream << std::put_time(&local, "%Y-%m-%d %H:%M:%S")
                        << '.' << std::setw(3) << std::setfill('0') << msPart;
        return timestampStream.str();
    }
}

std::wstring sayomirror::logging::AsciiToWide(const std::string_view str) {
    std::wstring out;
    out.reserve(str.size());
    for (const unsigned char c : str) {
        out.push_back(c);
    }
    return out;
}

std::filesystem::path sayomirror::logging::BuildDailyLogPath() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    (void)localtime_s(&local, &nowTime);

    std::wostringstream name;
    name << L"sayomirror-log-" << std::put_time(&local, L"%Y-%m-%d") << L".log";
    return GetExeDirectory() / name.str();
}

void sayomirror::logging::LogLine(const std::wstring_view message) {
    if (message.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logPath.empty()) {
        g_logPath = BuildDailyLogPath();
    }

    std::ofstream out(g_logPath, std::ios::app | std::ios::binary);
    if (!out) {
        return;
    }
    out << BuildTimestampPrefix() << "  " << ToUtf8(message) << "\n";
}
