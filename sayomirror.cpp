// sayomirror.cpp : Defines the entry point for the application.
//

#include "framework.h"
#include "sayomirror.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

// Global Variables:
HINSTANCE hInst; // current instance
WCHAR szTitle[kMaxLoadString]; // The title bar text
WCHAR szWindowClass[kMaxLoadString]; // the main window class name

// Forward declarations of functions included in this code module:
ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);

namespace {
    constexpr UINT_PTR kPresentTimerId = 1;
    constexpr UINT kPresentTimerMs = 16; // ~60fps

    std::mutex g_logMutex;
    std::filesystem::path g_logPath;

    std::string ToUtf8(std::wstring_view s) {
        if (s.empty()) {
            return {};
        }
        const int needed = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr,
                                               nullptr);
        if (needed <= 0) {
            return {};
        }
        std::string out;
        out.resize(static_cast<size_t>(needed));
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed, nullptr, nullptr);
        return out;
    }

    std::filesystem::path GetExeDirectory() {
        std::wstring buf;
        buf.resize(MAX_PATH);
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return std::filesystem::current_path();
        }
        buf.resize(n);
        std::filesystem::path exePath(buf);
        return exePath.has_parent_path() ? exePath.parent_path() : std::filesystem::current_path();
    }

    std::filesystem::path BuildDailyLogPath() {
        const auto now = std::chrono::system_clock::now();
        const auto nowMs = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
        const std::time_t tt = std::chrono::system_clock::to_time_t(now);
        std::tm local{};
        localtime_s(&local, &tt);

        std::wostringstream name;
        name << L"sayomirror-log-" << std::put_time(&local, L"%Y-%m-%d") << L".log";
        return GetExeDirectory() / name.str();
    }

    std::string BuildTimestampPrefix() {
        const auto now = std::chrono::system_clock::now();
        const auto nowMs = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
        const auto msPart = static_cast<int>(nowMs.time_since_epoch().count() % 1000);
        const std::time_t tt = std::chrono::system_clock::to_time_t(now);
        std::tm local{};
        localtime_s(&local, &tt);

        std::ostringstream oss;
        oss << std::put_time(&local, "%Y-%m-%d %H:%M:%S")
            << '.' << std::setw(3) << std::setfill('0') << msPart;
        return oss.str();
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

    void StopCaptureThread(sayomirror::AppState* st) {
        if (!st) {
            return;
        }
        st->stop.store(true, std::memory_order_relaxed);
        if (st->captureThread.joinable()) {
            st->captureThread.join();
        }
    }

    void StartCaptureThread(sayomirror::AppState* st, HWND hwnd) {
        if (!st) {
            return;
        }
        st->stop.store(false, std::memory_order_relaxed);
        st->captureThread = std::thread([st, hwnd]() {
            while (!st->stop.load(std::memory_order_relaxed)) {
                if (!st->dev || st->srcW == 0 || st->srcH == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }

                std::vector<uint8_t> frame;
                sayo::CaptureStats stats{};
                const bool ok = CaptureScreenFrame(
                    st->dev.get(),
                    st->srcW,
                    st->srcH,
                    st->scratchIn,
                    frame,
                    &stats,
                    st->proto);

                if (ok) {
                    {
                        std::lock_guard<std::mutex> lock(st->latestMutex);
                        st->latestRgb565.swap(frame);
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
        });
    }

    void DrawCenteredText(HDC hdc, RECT rc, const wchar_t* text) {
        SetBkMode(hdc, TRANSPARENT);
        DrawTextW(hdc, text ? text : L"", -1, &rc, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
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

    // TODO: Place code here.

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
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
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

    auto st = std::make_unique<sayomirror::AppState>();
    HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, st.get());

    if (!hWnd) {
        return FALSE;
    }

    (void)st.release();

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
    auto* st = reinterpret_cast<sayomirror::AppState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
    case WM_CREATE: {
        st = reinterpret_cast<sayomirror::AppState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
        LogLine(L"Opening device...");

        if (hid_init() != 0) {
            LogLine(L"hid_init() failed.");
            break;
        }

        const sayo::OpenResult opened = OpenVendorInterface(st->ids, /*toStdErr*/ true);
        st->dev.reset(opened.handle);
        if (!st->dev) {
            LogLine(L"No compatible SayoDevice HID interface found.");
            break;
        }

        const std::optional<std::pair<uint16_t, uint16_t>> lcd = TryGetLcdSize(st->dev.get(), st->proto);
        if (!lcd) {
            LogLine(L"Opened device, but LCD size query timed out.");
            break;
        }
        st->srcW = lcd->first;
        st->srcH = lcd->second;
        if (st->srcW == 0 || st->srcH == 0) {
            LogLine(L"Device reported invalid LCD size.");
            break;
        }

        st->scratchIn.assign(st->proto.reportLen22, 0);
        st->latestRgb565.assign(static_cast<size_t>(st->srcW) * static_cast<size_t>(st->srcH) * 2, 0);

        const std::wstring okText = L"Capturing " + std::to_wstring(st->srcW) + L"x" + std::to_wstring(st->srcH);
        LogLine(okText);
        StartCaptureThread(st, hWnd);
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

        if (!st) {
            DrawCenteredText(hdc, rc, L"No app state.");
            EndPaint(hWnd, &ps);
            return 0;
        }

        if (!st->dev) {
            const auto logName = BuildDailyLogPath().filename().wstring();
            const std::wstring msg = L"Device not opened. Check " + logName;
            DrawCenteredText(hdc, rc, msg.c_str());
            EndPaint(hWnd, &ps);
            return 0;
        }

        const int clientW = rc.right - rc.left;
        const int clientH = rc.bottom - rc.top;
        const int scaleX = st->srcW ? (clientW / static_cast<int>(st->srcW)) : 1;
        const int scaleY = st->srcH ? (clientH / static_cast<int>(st->srcH)) : 1;
        const int scale = (std::max)(1, (std::min)(scaleX, scaleY));
        const int dstW = static_cast<int>(st->srcW) * scale;
        const int dstH = static_cast<int>(st->srcH) * scale;
        const int dstX = (clientW - dstW) / 2;
        const int dstY = (clientH - dstH) / 2;

        {
            std::lock_guard<std::mutex> lock(st->latestMutex);
            if (!st->latestRgb565.empty()) {
                sayo::BlitRgb565ToHdc(
                    hdc,
                    st->latestRgb565,
                    st->srcW,
                    st->srcH,
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
        st = reinterpret_cast<sayomirror::AppState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
        KillTimer(hWnd, kPresentTimerId);
        StopCaptureThread(st);
        if (st) {
            st->dev.reset();
        }
        hid_exit();
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        delete st;
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
