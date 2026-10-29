#pragma once

#include "config/Config.h"
#include "display/DisplayInfo.h"
#include <windows.h>
#include <shellapi.h>
#include <functional>
#include <string>
#include <vector>

namespace pms {

inline constexpr UINT kWmTrayCallback = WM_APP + 10;

// Menu command IDs for system tray context menu
inline constexpr UINT kCmdTrayOpen          = 4001;
inline constexpr UINT kCmdTrayMasterToggle  = 4002;
inline constexpr UINT kCmdTrayStartWindows  = 4003;
inline constexpr UINT kCmdTrayIdentify      = 4004;
inline constexpr UINT kCmdTrayExit          = 4005;
inline constexpr UINT kCmdTrayMonitorBase   = 4100; // 4100 .. 4131

class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon();

    bool Create(HWND ownerHwnd, HICON appIcon);
    void Destroy();

    // Re-registers icon if Windows Explorer restarts ("TaskbarCreated" broadcast)
    void ReAddAfterExplorerRestart();

    void UpdateTooltip(const std::wstring& statusSummary);

    void ShowContextMenu(
        HWND ownerHwnd,
        const AppConfig& config,
        const std::vector<DisplayInfo>& displays);

private:
    HWND ownerHwnd_ = nullptr;
    HICON icon_ = nullptr;
    bool added_ = false;
    std::wstring lastTooltip_ = L"PerMonSharp";
};

} // namespace pms
