#include "ui/MainWindow.h"
#include "common/Version.h"
#include "common/StringUtils.h"
#include "resource.h"

#include <dwmapi.h>
#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

using Microsoft::WRL::ComPtr;

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

namespace pms {

namespace {

constexpr float kClientWidthDip  = 448.0f;
constexpr float kClientHeightDip = 336.0f;

constexpr float kSliderLeftDip   = 32.0f;
constexpr float kSliderRightDip  = 416.0f;
constexpr float kSliderCenterY   = 176.0f;

constexpr int kHotkeyIdMasterToggle = 1;

constexpr UINT_PTR kTimerSaveDebounce    = 101;
constexpr UINT_PTR kTimerDisplayDebounce = 102;
constexpr UINT_PTR kTimerRuntimePoll     = 103;

bool PointInRectF(D2D1_POINT_2F pt, const D2D1_RECT_F& r) {
    return pt.x >= r.left && pt.x <= r.right && pt.y >= r.top && pt.y <= r.bottom;
}

D2D1_RECT_F GetIdentifyBtnRect() {
    return D2D1::RectF(368.0f, 23.0f, 432.0f, 57.0f);
}

D2D1_RECT_F GetMonitorTabRect(int index, int count) {
    if (count <= 0) return D2D1::RectF(16.0f, 14.0f, 360.0f, 68.0f);
    float areaLeft = 16.0f;
    float areaRight = 360.0f;
    float gap = 6.0f;
    float totalW = areaRight - areaLeft;
    float tabW = (totalW - gap * static_cast<float>(count - 1)) / static_cast<float>(count);
    float l = areaLeft + static_cast<float>(index) * (tabW + gap);
    return D2D1::RectF(l, 14.0f, l + tabW, 68.0f);
}

D2D1_RECT_F GetMainCardRect()    { return D2D1::RectF(16.0f, 80.0f, 432.0f, 206.0f); }
D2D1_RECT_F GetEnableRowRect()   { return D2D1::RectF(16.0f, 80.0f, 432.0f, 124.0f); }
D2D1_RECT_F GetResetBtnRect()    { return D2D1::RectF(324.0f, 134.0f, 370.0f, 155.0f); }
D2D1_RECT_F GetSliderHitRect()   { return D2D1::RectF(22.0f, 160.0f, 426.0f, 196.0f); }

D2D1_RECT_F GetChkFullscreenRect() { return D2D1::RectF(16.0f,  218.0f, 222.0f, 246.0f); }
D2D1_RECT_F GetChkMinToTrayRect()  { return D2D1::RectF(228.0f, 218.0f, 432.0f, 246.0f); }
D2D1_RECT_F GetChkStartWinRect()   { return D2D1::RectF(16.0f,  252.0f, 222.0f, 280.0f); }
D2D1_RECT_F GetChkHotkeyRect()     { return D2D1::RectF(228.0f, 252.0f, 432.0f, 280.0f); }

D2D1_RECT_F GetMasterBtnRect()     { return D2D1::RectF(342.0f, 304.0f, 432.0f, 328.0f); }

} // namespace

MainWindow::MainWindow(const std::wstring& configPath, AppConfig initialConfig)
    : configPath_(configPath)
    , config_(std::move(initialConfig))
{
    palette_ = GetSystemPalette();
}

MainWindow::~MainWindow() {
    if (hwnd_ && hotkeyRegistered_) {
        UnregisterHotKey(hwnd_, kHotkeyIdMasterToggle);
    }
    tray_.Destroy();
    identify_.DismissAll();
    engine_.StopAll();
    DiscardDeviceResources();
    if (appIcon_) DestroyIcon(appIcon_);
    if (smallIcon_) DestroyIcon(smallIcon_);
}

bool MainWindow::Create(HINSTANCE hInstance, bool startMinimizedToTray, UINT showWindowBroadcastMsg) {
    showWindowBroadcastMsg_ = showWindowBroadcastMsg;
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");

    // Sync config's startWithWindows boolean with actual registry state so UI is always truthful
    config_.startWithWindows = ConfigManager::IsStartupRegistryEnabled();

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf()))) {
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwFactory_.GetAddressOf())))) {
        return false;
    }

    appIcon_ = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    smallIcon_ = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MainWindow::StaticWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = appIcon_;
    wc.hIconSm = smallIcon_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kMainWndClass;
    RegisterClassExW(&wc);

    // Determine primary monitor DPI for crisp initial sizing
    POINT ptZero{ 0, 0 };
    HMONITOR hPrimary = MonitorFromPoint(ptZero, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hPrimary, &mi);

    UINT initDpi = 96;
    HDC screenDc = GetDC(nullptr);
    if (screenDc) {
        initDpi = static_cast<UINT>(GetDeviceCaps(screenDc, LOGPIXELSX));
        ReleaseDC(nullptr, screenDc);
    }
    dpi_ = initDpi > 0 ? initDpi : 96;

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    DWORD exStyle = WS_EX_APPWINDOW;

    RECT wr{
        0, 0,
        MulDiv(static_cast<int>(kClientWidthDip),  static_cast<int>(dpi_), 96),
        MulDiv(static_cast<int>(kClientHeightDip), static_cast<int>(dpi_), 96)
    };
    AdjustWindowRectExForDpi(&wr, style, FALSE, exStyle, dpi_);

    int winW = wr.right - wr.left;
    int winH = wr.bottom - wr.top;
    int workW = mi.rcWork.right - mi.rcWork.left;
    int workH = mi.rcWork.bottom - mi.rcWork.top;
    int posX = mi.rcWork.left + (workW - winW) / 2;
    int posY = mi.rcWork.top  + (workH - winH) / 2;

    hwnd_ = CreateWindowExW(
        exStyle,
        kMainWndClass,
        kAppName,
        style,
        posX, posY, winW, winH,
        nullptr, nullptr, hInstance, this);

    if (!hwnd_) return false;

    dpi_ = GetDpiForWindow(hwnd_);
    // Re-adjust exact client size if monitor DPI differs from system DPI
    if (dpi_ != initDpi) {
        RECT wr2{
            0, 0,
            MulDiv(static_cast<int>(kClientWidthDip),  static_cast<int>(dpi_), 96),
            MulDiv(static_cast<int>(kClientHeightDip), static_cast<int>(dpi_), 96)
        };
        AdjustWindowRectExForDpi(&wr2, style, FALSE, exStyle, dpi_);
        SetWindowPos(hwnd_, nullptr, 0, 0, wr2.right - wr2.left, wr2.bottom - wr2.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    ApplyThemeToWindowFrame();
    tray_.Create(hwnd_, smallIcon_ ? smallIcon_ : appIcon_);

    // Enumerate displays and start workers for any previously enabled monitors
    engine_.RefreshDisplays(config_);
    SyncHotkeyRegistration();
    SyncBackgroundTimers();
    tray_.UpdateTooltip(engine_.BuildStatusSummary(config_));

    if (!startMinimizedToTray) {
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
    }

    return true;
}

int MainWindow::RunMessageLoop() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void MainWindow::ApplyThemeToWindowFrame() {
    palette_ = GetSystemPalette();
    if (!hwnd_) return;

    BOOL useDark = palette_.isDark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));

    if (IsWindows11OrGreater()) {
        DWORD cornerPref = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
    }
}

