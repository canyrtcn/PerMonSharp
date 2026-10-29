#pragma once

#include "config/Config.h"
#include "engine/EngineController.h"
#include "ui/IdentifyOverlay.h"
#include "ui/Theme.h"
#include "ui/TrayIcon.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace pms {

class MainWindow {
public:
    MainWindow(const std::wstring& configPath, AppConfig initialConfig);
    ~MainWindow();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool Create(HINSTANCE hInstance, bool startMinimizedToTray, UINT showWindowBroadcastMsg);
    int RunMessageLoop();

private:
    enum class HitPart {
        None = 0,
        MonitorTab,
        IdentifyBtn,
        EnableRow,
        ResetBtn,
        Slider,
        ChkFullscreen,
        ChkMinToTray,
        ChkStartWin,
        ChkHotkey,
        MasterToggleBtn
    };

    struct HitResult {
        HitPart part = HitPart::None;
        int tabIndex = -1;
        bool operator==(const HitResult& o) const {
            return part == o.part && tabIndex == o.tabIndex;
        }
        bool operator!=(const HitResult& o) const { return !(*this == o); }
    };

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    // Direct2D / DirectWrite resources & rendering
    bool EnsureGraphicsResources();
    void DiscardDeviceResources();
    void RenderToDc(HDC hdc);
    void OnPaint();

    // Layout & interaction helpers (coordinates in logical DIPs, 96 DPI base)
    D2D1_POINT_2F PixelToDip(int pxX, int pxY) const;
    HitResult HitTestDip(D2D1_POINT_2F pt) const;
    int SliderPercentFromDipX(float dipX) const;

    void SelectMonitorByIndex(int index);
    MonitorConfig* GetSelectedMonitorConfig();
    const MonitorConfig* GetSelectedMonitorConfig() const;
    const DisplayInfo* GetSelectedDisplayInfo() const;

    void SetSelectedSharpness(int newPercent, bool isDragging);
    void ToggleSelectedMonitorEnabled();
    void ToggleMasterEnabled();
    void ApplyThemeToWindowFrame();
    void SyncHotkeyRegistration();
    void SyncBackgroundTimers();
    void ScheduleSaveConfig(bool immediate);
    void RefreshStatusAndRepaint();
    void ShowAndActivateWindow();
    void HideToTray();
    void RequestCleanExit();

    std::wstring configPath_;
    AppConfig config_;
    EngineController engine_;
    TrayIcon tray_;
    IdentifyOverlay identify_;

    HWND hwnd_ = nullptr;
    HICON appIcon_ = nullptr;
    HICON smallIcon_ = nullptr;
    UINT dpi_ = 96;
    UINT showWindowBroadcastMsg_ = 0;
    UINT taskbarCreatedMsg_ = 0;
    bool hotkeyRegistered_ = false;
    bool hotkeyRegistrationFailed_ = false;
    bool fullscreenTimerActive_ = false;

    UiPalette palette_;
    HitResult hovered_{};
    HitResult pressed_{};
    bool draggingSlider_ = false;

    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwFactory_;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> rt_;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtTabTitle_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtTabSub_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtLabel_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtLabelBold_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtValue_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtSmall_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtBtn_;
};

} // namespace pms
