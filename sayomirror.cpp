// sayomirror.cpp : Defines the entry point for the application.
//

#include "framework.h"
#include "sayomirror.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <format>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "hidapi.h"

constexpr int kMaxLoadString = 100;

HINSTANCE hInst;
WCHAR szTitle[kMaxLoadString];
WCHAR szWindowClass[kMaxLoadString];

ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);

namespace {
    constexpr UINT_PTR kPresentTimerId = 1;
    constexpr UINT kPresentTimerMs = 16; // ~60fps

    std::mutex g_logMutex;
    std::filesystem::path g_logPath;

    std::wstring AsciiToWide(const std::string_view str) {
        std::wstring out;
        out.reserve(str.size());
        for (const unsigned char c : str) {
            out.push_back(c);
        }
        return out;
    }

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

    std::filesystem::path BuildDailyLogPath() {
        const auto now = std::chrono::system_clock::now();
        const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
        std::tm local{};
        // cast to void cause idgaf about using the return value
        (void)localtime_s(&local, &nowTime);

        std::wostringstream name;
        name << L"sayomirror-log-" << std::put_time(&local, L"%Y-%m-%d") << L".log";
        return GetExeDirectory() / name.str();
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

    void LogLine(std::wstring_view message) {
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

    void StopCaptureThread(sayomirror::AppState* appState) {
        if (!appState) {
            return;
        }
        appState->stop.store(true, std::memory_order_relaxed);
        if (appState->captureThread.joinable()) {
            appState->captureThread.join();
        }
    }

    void StartCaptureThread(sayomirror::AppState* appState, HWND hwnd) {
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

                        LogLine(std::format(
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
}

void sayomirror::HidDeviceDeleter::operator()(hid_device* d) const noexcept {
    if (d) {
        hid_close(d);
    }
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                      _In_opt_ HINSTANCE hPrevInstance,
                      _In_ LPWSTR lpCmdLine,
                      _In_ int nCmdShow) {
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // Initialize global strings
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, kMaxLoadString);
    LoadStringW(hInstance, IDC_SAYOMIRROR, szWindowClass, kMaxLoadString);
    MyRegisterClass(hInstance);

    // Perform application initialization:
    if (!InitInstance(hInstance, nCmdShow)) {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_SAYOMIRROR));

    MSG msg;

    // Main message loop:
    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return static_cast<int>(msg.wParam);
}


//
//  FUNCTION: MyRegisterClass()
//
//  PURPOSE: Registers the window class.
//
ATOM MyRegisterClass(HINSTANCE hInstance) {
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SAYOMIRROR));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = reinterpret_cast<HBRUSH>((COLOR_WINDOW + 1));
    wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_SAYOMIRROR);
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

//
//   FUNCTION: InitInstance(HINSTANCE, int)
//
//   PURPOSE: Saves instance handle and creates main window
//
//   COMMENTS:
//
//        In this function, we save the instance handle in a global variable and
//        create and display the main program window.
//
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow) {
    hInst = hInstance; // Store instance handle in our global variable

    auto appState = std::make_unique<sayomirror::AppState>();
    HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, appState.get());

    if (!hWnd) {
        return FALSE;
    }

    (void)appState.release();

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    return TRUE;
}

