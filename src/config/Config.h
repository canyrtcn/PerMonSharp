#pragma once

#include "common/Version.h"
#include <string>
#include <vector>
#include <algorithm>

namespace pms {

struct MonitorConfig {
    std::string monitorId;
    std::string lastKnownName;
    bool enabled = false;
    int sharpness = kDefaultSharpnessPercent; // [0 .. 100]
};

struct AppConfig {
    bool masterEnabled = true;
    bool pauseOnFullscreen = true;
    bool minimizeToTrayOnClose = true;
    bool startWithWindows = false;
    bool enableGlobalHotkey = true; // Ctrl + Alt + S
    std::string lastSelectedMonitorId;
    std::vector<MonitorConfig> monitors;

    MonitorConfig& GetOrCreateMonitor(const std::string& id, const std::string& friendlyName = {}) {
        for (auto& m : monitors) {
            if (m.monitorId == id) {
                if (!friendlyName.empty()) m.lastKnownName = friendlyName;
                return m;
            }
        }
        MonitorConfig mc{};
        mc.monitorId = id;
        mc.lastKnownName = friendlyName;
        mc.enabled = false;
        mc.sharpness = kDefaultSharpnessPercent;
        monitors.push_back(mc);
        return monitors.back();
    }

    const MonitorConfig* FindMonitor(const std::string& id) const {
        for (const auto& m : monitors) {
            if (m.monitorId == id) return &m;
        }
        return nullptr;
    }
};

class ConfigManager {
public:
    // Resolves settings.json path (portable beside .exe if forcePortable/portable.txt/settings.json exists,
    // otherwise %APPDATA%\PerMonSharp\settings.json).
    static std::wstring ResolveConfigPath(bool forcePortable = false);

    static bool LoadFromFile(const std::wstring& path, AppConfig& outConfig);
    static bool SaveToFile(const std::wstring& path, const AppConfig& config);

    // Pure string serialization helpers (exposed for unit testing)
    static std::string SerializeToJson(const AppConfig& config);
    static bool ParseFromJson(const std::string& jsonText, AppConfig& outConfig);

    // Windows HKCU\Software\Microsoft\Windows\CurrentVersion\Run integration
    static bool IsStartupRegistryEnabled();
    static bool SetStartupRegistryEnabled(bool enable);
};

} // namespace pms
