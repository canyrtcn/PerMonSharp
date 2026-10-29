#include "display/DisplayManager.h"
#include "common/StringUtils.h"

#include <shellscalingapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace pms {

namespace {

struct DisplayConfigMeta {
    std::wstring friendlyName;
    std::wstring devicePath;
    double refreshHz = 0.0;
    bool isInternal = false;
};

std::unordered_map<std::wstring, DisplayConfigMeta> QueryDisplayConfigMap() {
    std::unordered_map<std::wstring, DisplayConfigMeta> result;

    UINT32 numPaths = 0, numModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &numPaths, &numModes) != ERROR_SUCCESS || numPaths == 0) {
        return result;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(numPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(numModes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &numPaths, paths.data(), &numModes, modes.data(), nullptr) != ERROR_SUCCESS) {
        return result;
    }

    for (UINT32 i = 0; i < numPaths; ++i) {
        const auto& p = paths[i];

        DISPLAYCONFIG_SOURCE_DEVICE_NAME srcName{};
        srcName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        srcName.header.size = sizeof(srcName);
        srcName.header.adapterId = p.sourceInfo.adapterId;
        srcName.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&srcName.header) != ERROR_SUCCESS) {
            continue;
        }

        DisplayConfigMeta meta{};
        if (p.targetInfo.refreshRate.Denominator != 0 && p.targetInfo.refreshRate.Numerator != 0) {
            meta.refreshHz = static_cast<double>(p.targetInfo.refreshRate.Numerator) /
                             static_cast<double>(p.targetInfo.refreshRate.Denominator);
        }

        meta.isInternal =
            (p.targetInfo.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL) ||
            (p.targetInfo.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED) ||
            (p.targetInfo.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED);

        DISPLAYCONFIG_TARGET_DEVICE_NAME tgtName{};
        tgtName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgtName.header.size = sizeof(tgtName);
        tgtName.header.adapterId = p.targetInfo.adapterId;
        tgtName.header.id = p.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tgtName.header) == ERROR_SUCCESS) {
            meta.friendlyName = TrimWide(tgtName.monitorFriendlyDeviceName);
            meta.devicePath   = TrimWide(tgtName.monitorDevicePath);
        }

        result[srcName.viewGdiDeviceName] = std::move(meta);
    }

    return result;
}

} // namespace

std::vector<DisplayInfo> DisplayManager::EnumerateDisplays() {
    std::vector<DisplayInfo> list;

    auto cfgMap = QueryDisplayConfigMap();

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return list;
    }

    for (UINT ai = 0; ; ++ai) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(ai, &adapter) == DXGI_ERROR_NOT_FOUND) break;

        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        for (UINT oi = 0; ; ++oi) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(oi, &output) == DXGI_ERROR_NOT_FOUND) break;

            DXGI_OUTPUT_DESC od0{};
            if (FAILED(output->GetDesc(&od0)) || !od0.AttachedToDesktop) continue;

            DisplayInfo info{};
            info.adapterIndex = ai;
            info.outputIndex = oi;
            info.adapterLuid = ad.AdapterLuid;
            info.adapterName = TrimWide(ad.Description);
            info.gdiDeviceName = od0.DeviceName;
            info.hMonitor = od0.Monitor;
            info.desktopBounds = od0.DesktopCoordinates;
            info.rotation = (od0.Rotation == DXGI_MODE_ROTATION_UNSPECIFIED)
                            ? DXGI_MODE_ROTATION_IDENTITY
                            : od0.Rotation;

            ComPtr<IDXGIOutput6> out6;
            if (SUCCEEDED(output.As(&out6))) {
                DXGI_OUTPUT_DESC1 od1{};
                if (SUCCEEDED(out6->GetDesc1(&od1))) {
                    info.bitsPerColor = od1.BitsPerColor;
                    info.colorSpace = od1.ColorSpace;
                    info.desktopBounds = od1.DesktopCoordinates;
                    info.isHdrActive =
                        (od1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) ||
                        (od1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
                }
            }

            info.width  = info.desktopBounds.right - info.desktopBounds.left;
            info.height = info.desktopBounds.bottom - info.desktopBounds.top;
            if (info.width <= 0 || info.height <= 0) continue;

            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (info.hMonitor && GetMonitorInfoW(info.hMonitor, &mi)) {
                info.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
            }

            UINT dpiX = 96, dpiY = 96;
            if (info.hMonitor && SUCCEEDED(GetDpiForMonitor(info.hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
                info.dpiPercent = (dpiX * 100u + 48u) / 96u;
            }

            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsExW(info.gdiDeviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0)) {
                if (dm.dmDisplayFrequency > 1) {
                    info.refreshRateHz = static_cast<double>(dm.dmDisplayFrequency);
                }
            }

            auto it = cfgMap.find(info.gdiDeviceName);
            if (it != cfgMap.end()) {
                const auto& meta = it->second;
                if (meta.refreshHz > 1.0) {
                    info.refreshRateHz = meta.refreshHz;
                }
                if (!meta.friendlyName.empty()) {
                    info.friendlyName = meta.friendlyName;
                } else if (meta.isInternal) {
                    info.friendlyName = L"Internal Display";
                }
                if (!meta.devicePath.empty()) {
                    info.id = ToLowerAscii(WideToUtf8(meta.devicePath));
                }
            }

            if (info.friendlyName.empty()) {
                info.friendlyName = L"Generic Display";
            }
            if (info.id.empty()) {
                info.id = ToLowerAscii(WideToUtf8(info.gdiDeviceName));
            }
            info.displayLabel = info.friendlyName;

            list.push_back(std::move(info));
        }
    }

    // Sort: Primary first, then left-to-right, then top-to-bottom
    std::sort(list.begin(), list.end(), [](const DisplayInfo& a, const DisplayInfo& b) {
        if (a.isPrimary != b.isPrimary) return a.isPrimary > b.isPrimary;
        if (a.desktopBounds.left != b.desktopBounds.left) return a.desktopBounds.left < b.desktopBounds.left;
        return a.desktopBounds.top < b.desktopBounds.top;
    });

    // Disambiguate identical friendly names when multiple monitors of the same model are connected
    std::unordered_map<std::wstring, int> counts;
    for (const auto& d : list) counts[d.friendlyName]++;

    std::unordered_map<std::wstring, int> indices;
    for (auto& d : list) {
        if (counts[d.friendlyName] > 1) {
            int idx = ++indices[d.friendlyName];
            d.displayLabel = d.friendlyName + L" #" + std::to_wstring(idx);
        } else {
            d.displayLabel = d.friendlyName;
        }
    }

    return list;
}

} // namespace pms
