#pragma once

#include "config/Config.h"
#include "display/DisplayInfo.h"
#include "engine/MonitorWorker.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pms {

class EngineController {
public:
    EngineController();
    ~EngineController();

    EngineController(const EngineController&) = delete;
    EngineController& operator=(const EngineController&) = delete;

    // Re-enumerates physical displays and reconciles workers with `config`.
    void RefreshDisplays(AppConfig& config);

    // Reconciles active MonitorWorkers with `config` without re-enumerating DXGI topology.
    // If `keepZeroWhileDragging` is true, an existing worker at 0% sharpness stays alive during a slider drag.
    void ApplyConfig(const AppConfig& config, bool keepZeroWhileDragging = false);

    // Updates per-monitor fullscreen auto-pause states. Returns true if any worker's state changed.
    bool UpdateFullscreenStates(const AppConfig& config);

    // Cleanly stops all workers (e.g. on system suspend or shutdown).
    void StopAll();

    const std::vector<DisplayInfo>& GetDisplays() const { return displays_; }
    const DisplayInfo* FindDisplay(const std::string& id) const;

    WorkerState GetDisplayWorkerState(const std::string& id) const;
    std::wstring GetDisplayWorkerError(const std::string& id) const;
    size_t GetActiveWorkerCount() const { return workers_.size(); }

    // Concise human-readable status line for footer / tray tooltip
    std::wstring BuildStatusSummary(const AppConfig& config) const;

private:
    static bool SameDisplayGeometry(const DisplayInfo& a, const DisplayInfo& b);

    DWORD selfPid_ = 0;
    std::vector<DisplayInfo> displays_;
    std::unordered_map<std::string, std::unique_ptr<MonitorWorker>> workers_;
};

} // namespace pms
