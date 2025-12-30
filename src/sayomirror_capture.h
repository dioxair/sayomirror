#pragma once

#include "framework.h"

namespace sayomirror {
    struct AppState;
    
    constexpr UINT WM_APP_SAYODEVICE_DISCONNECTED = WM_APP + 1;
}

namespace sayomirror::capture {
    void StartCaptureThread(sayomirror::AppState* appState, HWND hwnd);
    void StopCaptureThread(sayomirror::AppState* appState);
}
