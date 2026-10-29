#include "display/FullscreenDetector.h"
#include <dwmapi.h>
#include <cwchar>
#include <iterator>

namespace pms {

namespace {

bool IsShellOrSystemOverlayWindow(HWND hwnd) {
    wchar_t cls[128] = {};
    if (GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))) <= 0) {
        return false;
    }

    static const wchar_t* const kIgnoredClasses[] = {
        L"Progman",
        L"WorkerW",
        L"Shell_TrayWnd",
        L"Shell_SecondaryTrayWnd",
        L"Windows.UI.Core.CoreWindow",
        L"XamlExplorerHostIslandWindow",
        L"MultitaskingViewFrame",
        L"ForegroundStaging",
        L"TaskListThumbnailWnd"
    };

    for (const wchar_t* ignored : kIgnoredClasses) {
        if (wcscmp(cls, ignored) == 0) return true;
    }
    return false;
}

bool EqualRectExact(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

} // namespace

bool FullscreenDetector::EvaluateWindowFullscreenOnMonitor(
    const RECT& windowBounds,
    const RECT& monitorBounds,
    const RECT& workAreaBounds,
    LONG_PTR windowStyle,
    bool isZoomed)
{
    // Window must cover the entire physical monitor (allowing 1px DWM rounding tolerance)
    constexpr LONG kTol = 1;
    bool coversMonitor =
        (windowBounds.left   <= monitorBounds.left   + kTol) &&
        (windowBounds.top    <= monitorBounds.top    + kTol) &&
        (windowBounds.right  >= monitorBounds.right  - kTol) &&
        (windowBounds.bottom >= monitorBounds.bottom - kTol);

    if (!coversMonitor) {
        return false;
    }

    // If the user has "Automatically hide the taskbar" enabled, workAreaBounds == monitorBounds.
    // In that setup, a standard maximized desktop app (IsZoomed && WS_CAPTION) covers monitorBounds
    // and should NOT be treated as a fullscreen game.
    bool autoHideTaskbar = EqualRectExact(monitorBounds, workAreaBounds);
    if (autoHideTaskbar && isZoomed && ((windowStyle & WS_CAPTION) == WS_CAPTION)) {
        return false;
    }

    return true;
}

bool FullscreenDetector::IsMonitorCoveredByFullscreenApp(HMONITOR targetMonitor, DWORD selfProcessId) {
    if (!targetMonitor) return false;

    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindowVisible(fg) || IsIconic(fg)) {
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == selfProcessId) {
        return false;
    }

    if (IsShellOrSystemOverlayWindow(fg)) {
        return false;
    }

    HMONITOR fgMon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    if (fgMon != targetMonitor) {
        return false;
    }

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(targetMonitor, &mi)) {
        return false;
    }

    RECT bounds{};
    if (FAILED(DwmGetWindowAttribute(fg, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) {
        if (!GetWindowRect(fg, &bounds)) {
            return false;
        }
    }

    LONG_PTR style = GetWindowLongPtrW(fg, GWL_STYLE);
    bool zoomed = IsZoomed(fg) != FALSE;

    return EvaluateWindowFullscreenOnMonitor(bounds, mi.rcMonitor, mi.rcWork, style, zoomed);
}

} // namespace pms