void MainWindow::SyncHotkeyRegistration() {
    if (!hwnd_) return;
    if (hotkeyRegistered_) {
        UnregisterHotKey(hwnd_, kHotkeyIdMasterToggle);
        hotkeyRegistered_ = false;
    }
    hotkeyRegistrationFailed_ = false;
    if (config_.enableGlobalHotkey) {
        if (RegisterHotKey(hwnd_, kHotkeyIdMasterToggle, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'S')) {
            hotkeyRegistered_ = true;
        } else {
            hotkeyRegistrationFailed_ = true;
        }
    }
}

void MainWindow::SyncBackgroundTimers() {
    if (!hwnd_) return;
    bool needPoll = engine_.GetActiveWorkerCount() > 0;
    if (needPoll && !fullscreenTimerActive_) {
        SetTimer(hwnd_, kTimerRuntimePoll, 450, nullptr);
        fullscreenTimerActive_ = true;
    } else if (!needPoll && fullscreenTimerActive_) {
        KillTimer(hwnd_, kTimerRuntimePoll);
        fullscreenTimerActive_ = false;
    }
}

void MainWindow::ScheduleSaveConfig(bool immediate) {
    if (!hwnd_) {
        ConfigManager::SaveToFile(configPath_, config_);
        return;
    }
    if (immediate) {
        KillTimer(hwnd_, kTimerSaveDebounce);
        ConfigManager::SaveToFile(configPath_, config_);
    } else {
        SetTimer(hwnd_, kTimerSaveDebounce, 350, nullptr);
    }
}