//
//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  PURPOSE: Processes messages for the main window.
//
//  WM_COMMAND  - process the application menu
//  WM_PAINT    - Paint the main window
//  WM_DESTROY  - post a quit message and return
//
//
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* appState = reinterpret_cast<sayomirror::AppState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
    case WM_CREATE: {
        appState = reinterpret_cast<sayomirror::AppState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

        if (hid_init() != 0) {
            LogLine(L"hid_init() failed.");
            break;
        }

        const sayo::OpenResult opened = OpenVendorInterface(appState->ids, sayo::OutputStream::StdOut);
        appState->dev.reset(opened.handle);
        if (!appState->dev) {
            LogLine(L"No compatible SayoDevice HID interface found.");
            break;
        }

        LogLine(std::format(
            L"Opening path: {} (interface={} usage_page=0x{:x} usage=0x{:x})",
            AsciiToWide(opened.openedPath),
            opened.interfaceNumber,
            static_cast<unsigned>(opened.usagePage),
            static_cast<unsigned>(opened.usage)));

        wchar_t buf[256]{};
        if (hid_get_manufacturer_string(appState->dev.get(), buf, _countof(buf)) == 0) {
            LogLine(std::format(L"Manufacturer String: {}", std::wstring_view(buf)));
        }
        if (hid_get_product_string(appState->dev.get(), buf, _countof(buf)) == 0) {
            LogLine(std::format(L"Product String: {}", std::wstring_view(buf)));
        }
        if (hid_get_serial_number_string(appState->dev.get(), buf, _countof(buf)) == 0) {
            LogLine(std::format(L"Serial Number String: {}", std::wstring_view(buf)));
        }

        const std::optional<std::pair<uint16_t, uint16_t>> lcd = TryGetLcdSize(appState->dev.get(), appState->proto);
        if (!lcd) {
            LogLine(L"Opened device, but LCD size query timed out.");
            break;
        }
        appState->srcW = lcd->first;
        appState->srcH = lcd->second;
        if (appState->srcW == 0 || appState->srcH == 0) {
            LogLine(L"Device reported invalid LCD size.");
            break;
        }

        LogLine(std::format(L"LCD size reported by device: {}x{}", appState->srcW, appState->srcH));

        appState->scratchIn.assign(appState->proto.reportLen22, 0);
        appState->latestRgb565.assign(static_cast<size_t>(appState->srcW) * static_cast<size_t>(appState->srcH) * 2, 0);

        StartCaptureThread(appState, hWnd);
        SetTimer(hWnd, kPresentTimerId, kPresentTimerMs, nullptr);
        return 0;
    }
    case WM_COMMAND: {
        // Parse the menu selections:
        switch (int wmId = LOWORD(wParam)) {
        case IDM_ABOUT:
            DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
            break;
        case IDM_EXIT:
            DestroyWindow(hWnd);
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
    }
    break;
    case WM_TIMER:
        if (wParam == kPresentTimerId) {
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT rc{};
        GetClientRect(hWnd, &rc);

        if (!appState) {
            EndPaint(hWnd, &ps);
            return 0;
        }

        if (!appState->dev) {
            const auto logName = BuildDailyLogPath().filename().wstring();
            const std::wstring msg = L"Device not opened. Check " + logName;
            EndPaint(hWnd, &ps);
            return 0;
        }

        const int clientW = rc.right - rc.left;
        const int clientH = rc.bottom - rc.top;
        const int scaleX = appState->srcW ? (clientW / static_cast<int>(appState->srcW)) : 1;
        const int scaleY = appState->srcH ? (clientH / static_cast<int>(appState->srcH)) : 1;
        const int scale = (std::max)(1, (std::min)(scaleX, scaleY));
        const int dstW = static_cast<int>(appState->srcW) * scale;
        const int dstH = static_cast<int>(appState->srcH) * scale;
        const int dstX = (clientW - dstW) / 2;
        const int dstY = (clientH - dstH) / 2;

        {
            std::lock_guard<std::mutex> lock(appState->latestMutex);
            if (!appState->latestRgb565.empty()) {
                sayo::BlitRgb565ToHdc(
                    hdc,
                    appState->latestRgb565,
                    appState->srcW,
                    appState->srcH,
                    dstX,
                    dstY,
                    dstW,
                    dstH);
            }
        }

        EndPaint(hWnd, &ps);
        return 0;
    }
    break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY: {
        appState = reinterpret_cast<sayomirror::AppState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
        KillTimer(hWnd, kPresentTimerId);
        StopCaptureThread(appState);
        if (appState) {
            appState->dev.reset();
        }
        hid_exit();
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        delete appState;
        return 0;
    }
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// Message handler for about box.
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    UNREFERENCED_PARAMETER(lParam);
    switch (message) {
    case WM_INITDIALOG:
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, LOWORD(wParam));
            return TRUE;
        }
        break;
    }
    return FALSE;
}
