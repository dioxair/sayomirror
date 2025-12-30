#pragma once

#include "framework.h"

namespace sayomirror {
    struct AppState;
}

namespace sayomirror::capture {
    void StartCaptureThread(sayomirror::AppState* appState, HWND hwnd);
    void StopCaptureThread(sayomirror::AppState* appState);
}