void MainWindow::RefreshStatusAndRepaint() {
    SyncBackgroundTimers();
    tray_.UpdateTooltip(engine_.BuildStatusSummary(config_));
    if (hwnd_ && IsWindowVisible(hwnd_)) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::ShowAndActivateWindow() {
    if (!hwnd_) return;
    if (!IsWindowVisible(hwnd_)) {
        ShowWindow(hwnd_, SW_SHOW);
    }
    if (IsIconic(hwnd_)) {
        ShowWindow(hwnd_, SW_RESTORE);
    }
    SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::HideToTray() {
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_HIDE);
    DiscardDeviceResources();
}

void MainWindow::RequestCleanExit() {
    ScheduleSaveConfig(true);
    identify_.DismissAll();
    engine_.StopAll();
    tray_.Destroy();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

const DisplayInfo* MainWindow::GetSelectedDisplayInfo() const {
    return engine_.FindDisplay(config_.lastSelectedMonitorId);
}

MonitorConfig* MainWindow::GetSelectedMonitorConfig() {
    if (config_.lastSelectedMonitorId.empty()) return nullptr;
    const DisplayInfo* di = GetSelectedDisplayInfo();
    return &config_.GetOrCreateMonitor(
        config_.lastSelectedMonitorId,
        di ? WideToUtf8(di->friendlyName) : std::string{});
}

const MonitorConfig* MainWindow::GetSelectedMonitorConfig() const {
    if (config_.lastSelectedMonitorId.empty()) return nullptr;
    return config_.FindMonitor(config_.lastSelectedMonitorId);
}

void MainWindow::SelectMonitorByIndex(int index) {
    const auto& displays = engine_.GetDisplays();
    if (index < 0 || static_cast<size_t>(index) >= displays.size()) return;
    if (config_.lastSelectedMonitorId != displays[index].id) {
        config_.lastSelectedMonitorId = displays[index].id;
        ScheduleSaveConfig(false);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::SetSelectedSharpness(int newPercent, bool isDragging) {
    MonitorConfig* mc = GetSelectedMonitorConfig();
    if (!mc) return;

    int clamped = std::clamp(newPercent, kMinSharpnessPercent, kMaxSharpnessPercent);
    if (mc->sharpness == clamped && isDragging) return;

    mc->sharpness = clamped;
    engine_.ApplyConfig(config_, isDragging);
    ScheduleSaveConfig(!isDragging);
    RefreshStatusAndRepaint();
}

void MainWindow::ToggleSelectedMonitorEnabled() {
    MonitorConfig* mc = GetSelectedMonitorConfig();
    if (!mc) return;

    mc->enabled = !mc->enabled;
    if (mc->enabled) {
        if (mc->sharpness <= 0) mc->sharpness = kDefaultSharpnessPercent;
        if (!config_.masterEnabled) config_.masterEnabled = true;
    }
    engine_.ApplyConfig(config_, false);
    ScheduleSaveConfig(true);
    RefreshStatusAndRepaint();
}

void MainWindow::ToggleMasterEnabled() {
    config_.masterEnabled = !config_.masterEnabled;
    engine_.ApplyConfig(config_, false);
    ScheduleSaveConfig(true);
    RefreshStatusAndRepaint();
}

D2D1_POINT_2F MainWindow::PixelToDip(int pxX, int pxY) const {
    float scale = static_cast<float>(dpi_) / 96.0f;
    if (scale <= 0.01f) scale = 1.0f;
    return D2D1::Point2F(static_cast<float>(pxX) / scale, static_cast<float>(pxY) / scale);
}

int MainWindow::SliderPercentFromDipX(float dipX) const {
    float t = (dipX - kSliderLeftDip) / (kSliderRightDip - kSliderLeftDip);
    t = std::clamp(t, 0.0f, 1.0f);
    return static_cast<int>(std::lround(t * 100.0f));
}

MainWindow::HitResult MainWindow::HitTestDip(D2D1_POINT_2F pt) const {
    const auto& displays = engine_.GetDisplays();
    int count = static_cast<int>(displays.size());
    for (int i = 0; i < count; ++i) {
        if (PointInRectF(pt, GetMonitorTabRect(i, count))) {
            return { HitPart::MonitorTab, i };
        }
    }

    if (PointInRectF(pt, GetIdentifyBtnRect()) && !displays.empty()) {
        return { HitPart::IdentifyBtn, -1 };
    }

    if (!displays.empty()) {
        if (PointInRectF(pt, GetEnableRowRect())) return { HitPart::EnableRow, -1 };
        if (PointInRectF(pt, GetResetBtnRect()))  return { HitPart::ResetBtn, -1 };
        if (PointInRectF(pt, GetSliderHitRect())) return { HitPart::Slider, -1 };
    }

    if (PointInRectF(pt, GetChkFullscreenRect())) return { HitPart::ChkFullscreen, -1 };
    if (PointInRectF(pt, GetChkMinToTrayRect()))  return { HitPart::ChkMinToTray, -1 };
    if (PointInRectF(pt, GetChkStartWinRect()))   return { HitPart::ChkStartWin, -1 };
    if (PointInRectF(pt, GetChkHotkeyRect()))     return { HitPart::ChkHotkey, -1 };

    bool anyMonitorConfigured = false;
    for (const auto& d : displays) {
        const auto* mc = config_.FindMonitor(d.id);
        if (mc && mc->enabled && mc->sharpness > 0) {
            anyMonitorConfigured = true;
            break;
        }
    }
    if (anyMonitorConfigured && PointInRectF(pt, GetMasterBtnRect())) {
        return { HitPart::MasterToggleBtn, -1 };
    }

    return {};
}

bool MainWindow::EnsureGraphicsResources() {
    if (!hwnd_ || !d2dFactory_ || !dwFactory_) return false;

    if (!fmtLabel_) {
        // Choose Segoe UI Variable Text on Windows 11, falling back cleanly to Segoe UI on Windows 10
        const wchar_t* fontName = L"Segoe UI";
        ComPtr<IDWriteFontCollection> sysFonts;
        if (SUCCEEDED(dwFactory_->GetSystemFontCollection(&sysFonts)) && sysFonts) {
            UINT32 idx = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(sysFonts->FindFamilyName(L"Segoe UI Variable Text", &idx, &exists)) && exists) {
                fontName = L"Segoe UI Variable Text";
            }
        }

        auto makeFmt = [&](float sizeDip, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& outFmt) {
            dwFactory_->CreateTextFormat(
                fontName, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                sizeDip, L"en-US", &outFmt);
            if (outFmt) {
                outFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
                ComPtr<IDWriteInlineObject> ell;
                if (SUCCEEDED(dwFactory_->CreateEllipsisTrimmingSign(outFmt.Get(), &ell))) {
                    outFmt->SetTrimming(&trim, ell.Get());
                }
            }
        };

        makeFmt(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, fmtTabTitle_);
        makeFmt(10.5f, DWRITE_FONT_WEIGHT_NORMAL,    fmtTabSub_);
        makeFmt(12.5f, DWRITE_FONT_WEIGHT_NORMAL,    fmtLabel_);
        makeFmt(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, fmtLabelBold_);
        makeFmt(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, fmtValue_);
        makeFmt(11.5f, DWRITE_FONT_WEIGHT_NORMAL,    fmtSmall_);
        makeFmt(11.5f, DWRITE_FONT_WEIGHT_MEDIUM,    fmtBtn_);

        if (fmtValue_) {
            fmtValue_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            fmtValue_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (fmtBtn_) {
            fmtBtn_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            fmtBtn_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    if (!rt_) {
        D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            static_cast<float>(dpi_),
            static_cast<float>(dpi_),
            D2D1_RENDER_TARGET_USAGE_NONE,
            D2D1_FEATURE_LEVEL_DEFAULT);

        if (FAILED(d2dFactory_->CreateDCRenderTarget(&rtp, &rt_))) {
            return false;
        }
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    }
    rt_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
    return true;
}

void MainWindow::DiscardDeviceResources() {
    rt_.Reset();
}

void MainWindow::OnPaint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);
    if (hdc) {
        RenderToDc(hdc);
    }
    EndPaint(hwnd_, &ps);
}

void MainWindow::RenderToDc(HDC hdc) {
    if (!hdc || !EnsureGraphicsResources()) {
        return;
    }

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    if (rc.right <= rc.left || rc.bottom <= rc.top) {
        return;
    }

    if (FAILED(rt_->BindDC(hdc, &rc))) {
        return;
    }

    rt_->BeginDraw();
    rt_->Clear(palette_.windowBg);

    ComPtr<ID2D1SolidColorBrush> br;
    rt_->CreateSolidColorBrush(palette_.textPrimary, &br);

    auto fillRounded = [&](const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& col) {
        br->SetColor(col);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), br.Get());
    };

    auto strokeRounded1px = [&](const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& col) {
        D2D1_RECT_F aligned = D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f);
        br->SetColor(col);
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(aligned, radius, radius), br.Get(), 1.0f);
    };

    const auto& displays = engine_.GetDisplays();
    const int dispCount = static_cast<int>(displays.size());

    // -------------------------------------------------------------------------
    // 1. Top Strip: Connected Monitor Selector Tabs + Identify Button
    // -------------------------------------------------------------------------
    if (displays.empty()) {
        D2D1_RECT_F emptyBox = D2D1::RectF(16.0f, 14.0f, 432.0f, 68.0f);
        fillRounded(emptyBox, 5.0f, palette_.panelBg);
        strokeRounded1px(emptyBox, 5.0f, palette_.border);
        if (fmtLabel_) {
            fmtLabel_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            br->SetColor(palette_.textSecondary);
            const wchar_t kMsg[] = L"No active displays detected";
            rt_->DrawTextW(kMsg, static_cast<UINT32>(std::size(kMsg) - 1),
                           fmtLabel_.Get(), D2D1::RectF(28.0f, 14.0f, 420.0f, 68.0f), br.Get());
        }
    } else {
        for (int i = 0; i < dispCount; ++i) {
            const auto& d = displays[i];
            const MonitorConfig* mc = config_.FindMonitor(d.id);
            bool isSelected = (d.id == config_.lastSelectedMonitorId);
            bool isHovered = (hovered_.part == HitPart::MonitorTab && hovered_.tabIndex == i);

            D2D1_RECT_F tr = GetMonitorTabRect(i, dispCount);
            D2D1_COLOR_F bg = isSelected ? palette_.tabSelectedBg
                                         : (isHovered ? palette_.panelHoverBg : palette_.tabInactiveBg);
            fillRounded(tr, 5.0f, bg);
            strokeRounded1px(tr, 5.0f, isSelected ? palette_.borderStrong : palette_.border);

            // Crisp 2px accent indicator bar along bottom of selected monitor tab
            if (isSelected) {
                br->SetColor(palette_.accent);
                rt_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(tr.left + 10.0f, tr.bottom - 2.5f, tr.right - 10.0f, tr.bottom - 0.5f), 1.0f, 1.0f),
                    br.Get());
            }

            // Status dot (6px diameter)
            WorkerState wst = engine_.GetDisplayWorkerState(d.id);
            bool cfgActive = config_.masterEnabled && mc && mc->enabled && mc->sharpness > 0;
            float dotX = tr.left + 13.0f;
            float dotY = tr.top  + 18.0f;
            if (wst == WorkerState::Error) {
                br->SetColor(palette_.statusError);
                rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dotX, dotY), 3.2f, 3.2f), br.Get());
            } else if (wst == WorkerState::PausedFullscreen) {
                br->SetColor(palette_.statusAmber);
                rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dotX, dotY), 3.2f, 3.2f), br.Get());
            } else if (cfgActive) {
                br->SetColor(palette_.accent);
                rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dotX, dotY), 3.2f, 3.2f), br.Get());
            } else {
                br->SetColor(palette_.textMuted);
                rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(dotX, dotY), 2.8f, 2.8f), br.Get(), 1.1f);
            }

            // Monitor friendly model name
            if (fmtTabTitle_) {
                fmtTabTitle_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                br->SetColor(isSelected ? palette_.textPrimary : palette_.textSecondary);
                rt_->DrawTextW(
                    d.displayLabel.c_str(),
                    static_cast<UINT32>(d.displayLabel.size()),
                    fmtTabTitle_.Get(),
                    D2D1::RectF(tr.left + 22.0f, tr.top + 8.0f, tr.right - 8.0f, tr.top + 28.0f),
                    br.Get());
            }

            // Resolution · Refresh rate subtitle
            if (fmtTabSub_) {
                int hzInt = static_cast<int>(std::lround(d.refreshRateHz));
                wchar_t subBuf[96];
                std::swprintf(subBuf, std::size(subBuf), L"%dx%d · %d Hz", d.width, d.height, hzInt);
                std::wstring subStr = subBuf;
                if (mc && mc->enabled && mc->sharpness > 0) {
                    subStr += L" · " + std::to_wstring(mc->sharpness) + L"%";
                }

                fmtTabSub_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                br->SetColor(palette_.textMuted);
                rt_->DrawTextW(
                    subStr.c_str(),
                    static_cast<UINT32>(subStr.size()),
                    fmtTabSub_.Get(),
                    D2D1::RectF(tr.left + 22.0f, tr.top + 29.0f, tr.right - 8.0f, tr.top + 47.0f),
                    br.Get());
            }
        }

        // Identify Displays button
        D2D1_RECT_F idRect = GetIdentifyBtnRect();
        bool idHov = (hovered_.part == HitPart::IdentifyBtn);
        fillRounded(idRect, 4.5f, idHov ? palette_.panelHoverBg : palette_.panelBg);
        strokeRounded1px(idRect, 4.5f, idHov ? palette_.borderStrong : palette_.border);
        if (fmtBtn_) {
            br->SetColor(palette_.textPrimary);
            const wchar_t kIdTxt[] = L"Identify";
            rt_->DrawTextW(kIdTxt, static_cast<UINT32>(std::size(kIdTxt) - 1), fmtBtn_.Get(), idRect, br.Get());
        }
    }

    // -------------------------------------------------------------------------
    // 2. Main Per-Monitor Control Card
    // -------------------------------------------------------------------------
    D2D1_RECT_F card = GetMainCardRect();
    fillRounded(card, 6.0f, palette_.panelBg);
    strokeRounded1px(card, 6.0f, palette_.border);

    const DisplayInfo* selDisp = GetSelectedDisplayInfo();
    const MonitorConfig* selCfg = GetSelectedMonitorConfig();
    bool monEnabled = (selCfg != nullptr) && selCfg->enabled;
    bool effectiveActive = config_.masterEnabled && monEnabled && selCfg->sharpness > 0;
    int sharpPct = selCfg ? selCfg->sharpness : kDefaultSharpnessPercent;

    // Enable row hover highlight (clipped cleanly inside top half of card)
    if (hovered_.part == HitPart::EnableRow && selDisp) {
        D2D1_RECT_F er = D2D1::RectF(card.left + 1.0f, card.top + 1.0f, card.right - 1.0f, 124.0f);
        fillRounded(er, 5.0f, palette_.panelHoverBg);
    }

    if (fmtLabelBold_) {
        fmtLabelBold_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        br->SetColor(selDisp ? palette_.textPrimary : palette_.textMuted);
        const wchar_t kEnTxt[] = L"Enable sharpening on this display";
        rt_->DrawTextW(kEnTxt, static_cast<UINT32>(std::size(kEnTxt) - 1),
                       fmtLabelBold_.Get(), D2D1::RectF(30.0f, 80.0f, 290.0f, 124.0f), br.Get());
    }

    // Subtle Primary / HDR metadata tag before toggle switch
    if (selDisp && (selDisp->isPrimary || selDisp->isHdrActive) && fmtTabSub_) {
        std::wstring badge;
        if (selDisp->isPrimary) badge = L"Primary";
        if (selDisp->isHdrActive) {
            if (!badge.empty()) badge += L" · ";
            badge += L"HDR";
        }
        fmtTabSub_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        fmtTabSub_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        br->SetColor(palette_.textMuted);
        rt_->DrawTextW(badge.c_str(), static_cast<UINT32>(badge.size()),
                       fmtTabSub_.Get(), D2D1::RectF(270.0f, 80.0f, 366.0f, 124.0f), br.Get());
        fmtTabSub_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    // Native Windows 11-style pill toggle switch (40x20 DIPs)
    D2D1_RECT_F swRect = D2D1::RectF(378.0f, 92.0f, 418.0f, 112.0f);
    if (monEnabled) {
        D2D1_COLOR_F swCol = config_.masterEnabled ? palette_.accent : palette_.textMuted;
        fillRounded(swRect, 10.0f, swCol);
        br->SetColor(palette_.accentText);
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(408.0f, 102.0f), 6.8f, 6.8f), br.Get());
    } else {
        strokeRounded1px(swRect, 10.0f, palette_.borderStrong);
        br->SetColor(palette_.textSecondary);
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(388.0f, 102.0f), 6.2f, 6.2f), br.Get());
    }

    // Card inner divider
    br->SetColor(palette_.divider);
    rt_->DrawLine(D2D1::Point2F(28.0f, 124.5f), D2D1::Point2F(420.0f, 124.5f), br.Get(), 1.0f);

    // Sharpness label + Reset button + numeric % value
    if (fmtLabel_) {
        fmtLabel_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        br->SetColor(monEnabled ? palette_.textPrimary : palette_.textSecondary);
        const wchar_t kShTxt[] = L"Sharpness";
        rt_->DrawTextW(kShTxt, static_cast<UINT32>(std::size(kShTxt) - 1),
                       fmtLabel_.Get(), D2D1::RectF(30.0f, 132.0f, 160.0f, 156.0f), br.Get());
    }

    bool canReset = (selDisp != nullptr) && (sharpPct != kDefaultSharpnessPercent);
    D2D1_RECT_F rstRect = GetResetBtnRect();
    if (canReset) {
        bool rstHov = (hovered_.part == HitPart::ResetBtn);
        if (rstHov) {
            fillRounded(rstRect, 3.5f, palette_.panelHoverBg);
            strokeRounded1px(rstRect, 3.5f, palette_.borderStrong);
        }
        if (fmtBtn_) {
            br->SetColor(rstHov ? palette_.textPrimary : palette_.textSecondary);
            const wchar_t kRst[] = L"Reset";
            rt_->DrawTextW(kRst, static_cast<UINT32>(std::size(kRst) - 1), fmtBtn_.Get(), rstRect, br.Get());
        }
    }

    if (fmtValue_) {
        std::wstring pctStr = std::to_wstring(sharpPct) + L"%";
        br->SetColor(effectiveActive ? palette_.textPrimary : palette_.textSecondary);
        rt_->DrawTextW(pctStr.c_str(), static_cast<UINT32>(pctStr.size()),
                       fmtValue_.Get(), D2D1::RectF(368.0f, 131.0f, 418.0f, 157.0f), br.Get());
    }

    // Precision Slider Track + Ticks + Thumb
    float norm = static_cast<float>(sharpPct) / 100.0f;
    float thumbX = kSliderLeftDip + norm * (kSliderRightDip - kSliderLeftDip);

    // Subtle 25% step tick marks below track
    br->SetColor(palette_.borderStrong);
    for (int step = 0; step <= 4; ++step) {
        float tx = std::round(kSliderLeftDip + (static_cast<float>(step) * 0.25f) * (kSliderRightDip - kSliderLeftDip)) + 0.5f;
        rt_->DrawLine(D2D1::Point2F(tx, kSliderCenterY + 9.0f),
                      D2D1::Point2F(tx, kSliderCenterY + 13.0f), br.Get(), 1.0f);
    }

    // Unfilled track
    fillRounded(D2D1::RectF(kSliderLeftDip, kSliderCenterY - 2.0f, kSliderRightDip, kSliderCenterY + 2.0f),
                2.0f, palette_.trackBg);

    // Filled left track
    if (sharpPct > 0) {
        D2D1_COLOR_F fillCol = effectiveActive ? palette_.accent : palette_.textMuted;
        fillRounded(D2D1::RectF(kSliderLeftDip, kSliderCenterY - 2.0f, thumbX, kSliderCenterY + 2.0f),
                    2.0f, fillCol);
    }

    // Slider thumb
    bool sliderHov = (hovered_.part == HitPart::Slider) || draggingSlider_;
    D2D1_ELLIPSE outerThumb = D2D1::Ellipse(D2D1::Point2F(thumbX, kSliderCenterY), 8.5f, 8.5f);
    br->SetColor(palette_.thumbFill);
    rt_->FillEllipse(outerThumb, br.Get());
    br->SetColor(palette_.thumbBorder);
    rt_->DrawEllipse(outerThumb, br.Get(), 1.0f);

    float innerRad = draggingSlider_ ? 3.5f : (sliderHov ? 5.0f : 4.2f);
    br->SetColor(effectiveActive ? palette_.accent : palette_.textMuted);
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, kSliderCenterY), innerRad, innerRad), br.Get());

    // -------------------------------------------------------------------------
    // 3. Behavior & Background Checkboxes (2x2 clean grid)
    // -------------------------------------------------------------------------
    auto drawCheckbox = [&](const D2D1_RECT_F& r, bool checked, HitPart part, const wchar_t* label) {
        bool hov = (hovered_.part == part);
        if (hov) {
            fillRounded(r, 4.0f, palette_.panelBg);
        }

        float boxLeft = r.left + 4.0f;
        float boxTop  = std::round((r.top + r.bottom) * 0.5f - 8.0f);
        D2D1_RECT_F box = D2D1::RectF(boxLeft, boxTop, boxLeft + 16.0f, boxTop + 16.0f);

        if (checked) {
            fillRounded(box, 3.2f, palette_.accent);
            // Crisp vector checkmark
            br->SetColor(palette_.accentText);
            rt_->DrawLine(D2D1::Point2F(boxLeft + 3.8f, boxTop + 8.3f),
                          D2D1::Point2F(boxLeft + 6.9f, boxTop + 11.4f), br.Get(), 1.75f);
            rt_->DrawLine(D2D1::Point2F(boxLeft + 6.6f, boxTop + 11.4f),
                          D2D1::Point2F(boxLeft + 12.4f, boxTop + 5.0f), br.Get(), 1.75f);
        } else {
            fillRounded(box, 3.2f, palette_.panelBg);
            strokeRounded1px(box, 3.2f, hov ? palette_.textSecondary : palette_.borderStrong);
        }

        if (fmtSmall_) {
            fmtSmall_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            br->SetColor(palette_.textPrimary);
            rt_->DrawTextW(label, static_cast<UINT32>(wcslen(label)),
                           fmtSmall_.Get(), D2D1::RectF(boxLeft + 24.0f, r.top, r.right - 2.0f, r.bottom), br.Get());
        }
    };

    drawCheckbox(GetChkFullscreenRect(), config_.pauseOnFullscreen,    HitPart::ChkFullscreen, L"Pause for fullscreen apps");
    drawCheckbox(GetChkMinToTrayRect(),  config_.minimizeToTrayOnClose, HitPart::ChkMinToTray,  L"Minimize to tray on close");
    drawCheckbox(GetChkStartWinRect(),   config_.startWithWindows,     HitPart::ChkStartWin,   L"Start with Windows");

    const wchar_t* hkLabel = (config_.enableGlobalHotkey && hotkeyRegistrationFailed_)
                             ? L"Ctrl+Alt+S (in use)"
                             : L"Shortcut (Ctrl+Alt+S)";
    drawCheckbox(GetChkHotkeyRect(), config_.enableGlobalHotkey, HitPart::ChkHotkey, hkLabel);

    // -------------------------------------------------------------------------
    // 4. Footer Status Bar & Master Pause/Resume Button
    // -------------------------------------------------------------------------
    br->SetColor(palette_.divider);
    rt_->DrawLine(D2D1::Point2F(16.0f, 296.5f), D2D1::Point2F(432.0f, 296.5f), br.Get(), 1.0f);

    std::wstring summary = engine_.BuildStatusSummary(config_);
    bool anyMonitorConfigured = false;
    for (const auto& d : displays) {
        const auto* mc = config_.FindMonitor(d.id);
        if (mc && mc->enabled && mc->sharpness > 0) {
            anyMonitorConfigured = true;
            break;
        }
    }

    float statusDotY = 316.0f;
    if (anyMonitorConfigured && config_.masterEnabled) {
        br->SetColor(palette_.accent);
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(21.0f, statusDotY), 3.0f, 3.0f), br.Get());
    } else if (anyMonitorConfigured && !config_.masterEnabled) {
        br->SetColor(palette_.statusAmber);
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(21.0f, statusDotY), 3.0f, 3.0f), br.Get());
    } else {
        br->SetColor(palette_.textMuted);
        rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(21.0f, statusDotY), 2.7f, 2.7f), br.Get(), 1.0f);
    }

    if (fmtSmall_) {
        fmtSmall_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        br->SetColor(palette_.textSecondary);
        float maxStatusRight = anyMonitorConfigured ? 334.0f : 430.0f;
        rt_->DrawTextW(summary.c_str(), static_cast<UINT32>(summary.size()),
                       fmtSmall_.Get(), D2D1::RectF(30.0f, 297.0f, maxStatusRight, 335.0f), br.Get());
    }

    if (anyMonitorConfigured) {
        D2D1_RECT_F mb = GetMasterBtnRect();
        bool mbHov = (hovered_.part == HitPart::MasterToggleBtn);
        fillRounded(mb, 4.0f, mbHov ? palette_.panelHoverBg : palette_.panelBg);
        strokeRounded1px(mb, 4.0f, mbHov ? palette_.borderStrong : palette_.border);
        if (fmtBtn_) {
            br->SetColor(palette_.textPrimary);
            const wchar_t* btnTxt = config_.masterEnabled ? L"Pause All" : L"Resume All";
            rt_->DrawTextW(btnTxt, static_cast<UINT32>(wcslen(btnTxt)), fmtBtn_.Get(), mb, br.Get());
        }
    }

    HRESULT hrEnd = rt_->EndDraw();
    if (hrEnd == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
    }
}

