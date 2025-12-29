// sayomirror.cpp : Defines the entry point for the application.
//

#include "framework.h"
#include "sayomirror.h"
#include "sayo_screen_capture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "hidapi.h"

#define MAX_LOADSTRING 100

// Global Variables:
HINSTANCE hInst; // current instance
WCHAR szTitle[MAX_LOADSTRING]; // The title bar text
WCHAR szWindowClass[MAX_LOADSTRING]; // the main window class name

// Forward declarations of functions included in this code module:
ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);

namespace {
    constexpr UINT_PTR kPresentTimerId = 1;
    constexpr UINT kPresentTimerMs = 16; // ~60fps

    struct AppState {
        sayo::DeviceIds ids{};
        sayo::ProtocolConstants proto{};
        hid_device* dev = nullptr;

        uint16_t srcW = 0;
        uint16_t srcH = 0;

        std::vector<uint8_t> scratchIn;
        std::vector<uint8_t> latestRgb565;
        std::mutex latestMutex;

        std::atomic<bool> stop{ false };
        std::thread captureThread;

        wchar_t statusText[256] = L"";
    };

    void SetStatus(AppState* st, const wchar_t* text) {
        if (!st) {
            return;
        }
        if (!text) {
            st->statusText[0] = L'\0';
            return;
        }
        wcsncpy_s(st->statusText, text, _TRUNCATE);
    }

    void StopCaptureThread(AppState* st) {
        if (!st) {
            return;
        }
        st->stop.store(true, std::memory_order_relaxed);
        if (st->captureThread.joinable()) {
            st->captureThread.join();
        }
    }

    void CloseDevice(AppState* st) {
        if (!st) {
            return;
        }
        if (st->dev) {
            hid_close(st->dev);
            st->dev = nullptr;
        }
    }

    void StartCaptureThread(AppState* st, HWND hwnd) {
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
                const bool ok = sayo::CaptureScreenFrame(
                    st->dev,
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

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                      _In_opt_ HINSTANCE hPrevInstance,
                      _In_ LPWSTR lpCmdLine,
                      _In_ int nCmdShow) {
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // TODO: Place code here.

    // Initialize global strings
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_SAYOMIRROR, szWindowClass, MAX_LOADSTRING);
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

	auto* st = new AppState{};
    HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, st);

    if (!hWnd) {
		delete st;
        return FALSE;
    }

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
    auto* st = reinterpret_cast<AppState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
		return DefWindowProcW(hWnd, message, wParam, lParam);
    }
    case WM_CREATE: {
        st = reinterpret_cast<AppState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
        SetStatus(st, L"Opening device...");

        if (hid_init() != 0) {
            SetStatus(st, L"hid_init() failed.");
            break;
        }

        const sayo::OpenResult opened = sayo::OpenVendorInterface(st->ids, /*toStdErr*/ true);
        st->dev = opened.handle;
        if (!st->dev) {
            SetStatus(st, L"No compatible SayoDevice HID interface found.");
            break;
        }

        const auto lcd = sayo::TryGetLcdSize(st->dev, st->proto);
        if (!lcd) {
            SetStatus(st, L"Opened device, but LCD size query timed out.");
            break;
        }
        st->srcW = lcd->first;
        st->srcH = lcd->second;
        if (st->srcW == 0 || st->srcH == 0) {
            SetStatus(st, L"Device reported invalid LCD size.");
            break;
        }

        st->scratchIn.assign(st->proto.reportLen22, 0);
        st->latestRgb565.assign(static_cast<size_t>(st->srcW) * static_cast<size_t>(st->srcH) * 2, 0);

        SetStatus(st, L"");
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

        if (st->statusText[0] != L'\0') {
            DrawCenteredText(hdc, rc, st->statusText);
            EndPaint(hWnd, &ps);
            return 0;
        }

        if (!st->dev) {
            DrawCenteredText(hdc, rc, L"Device not opened.");
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
        st = reinterpret_cast<AppState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
        KillTimer(hWnd, kPresentTimerId);
        StopCaptureThread(st);
        CloseDevice(st);
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
