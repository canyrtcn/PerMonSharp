#pragma once

#include "display/DisplayInfo.h"
#include <windows.h>
#include <vector>

namespace pms {

// Displays temporary (1.4s), click-through numeric + model name badges on each physical display
// so users with multi-monitor setups can immediately verify which monitor corresponds to each tab.
class IdentifyOverlay {
public:
    IdentifyOverlay() = default;
    ~IdentifyOverlay();

    void ShowForDisplays(const std::vector<DisplayInfo>& displays);
    void DismissAll();

private:
    struct BadgeWindow {
        HWND hwnd = nullptr;
        int number = 1;
        std::wstring label;
        UINT dpi = 96;
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void RenderBadge(HWND hwnd, const BadgeWindow& bw);

    std::vector<BadgeWindow> badges_;
};

} // namespace pms
