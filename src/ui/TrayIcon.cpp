#include "ui/TrayIcon.h"
#include "common/Version.h"
#include <cwchar>

namespace pms {

TrayIcon::~TrayIcon() {
    Destroy();
}

bool TrayIcon::Create(HWND ownerHwnd, HICON appIcon) {
    ownerHwnd_ = ownerHwnd;
    icon_ = appIcon ? appIcon : LoadIconW(nullptr, IDI_APPLICATION);

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = ownerHwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kWmTrayCallback;
    nid.hIcon = icon_;
    wcsncpy_s(nid.szTip, lastTooltip_.c_str(), _TRUNCATE);

    added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    return added_;
}

void TrayIcon::ReAddAfterExplorerRestart() {
    if (!ownerHwnd_) return;
    added_ = false;
    Create(ownerHwnd_, icon_);
}

void TrayIcon::Destroy() {
    if (added_ && ownerHwnd_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = ownerHwnd_;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
    added_ = false;
}

void TrayIcon::UpdateTooltip(const std::wstring& statusSummary) {
    std::wstring full = L"PerMonSharp";
    if (!statusSummary.empty()) {
        full += L" — " + statusSummary;
    }
    if (full.size() >= 127) {
        full.resize(126);
    }
    lastTooltip_ = full;

    if (!added_ || !ownerHwnd_) return;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = ownerHwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, lastTooltip_.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::ShowContextMenu(
    HWND ownerHwnd,
    const AppConfig& config,
    const std::vector<DisplayInfo>& displays)
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING, kCmdTrayOpen, L"Open PerMonSharp");
    SetMenuDefaultItem(menu, kCmdTrayOpen, FALSE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    if (displays.empty()) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No active displays");
    } else {
        size_t maxMon = std::min<size_t>(displays.size(), 32);
        for (size_t i = 0; i < maxMon; ++i) {
            const auto& d = displays[i];
            const MonitorConfig* mc = config.FindMonitor(d.id);
            bool monEn = (mc != nullptr) && mc->enabled;
            int pct = mc ? mc->sharpness : kDefaultSharpnessPercent;

            std::wstring label = d.displayLabel + L"  (" + std::to_wstring(pct) + L"%)";
            UINT flags = MF_STRING | (monEn ? MF_CHECKED : MF_UNCHECKED);
            if (!config.masterEnabled) {
                // Keep clickable so toggling re-enables or toggles monitor state intuitively
            }
            AppendMenuW(menu, flags, kCmdTrayMonitorBase + static_cast<UINT>(i), label.c_str());
        }
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    if (displays.size() > 1) {
        AppendMenuW(menu, MF_STRING, kCmdTrayIdentify, L"Identify Displays");
    }

    AppendMenuW(
        menu,
        MF_STRING | (!config.masterEnabled ? MF_CHECKED : MF_UNCHECKED),
        kCmdTrayMasterToggle,
        L"Pause All Sharpening");

    AppendMenuW(
        menu,
        MF_STRING | (config.startWithWindows ? MF_CHECKED : MF_UNCHECKED),
        kCmdTrayStartWindows,
        L"Start with Windows");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdTrayExit, L"Exit");

    POINT pt{};
    GetCursorPos(&pt);

    // Required Win32 pattern (KB135788) so clicking outside dismisses the tray popup cleanly
    SetForegroundWindow(ownerHwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, ownerHwnd, nullptr);
    PostMessageW(ownerHwnd, WM_NULL, 0, 0);

    DestroyMenu(menu);
}

} // namespace pms