LRESULT CALLBACK MainWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = static_cast<MainWindow*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        return self->HandleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (showWindowBroadcastMsg_ != 0 && msg == showWindowBroadcastMsg_) {
        ShowAndActivateWindow();
        return 0;
    }
    if (taskbarCreatedMsg_ != 0 && msg == taskbarCreatedMsg_) {
        tray_.ReAddAfterExplorerRestart();
        tray_.UpdateTooltip(engine_.BuildStatusSummary(config_));
        return 0;
    }

    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            OnPaint();
            return 0;

        case WM_PRINTCLIENT:
            RenderToDc(reinterpret_cast<HDC>(wParam));
            return 0;

        case WM_SIZE:
            return 0;

        case WM_DPICHANGED: {
            dpi_ = HIWORD(wParam);
            if (rt_) {
                rt_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
            }
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested) {
                SetWindowPos(hwnd_, nullptr,
                             suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case WM_SETTINGCHANGE: {
            ApplyThemeToWindowFrame();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case WM_DISPLAYCHANGE: {
            // Coalesce bursty Windows hot-plug / resolution change notifications
            SetTimer(hwnd_, kTimerDisplayDebounce, 250, nullptr);
            return 0;
        }

        case WM_POWERBROADCAST: {
            if (wParam == PBT_APMSUSPEND) {
                engine_.StopAll();
            } else if (wParam == PBT_APMRESUMEAUTOMATIC) {
                SetTimer(hwnd_, kTimerDisplayDebounce, 600, nullptr);
            }
            return TRUE;
        }

        case WM_HOTKEY: {
            if (wParam == kHotkeyIdMasterToggle) {
                ToggleMasterEnabled();
            }
            return 0;
        }

        case WM_TIMER: {
            if (wParam == kTimerSaveDebounce) {
                KillTimer(hwnd_, kTimerSaveDebounce);
                ConfigManager::SaveToFile(configPath_, config_);
            } else if (wParam == kTimerDisplayDebounce) {
                KillTimer(hwnd_, kTimerDisplayDebounce);
                engine_.RefreshDisplays(config_);
                ScheduleSaveConfig(true);
                RefreshStatusAndRepaint();
            } else if (wParam == kTimerRuntimePoll) {
                bool changed = engine_.UpdateFullscreenStates(config_);
                if (changed || IsWindowVisible(hwnd_)) {
                    tray_.UpdateTooltip(engine_.BuildStatusSummary(config_));
                    if (IsWindowVisible(hwnd_)) {
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                }
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&tme);

            D2D1_POINT_2F dip = PixelToDip(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (draggingSlider_) {
                SetSelectedSharpness(SliderPercentFromDipX(dip.x), true);
                return 0;
            }

            HitResult hit = HitTestDip(dip);
            if (hit != hovered_) {
                hovered_ = hit;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            if (hovered_.part != HitPart::None) {
                hovered_ = {};
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            SetFocus(hwnd_);
            D2D1_POINT_2F dip = PixelToDip(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            HitResult hit = HitTestDip(dip);
            pressed_ = hit;

            if (hit.part == HitPart::Slider) {
                draggingSlider_ = true;
                SetCapture(hwnd_);
                SetSelectedSharpness(SliderPercentFromDipX(dip.x), true);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            D2D1_POINT_2F dip = PixelToDip(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (draggingSlider_) {
                draggingSlider_ = false;
                ReleaseCapture();
                SetSelectedSharpness(SliderPercentFromDipX(dip.x), false);
                pressed_ = {};
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

            HitResult hit = HitTestDip(dip);
            if (hit == pressed_) {
                switch (hit.part) {
                    case HitPart::MonitorTab:
                        SelectMonitorByIndex(hit.tabIndex);
                        break;
                    case HitPart::IdentifyBtn:
                        identify_.ShowForDisplays(engine_.GetDisplays());
                        break;
                    case HitPart::EnableRow:
                        ToggleSelectedMonitorEnabled();
                        break;
                    case HitPart::ResetBtn:
                        SetSelectedSharpness(kDefaultSharpnessPercent, false);
                        break;
                    case HitPart::ChkFullscreen:
                        config_.pauseOnFullscreen = !config_.pauseOnFullscreen;
                        engine_.UpdateFullscreenStates(config_);
                        ScheduleSaveConfig(true);
                        RefreshStatusAndRepaint();
                        break;
                    case HitPart::ChkMinToTray:
                        config_.minimizeToTrayOnClose = !config_.minimizeToTrayOnClose;
                        ScheduleSaveConfig(true);
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        break;
                    case HitPart::ChkStartWin: {
                        bool target = !config_.startWithWindows;
                        if (ConfigManager::SetStartupRegistryEnabled(target)) {
                            config_.startWithWindows = target;
                            ScheduleSaveConfig(true);
                            InvalidateRect(hwnd_, nullptr, FALSE);
                        }
                        break;
                    }
                    case HitPart::ChkHotkey:
                        config_.enableGlobalHotkey = !config_.enableGlobalHotkey;
                        SyncHotkeyRegistration();
                        ScheduleSaveConfig(true);
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        break;
                    case HitPart::MasterToggleBtn:
                        ToggleMasterEnabled();
                        break;
                    default:
                        break;
                }
            }
            pressed_ = {};
            return 0;
        }

        case WM_CAPTURECHANGED: {
            if (draggingSlider_) {
                draggingSlider_ = false;
                engine_.ApplyConfig(config_, false);
                ScheduleSaveConfig(true);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSEWHEEL: {
            POINT ptScreen{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd_, &ptScreen);
            D2D1_POINT_2F dip = PixelToDip(ptScreen.x, ptScreen.y);
            if (PointInRectF(dip, GetMainCardRect())) {
                const auto* mc = GetSelectedMonitorConfig();
                if (mc) {
                    int notches = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
                    int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 5 : 1;
                    SetSelectedSharpness(mc->sharpness + notches * step, false);
                }
            }
            return 0;
        }

        case WM_KEYDOWN: {
            const auto& displays = engine_.GetDisplays();
            const auto* mc = GetSelectedMonitorConfig();
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

            if (wParam == VK_LEFT || wParam == VK_DOWN) {
                if (mc) SetSelectedSharpness(mc->sharpness - (shift ? 5 : 1), false);
                return 0;
            }
            if (wParam == VK_RIGHT || wParam == VK_UP) {
                if (mc) SetSelectedSharpness(mc->sharpness + (shift ? 5 : 1), false);
                return 0;
            }
            if (wParam == VK_PRIOR) { // PageUp
                if (mc) SetSelectedSharpness(mc->sharpness + 5, false);
                return 0;
            }
            if (wParam == VK_NEXT) { // PageDown
                if (mc) SetSelectedSharpness(mc->sharpness - 5, false);
                return 0;
            }
            if (wParam == VK_SPACE) {
                ToggleSelectedMonitorEnabled();
                return 0;
            }
            if (wParam == 'R') {
                SetSelectedSharpness(kDefaultSharpnessPercent, false);
                return 0;
            }
            if (wParam == VK_TAB && !displays.empty()) {
                int curIdx = 0;
                for (size_t i = 0; i < displays.size(); ++i) {
                    if (displays[i].id == config_.lastSelectedMonitorId) {
                        curIdx = static_cast<int>(i);
                        break;
                    }
                }
                int nextIdx = shift
                    ? (curIdx - 1 + static_cast<int>(displays.size())) % static_cast<int>(displays.size())
                    : (curIdx + 1) % static_cast<int>(displays.size());
                SelectMonitorByIndex(nextIdx);
                return 0;
            }
            if (wParam >= '1' && wParam <= '9') {
                int idx = static_cast<int>(wParam - '1');
                if (static_cast<size_t>(idx) < displays.size()) {
                    SelectMonitorByIndex(idx);
                }
                return 0;
            }
            break;
        }

        case kWmTrayCallback: {
            switch (LOWORD(lParam)) {
                case WM_LBUTTONUP:
                    if (IsWindowVisible(hwnd_) && GetForegroundWindow() == hwnd_) {
                        HideToTray();
                    } else {
                        ShowAndActivateWindow();
                    }
                    return 0;
                case WM_MBUTTONUP:
                    ToggleMasterEnabled();
                    return 0;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    tray_.ShowContextMenu(hwnd_, config_, engine_.GetDisplays());
                    return 0;
                default:
                    break;
            }
            return 0;
        }

        case WM_COMMAND: {
            UINT cmd = LOWORD(wParam);
            if (cmd == kCmdTrayOpen) {
                ShowAndActivateWindow();
                return 0;
            }
            if (cmd == kCmdTrayMasterToggle) {
                ToggleMasterEnabled();
                return 0;
            }
            if (cmd == kCmdTrayIdentify) {
                identify_.ShowForDisplays(engine_.GetDisplays());
                return 0;
            }
            if (cmd == kCmdTrayStartWindows) {
                bool target = !config_.startWithWindows;
                if (ConfigManager::SetStartupRegistryEnabled(target)) {
                    config_.startWithWindows = target;
                    ScheduleSaveConfig(true);
                    if (IsWindowVisible(hwnd_)) InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return 0;
            }
            if (cmd == kCmdTrayExit) {
                RequestCleanExit();
                return 0;
            }
            if (cmd >= kCmdTrayMonitorBase && cmd < kCmdTrayMonitorBase + 32) {
                size_t idx = cmd - kCmdTrayMonitorBase;
                const auto& displays = engine_.GetDisplays();
                if (idx < displays.size()) {
                    const auto& d = displays[idx];
                    auto& mc = config_.GetOrCreateMonitor(d.id, WideToUtf8(d.friendlyName));
                    mc.enabled = !mc.enabled;
                    if (mc.enabled) {
                        if (mc.sharpness <= 0) mc.sharpness = kDefaultSharpnessPercent;
                        if (!config_.masterEnabled) config_.masterEnabled = true;
                    }
                    engine_.ApplyConfig(config_, false);
                    ScheduleSaveConfig(true);
                    RefreshStatusAndRepaint();
                }
                return 0;
            }
            break;
        }

        case WM_CLOSE: {
            if (config_.minimizeToTrayOnClose) {
                HideToTray();
                return 0;
            }
            RequestCleanExit();
            return 0;
        }

        case WM_DESTROY: {
            PostQuitMessage(0);
            return 0;
        }

        default:
            break;
    }

    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

} // namespace pms
