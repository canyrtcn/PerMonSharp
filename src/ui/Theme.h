#pragma once

#include <windows.h>
#include <d2d1.h>

namespace pms {

struct UiPalette {
    bool isDark = true;
    D2D1_COLOR_F windowBg;
    D2D1_COLOR_F panelBg;
    D2D1_COLOR_F panelHoverBg;
    D2D1_COLOR_F tabInactiveBg;
    D2D1_COLOR_F tabSelectedBg;
    D2D1_COLOR_F border;
    D2D1_COLOR_F borderStrong;
    D2D1_COLOR_F divider;
    D2D1_COLOR_F textPrimary;
    D2D1_COLOR_F textSecondary;
    D2D1_COLOR_F textMuted;
    D2D1_COLOR_F accent;
    D2D1_COLOR_F accentHover;
    D2D1_COLOR_F accentText;
    D2D1_COLOR_F trackBg;
    D2D1_COLOR_F thumbFill;
    D2D1_COLOR_F thumbBorder;
    D2D1_COLOR_F statusAmber;
    D2D1_COLOR_F statusError;
};

inline D2D1_COLOR_F RgbaHex(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(
        static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
        static_cast<float>((rgb >> 8)  & 0xFF) / 255.0f,
        static_cast<float>( rgb        & 0xFF) / 255.0f,
        a);
}

inline bool QuerySystemUsesDarkTheme() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return true;
    }
    DWORD lightTheme = 0;
    DWORD cb = sizeof(lightTheme);
    LSTATUS st = RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr,
                                  reinterpret_cast<LPBYTE>(&lightTheme), &cb);
    RegCloseKey(hKey);
    if (st != ERROR_SUCCESS) return true;
    return lightTheme == 0;
}

inline UiPalette GetSystemPalette() {
    bool dark = QuerySystemUsesDarkTheme();
    UiPalette p{};
    p.isDark = dark;

    if (dark) {
        p.windowBg      = RgbaHex(0x1C1C1C);
        p.panelBg       = RgbaHex(0x252525);
        p.panelHoverBg  = RgbaHex(0x2D2D2D);
        p.tabInactiveBg = RgbaHex(0x222222);
        p.tabSelectedBg = RgbaHex(0x2B2B2B);
        p.border        = RgbaHex(0x333333);
        p.borderStrong  = RgbaHex(0x454545);
        p.divider       = RgbaHex(0x2F2F2F);
        p.textPrimary   = RgbaHex(0xF2F2F2);
        p.textSecondary = RgbaHex(0xA4A4A4);
        p.textMuted     = RgbaHex(0x6E6E6E);
        p.accent        = RgbaHex(0x4CC2FF);
        p.accentHover   = RgbaHex(0x6CD0FF);
        p.accentText    = RgbaHex(0x0B1620);
        p.trackBg       = RgbaHex(0x3D3D3D);
        p.thumbFill     = RgbaHex(0xEAEAEA);
        p.thumbBorder   = RgbaHex(0x555555);
        p.statusAmber   = RgbaHex(0xF2B84B);
        p.statusError   = RgbaHex(0xF06A6A);
    } else {
        p.windowBg      = RgbaHex(0xF3F3F3);
        p.panelBg       = RgbaHex(0xFFFFFF);
        p.panelHoverBg  = RgbaHex(0xF7F7F7);
        p.tabInactiveBg = RgbaHex(0xEAEAEA);
        p.tabSelectedBg = RgbaHex(0xFFFFFF);
        p.border        = RgbaHex(0xDFDFDF);
        p.borderStrong  = RgbaHex(0xC6C6C6);
        p.divider       = RgbaHex(0xEAEAEA);
        p.textPrimary   = RgbaHex(0x1A1A1A);
        p.textSecondary = RgbaHex(0x5D5D5D);
        p.textMuted     = RgbaHex(0x949494);
        p.accent        = RgbaHex(0x0067C0);
        p.accentHover   = RgbaHex(0x1877CA);
        p.accentText    = RgbaHex(0xFFFFFF);
        p.trackBg       = RgbaHex(0xD4D4D4);
        p.thumbFill     = RgbaHex(0xFFFFFF);
        p.thumbBorder   = RgbaHex(0xB5B5B5);
        p.statusAmber   = RgbaHex(0xC97D09);
        p.statusError   = RgbaHex(0xC42B1C);
    }

    return p;
}

} // namespace pms
