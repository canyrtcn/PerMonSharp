#pragma once

#include <windows.h>

namespace pms {

class FullscreenDetector {
public:
    // Pure geometric/style evaluation helper (exposed for deterministic unit testing)
    static bool EvaluateWindowFullscreenOnMonitor(
        const RECT& windowBounds,
        const RECT& monitorBounds,
        const RECT& workAreaBounds,
        LONG_PTR windowStyle,
        bool isZoomed);

    // Checks whether the foreground window is currently running fullscreen (game, F11 browser, media player)
    // on the specified monitor, ignoring desktop shell surfaces and normal maximized windows.
    static bool IsMonitorCoveredByFullscreenApp(HMONITOR targetMonitor, DWORD selfProcessId);
};

} // namespace pms
