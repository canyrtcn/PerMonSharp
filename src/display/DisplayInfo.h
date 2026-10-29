#pragma once

#include <windows.h>
#include <dxgi1_6.h>
#include <string>
#include <cstdint>

namespace pms {

struct DisplayInfo {
    std::string id;                // Persistent normalized hardware path / EDID key
    std::wstring gdiDeviceName;    // e.g. L"\\\\.\\DISPLAY1"
    std::wstring friendlyName;     // Raw EDID model name
    std::wstring displayLabel;     // Disambiguated UI label (e.g. appends " #1" for identical models)
    std::wstring adapterName;      // Physical GPU driving this output

    HMONITOR hMonitor = nullptr;
    LUID adapterLuid{};
    uint32_t adapterIndex = 0;
    uint32_t outputIndex = 0;

    RECT desktopBounds{};          // Physical pixel bounds in virtual screen coordinates
    int width = 0;                 // Desktop width in physical pixels
    int height = 0;                // Desktop height in physical pixels
    double refreshRateHz = 60.0;   // Exact refresh rate (e.g. 144.0, 59.95 -> 60 Hz display)
    uint32_t dpiPercent = 100;     // Effective DPI scale (100, 125, 150, 200...)
    uint32_t bitsPerColor = 8;

    DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY;
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bool isPrimary = false;
    bool isHdrActive = false;
};

} // namespace pms
