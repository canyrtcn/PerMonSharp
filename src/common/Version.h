#pragma once

#include <windows.h>
#include <cstdint>

namespace pms {

inline constexpr wchar_t kAppName[]        = L"PerMonSharp";
inline constexpr wchar_t kAppVersion[]     = L"1.0.0";
inline constexpr wchar_t kMainWndClass[]   = L"PerMonSharpMainWnd";
inline constexpr wchar_t kOverlayWndClass[]= L"PerMonSharpOverlayWnd";
inline constexpr wchar_t kIdentifyWndClass[]= L"PerMonSharpIdentifyWnd";
inline constexpr wchar_t kSingleInstMutex[]= L"Local\\PerMonSharp_SingleInstance_v1";
inline constexpr wchar_t kShowWindowMsg[]  = L"PerMonSharp_ShowMainWindow_v1";

inline constexpr int kDefaultSharpnessPercent = 35;
inline constexpr int kMinSharpnessPercent     = 0;
inline constexpr int kMaxSharpnessPercent     = 100;

struct OsVersionInfo {
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t build = 0;
};

inline OsVersionInfo GetRealOsVersion() {
    OsVersionInfo info{};
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return info;

    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (!fn) return info;

    RTL_OSVERSIONINFOW rovi{};
    rovi.dwOSVersionInfoSize = sizeof(rovi);
    if (fn(&rovi) == 0) {
        info.major = rovi.dwMajorVersion;
        info.minor = rovi.dwMinorVersion;
        info.build = rovi.dwBuildNumber;
    }
    return info;
}

// WDA_EXCLUDEFROMCAPTURE requires Windows 10 Version 2004 (Build 19041) or newer.
inline bool IsExcludeFromCaptureSupported() {
    return GetRealOsVersion().build >= 19041;
}

// Borderless Windows.Graphics.Capture for unpackaged Win32 apps requires Windows 11 (Build 22000+).
inline bool IsWindows11OrGreater() {
    return GetRealOsVersion().build >= 22000;
}

} // namespace pms
