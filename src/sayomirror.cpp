// sayomirror.cpp : Defines the entry point for the application.
//

#include "framework.h"
#include "sayomirror.h"

#include "sayomirror_capture.h"
#include "sayomirror_logging.h"
#include "sayomirror_window_utils.h"

#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hidapi.h"

constexpr int kMaxLoadString = 100;

HINSTANCE hInst;
WCHAR szTitle[kMaxLoadString];
WCHAR szWindowClass[kMaxLoadString];

ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

namespace {
    constexpr UINT_PTR kPresentTimerId = 1;
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

    MSG msg;

    // Main message loop:
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
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

    wcex.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SAYOMIRROR));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wcex.lpszMenuName = nullptr;
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
            sayomirror::logging::LogLine(L"hid_init() failed.");
            break;
        }

        const sayo::OpenResult opened = OpenVendorInterface(appState->ids, sayo::OutputStream::StdOut);
        appState->dev.reset(opened.handle);
        if (!appState->dev) {
            appState->statusText =
                L"No compatible SayoDevice HID interface found :(\n\n"
                L"Please make a GitHub Issue at https://github.com/dioxair/sayomirror and follow the directions to report this bug";
            sayomirror::logging::LogLine(appState->statusText);
            break;
        }

        sayomirror::logging::LogLine(std::format(
            L"Opening path: {} (interface={} usage_page=0x{:x} usage=0x{:x})",
            sayomirror::logging::AsciiToWide(opened.openedPath),
            opened.interfaceNumber,
            static_cast<unsigned>(opened.usagePage),
            static_cast<unsigned>(opened.usage)));

        wchar_t buf[256]{};
        if (hid_get_manufacturer_string(appState->dev.get(), buf, _countof(buf)) == 0) {
            sayomirror::logging::LogLine(std::format(L"Manufacturer String: {}", std::wstring_view(buf)));
        }
        if (hid_get_product_string(appState->dev.get(), buf, _countof(buf)) == 0) {
            sayomirror::logging::LogLine(std::format(L"Product String: {}", std::wstring_view(buf)));
        }
#if _DEBUG
        if (hid_get_serial_number_string(appState->dev.get(), buf, _countof(buf)) == 0) {
            sayomirror::logging::LogLine(std::format(L"Serial Number String: {}", std::wstring_view(buf)));
        }
#endif

        const std::optional<std::pair<uint16_t, uint16_t>> lcd = TryGetLcdSize(appState->dev.get(), appState->proto);
        if (!lcd) {
            sayomirror::logging::LogLine(L"Opened device, but LCD size query timed out.");
            break;
        }
        appState->srcW = lcd->first;
        appState->srcH = lcd->second;
        if (appState->srcW == 0 || appState->srcH == 0) {
            sayomirror::logging::LogLine(L"Device reported invalid LCD size.");
            break;
        }

        sayomirror::logging::LogLine(std::format(L"LCD size reported by device: {}x{}", appState->srcW,
                                                 appState->srcH));

        sayomirror::window_utils::FitWindowToDevice(hWnd, appState->srcW, appState->srcH);

        appState->presentTargetPeriodMs = sayomirror::window_utils::ComputeTargetPresentPeriodMs(hWnd);
        if (appState->presentTargetPeriodMs > 0.0) {
            const double hz = 1000.0 / appState->presentTargetPeriodMs;
            sayomirror::logging::LogLine(std::format(L"monitor refresh (approx): {:.3f} Hz (target {:.3f} ms)", hz,
                                                     appState->presentTargetPeriodMs));
        }

        appState->scratchIn.assign(appState->proto.reportLen22, 0);
        appState->latestRgb565.assign(static_cast<size_t>(appState->srcW) * static_cast<size_t>(appState->srcH) * 2, 0);

        sayomirror::capture::StartCaptureThread(appState, hWnd);
        SetTimer(hWnd, kPresentTimerId, sayomirror::window_utils::ComputeNextPresentDelayMs(appState), nullptr);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kPresentTimerId) {
            InvalidateRect(hWnd, nullptr, FALSE);

            if (appState && appState->presentTargetPeriodMs > 0.0) {
                KillTimer(hWnd, kPresentTimerId);
                SetTimer(hWnd, kPresentTimerId, sayomirror::window_utils::ComputeNextPresentDelayMs(appState), nullptr);
            }
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        // When the device is open, WM_PAINT blits the full client area so we
        // suppress background erases to reduce flicker. In error/not-opened
        // states, let DefWindowProc erase to the class background brush.
        if (appState && appState->dev) {
            return 1;
        }
        return DefWindowProc(hWnd, message, wParam, lParam);
    case WM_LBUTTONDBLCLK:
        if (appState && appState->srcW && appState->srcH) {
            sayomirror::window_utils::FitWindowToDevice(hWnd, appState->srcW, appState->srcH);
        }
        return 0;
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
            if (appState->statusText.empty()) {
                const auto logName = sayomirror::logging::BuildDailyLogPath().filename().wstring();
                appState->statusText = L"Device not opened. Check " + logName;
            }

            FillRect(hdc, &rc, GetSysColorBrush(COLOR_WINDOW));
            (void)SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));

            (void)SetBkMode(hdc, TRANSPARENT);
            RECT textRc = rc;
            constexpr int pad = 12;
            textRc.left += pad;
            textRc.right -= pad;
            textRc.top += pad;
            textRc.bottom -= pad;

            const int availW = (std::max)(0L, textRc.right - textRc.left);
            const int availH = (std::max)(0L, textRc.bottom - textRc.top);
            const wchar_t* text = appState->statusText.c_str();
            const int textLen = static_cast<int>(appState->statusText.size());

            HFONT chosenFont = nullptr;
            HGDIOBJ prevFont = nullptr;

            const int startPx = (std::max)(18, availH / 7);
            constexpr int minPx = 12;
            for (int px = startPx; px >= minPx; px -= 2) {
                const HFONT tryFont = CreateFontW(
                    -px,
                    0,
                    0,
                    0,
                    FW_SEMIBOLD,
                    FALSE,
                    FALSE,
                    FALSE,
                    DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE,
                    L"Segoe UI");
                if (!tryFont) {
                    continue;
                }

                const HGDIOBJ prev = SelectObject(hdc, tryFont);
                RECT calc = {0, 0, availW, 0};
                const int calcH = DrawTextW(
                    hdc,
                    text,
                    textLen,
                    &calc,
                    DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);

                const int needW = static_cast<int>(calc.right - calc.left);
                const int needH = calcH;

                SelectObject(hdc, prev);
                if (needW <= availW && needH <= availH) {
                    chosenFont = tryFont;
                    break;
                }
                DeleteObject(tryFont);
            }

            if (!chosenFont) {
                chosenFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            }

            prevFont = chosenFont ? SelectObject(hdc, chosenFont) : nullptr;

            DrawTextW(
                hdc,
                text,
                textLen,
                &textRc,
                DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);

            if (prevFont) {
                SelectObject(hdc, prevFont);
            }

            if (chosenFont && chosenFont != GetStockObject(DEFAULT_GUI_FONT)) {
                DeleteObject(chosenFont);
            }
            EndPaint(hWnd, &ps);
            return 0;
        }

        const int clientW = rc.right - rc.left;
        const int clientH = rc.bottom - rc.top;
        constexpr int dstX = 0;
        constexpr int dstY = 0;
        const int dstW = clientW;
        const int dstH = clientH;

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
        sayomirror::capture::StopCaptureThread(appState);
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
