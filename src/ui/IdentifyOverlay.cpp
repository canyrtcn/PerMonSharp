#include "ui/IdentifyOverlay.h"
#include "common/Version.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <mutex>

using Microsoft::WRL::ComPtr;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace pms {

namespace {
constexpr UINT_PTR kTimerDismiss = 1;
constexpr UINT kIdentifyDurationMs = 1400;
}

IdentifyOverlay::~IdentifyOverlay() {
    DismissAll();
}

void IdentifyOverlay::DismissAll() {
    for (auto& b : badges_) {
        if (b.hwnd && IsWindow(b.hwnd)) {
            KillTimer(b.hwnd, kTimerDismiss);
            DestroyWindow(b.hwnd);
        }
    }
    badges_.clear();
}

void IdentifyOverlay::ShowForDisplays(const std::vector<DisplayInfo>& displays) {
    DismissAll();
    if (displays.empty()) return;

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    static std::once_flag s_reg;
    std::call_once(s_reg, [&]() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &IdentifyOverlay::WndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = kIdentifyWndClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
    });

    badges_.reserve(displays.size());
    for (size_t i = 0; i < displays.size(); ++i) {
        const auto& d = displays[i];
        UINT dpi = (d.dpiPercent * 96u) / 100u;
        if (dpi < 96) dpi = 96;

        int wPx = MulDiv(240, static_cast<int>(dpi), 96);
        int hPx = MulDiv(76,  static_cast<int>(dpi), 96);
        int marginPx = MulDiv(36, static_cast<int>(dpi), 96);

        int x = d.desktopBounds.left + marginPx;
        int y = d.desktopBounds.top  + marginPx;

        BadgeWindow bw{};
        bw.number = static_cast<int>(i + 1);
        bw.label = d.displayLabel;
        bw.dpi = dpi;

        HWND hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kIdentifyWndClass,
            L"",
            WS_POPUP,
            x, y, wPx, hPx,
            nullptr, nullptr, hInst, this);

        if (hwnd) {
            bw.hwnd = hwnd;
            badges_.push_back(bw);
            SetLayeredWindowAttributes(hwnd, 0, 238, LWA_ALPHA);
            SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);

            // Round corners cleanly on Windows 11
            HRGN rgn = CreateRoundRectRgn(0, 0, wPx + 1, hPx + 1,
                                          MulDiv(12, static_cast<int>(dpi), 96),
                                          MulDiv(12, static_cast<int>(dpi), 96));
            SetWindowRgn(hwnd, rgn, FALSE);

            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            InvalidateRect(hwnd, nullptr, FALSE);
            SetTimer(hwnd, kTimerDismiss, kIdentifyDurationMs, nullptr);
        }
    }
}

void IdentifyOverlay::RenderBadge(HWND hwnd, const BadgeWindow& bw) {
    PAINTSTRUCT ps{};
    BeginPaint(hwnd, &ps);

    RECT rc{};
    GetClientRect(hwnd, &rc);

    ComPtr<ID2D1Factory> d2dFactory;
    ComPtr<IDWriteFactory> dwFactory;
    if (SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.GetAddressOf())) &&
        SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                      reinterpret_cast<IUnknown**>(dwFactory.GetAddressOf())))) {
        D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            static_cast<float>(bw.dpi),
            static_cast<float>(bw.dpi));

        D2D1_HWND_RENDER_TARGET_PROPERTIES hrtp = D2D1::HwndRenderTargetProperties(
            hwnd, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top));

        ComPtr<ID2D1HwndRenderTarget> rt;
        if (SUCCEEDED(d2dFactory->CreateHwndRenderTarget(rtp, hrtp, &rt))) {
            rt->BeginDraw();
            rt->Clear(D2D1::ColorF(0.11f, 0.12f, 0.14f, 1.0f));

            D2D1_SIZE_F sz = rt->GetSize();
            ComPtr<ID2D1SolidColorBrush> borderBr, accentBr, textBr, subBr;
            rt->CreateSolidColorBrush(D2D1::ColorF(0.30f, 0.76f, 1.0f, 1.0f), &accentBr);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.26f, 0.28f, 0.32f, 1.0f), &borderBr);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.96f, 0.98f, 1.0f), &textBr);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.68f, 0.71f, 0.75f, 1.0f), &subBr);

            D2D1_ROUNDED_RECT outer = D2D1::RoundedRect(
                D2D1::RectF(1.0f, 1.0f, sz.width - 1.0f, sz.height - 1.0f), 6.0f, 6.0f);
            rt->DrawRoundedRectangle(outer, borderBr.Get(), 1.5f);

            // Number pill on left
            D2D1_ROUNDED_RECT numBox = D2D1::RoundedRect(
                D2D1::RectF(14.0f, 14.0f, 62.0f, sz.height - 14.0f), 5.0f, 5.0f);
            rt->FillRoundedRectangle(numBox, accentBr.Get());

            ComPtr<ID2D1SolidColorBrush> darkNumBr;
            rt->CreateSolidColorBrush(D2D1::ColorF(0.05f, 0.09f, 0.13f, 1.0f), &darkNumBr);

            ComPtr<IDWriteTextFormat> numFmt, titleFmt, subFmt;
            dwFactory->CreateTextFormat(L"Segoe UI Variable Display", nullptr,
                DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                24.0f, L"en-US", &numFmt);
            if (numFmt) {
                numFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                numFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                std::wstring numStr = std::to_wstring(bw.number);
                rt->DrawTextW(numStr.c_str(), static_cast<UINT32>(numStr.size()),
                              numFmt.Get(), numBox.rect, darkNumBr.Get());
            }

            dwFactory->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
                DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                14.5f, L"en-US", &titleFmt);
            if (titleFmt) {
                titleFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
                ComPtr<IDWriteInlineObject> ell;
                dwFactory->CreateEllipsisTrimmingSign(titleFmt.Get(), &ell);
                titleFmt->SetTrimming(&trim, ell.Get());
                rt->DrawTextW(bw.label.c_str(), static_cast<UINT32>(bw.label.size()),
                              titleFmt.Get(), D2D1::RectF(74.0f, 17.0f, sz.width - 14.0f, 40.0f), textBr.Get());
            }

            dwFactory->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                11.5f, L"en-US", &subFmt);
            if (subFmt) {
                const wchar_t kSub[] = L"PerMonSharp Display";
                rt->DrawTextW(kSub, static_cast<UINT32>(std::size(kSub) - 1),
                              subFmt.Get(), D2D1::RectF(74.0f, 41.0f, sz.width - 14.0f, 62.0f), subBr.Get());
            }

            rt->EndDraw();
        }
    }

    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK IdentifyOverlay::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    auto* self = reinterpret_cast<IdentifyOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_TIMER:
            if (wParam == kTimerDismiss && self) {
                self->DismissAll();
                return 0;
            }
            break;
        case WM_PAINT:
            if (self) {
                for (const auto& b : self->badges_) {
                    if (b.hwnd == hwnd) {
                        self->RenderBadge(hwnd, b);
                        return 0;
                    }
                }
            }
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace pms
