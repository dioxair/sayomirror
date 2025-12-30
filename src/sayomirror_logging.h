#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace sayomirror::logging {
    std::wstring AsciiToWide(std::string_view str);
    std::filesystem::path BuildDailyLogPath();
    void LogLine(std::wstring_view message);
}
