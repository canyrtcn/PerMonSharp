#include "engine/EngineController.h"
#include "display/DisplayManager.h"
#include "display/FullscreenDetector.h"
#include "common/StringUtils.h"

namespace pms {

EngineController::EngineController()
    : selfPid_(GetCurrentProcessId())
{
}

EngineController::~EngineController() {
    StopAll();
}

bool EngineController::SameDisplayGeometry(const DisplayInfo& a, const DisplayInfo& b) {
    return a.hMonitor == b.hMonitor &&
           a.adapterLuid.LowPart == b.adapterLuid.LowPart &&
           a.adapterLuid.HighPart == b.adapterLuid.HighPart &&
           a.desktopBounds.left == b.desktopBounds.left &&
           a.desktopBounds.top == b.desktopBounds.top &&
           a.desktopBounds.right == b.desktopBounds.right &&
           a.desktopBounds.bottom == b.desktopBounds.bottom &&
           a.rotation == b.rotation &&
           a.isHdrActive == b.isHdrActive;
}

void EngineController::RefreshDisplays(AppConfig& config) {
    displays_ = DisplayManager::EnumerateDisplays();

    // Ensure every connected display has an entry in config (with friendly name updated)
    for (const auto& d : displays_) {
        config.GetOrCreateMonitor(d.id, WideToUtf8(d.friendlyName));
    }

    // Ensure lastSelectedMonitorId points to a currently connected display
    bool selectedValid = false;
    for (const auto& d : displays_) {
        if (d.id == config.lastSelectedMonitorId) {
            selectedValid = true;
            break;
        }
    }
    if (!selectedValid) {
        config.lastSelectedMonitorId = displays_.empty() ? std::string{} : displays_.front().id;
    }

    // Stop workers whose display disconnected or changed geometry/HDR/rotation
    for (auto it = workers_.begin(); it != workers_.end(); ) {
        const DisplayInfo* cur = FindDisplay(it->first);
        if (!cur || !SameDisplayGeometry(it->second->GetDisplayInfo(), *cur)) {
            it->second->Stop();
            it = workers_.erase(it);
        } else {
            ++it;
        }
    }

    ApplyConfig(config, false);
}

void EngineController::ApplyConfig(const AppConfig& config, bool keepZeroWhileDragging) {
    if (!config.masterEnabled) {
        StopAll();
        return;
    }

    // Remove workers for monitors no longer enabled
    for (auto it = workers_.begin(); it != workers_.end(); ) {
        const MonitorConfig* mc = config.FindMonitor(it->first);
        const DisplayInfo* di = FindDisplay(it->first);
        bool shouldRun = (di != nullptr) && (mc != nullptr) && mc->enabled &&
                         (mc->sharpness > 0 || keepZeroWhileDragging);
        if (!shouldRun) {
            it->second->Stop();
            it = workers_.erase(it);
        } else {
            ++it;
        }
    }

    // Start or update workers for enabled monitors
    for (const auto& d : displays_) {
        const MonitorConfig* mc = config.FindMonitor(d.id);
        if (!mc || !mc->enabled) continue;

        auto it = workers_.find(d.id);
        if (it != workers_.end()) {
            it->second->SetSharpnessPercent(mc->sharpness);
            bool fsPaused = config.pauseOnFullscreen &&
                            FullscreenDetector::IsMonitorCoveredByFullscreenApp(d.hMonitor, selfPid_);
            it->second->SetPausedForFullscreen(fsPaused);
        } else if (mc->sharpness > 0) {
            auto worker = std::make_unique<MonitorWorker>(d, mc->sharpness);
            bool fsPaused = config.pauseOnFullscreen &&
                            FullscreenDetector::IsMonitorCoveredByFullscreenApp(d.hMonitor, selfPid_);
            worker->SetPausedForFullscreen(fsPaused);
            worker->Start();
            workers_.emplace(d.id, std::move(worker));
        }
    }
}

bool EngineController::UpdateFullscreenStates(const AppConfig& config) {
    bool anyChanged = false;
    for (auto& kv : workers_) {
        MonitorWorker* w = kv.second.get();
        bool shouldPause = config.pauseOnFullscreen &&
                           FullscreenDetector::IsMonitorCoveredByFullscreenApp(w->GetDisplayInfo().hMonitor, selfPid_);
        if (w->IsPausedForFullscreen() != shouldPause) {
            w->SetPausedForFullscreen(shouldPause);
            anyChanged = true;
        }
    }
    return anyChanged;
}

void EngineController::StopAll() {
    for (auto& kv : workers_) {
        kv.second->Stop();
    }
    workers_.clear();
}

const DisplayInfo* EngineController::FindDisplay(const std::string& id) const {
    for (const auto& d : displays_) {
        if (d.id == id) return &d;
    }
    return nullptr;
}

WorkerState EngineController::GetDisplayWorkerState(const std::string& id) const {
    auto it = workers_.find(id);
    if (it == workers_.end()) return WorkerState::Stopped;
    return it->second->GetState();
}

std::wstring EngineController::GetDisplayWorkerError(const std::string& id) const {
    auto it = workers_.find(id);
    if (it == workers_.end()) return {};
    return it->second->GetErrorText();
}

std::wstring EngineController::BuildStatusSummary(const AppConfig& config) const {
    if (displays_.empty()) {
        return L"No active displays detected";
    }

    if (!config.masterEnabled) {
        return L"All sharpening paused";
    }

    int activeCount = 0;
    int pausedCount = 0;
    int errorCount = 0;
    std::wstring firstError;

    for (const auto& d : displays_) {
        WorkerState st = GetDisplayWorkerState(d.id);
        if (st == WorkerState::Active || st == WorkerState::Starting || st == WorkerState::WaitingForDesktop) {
            ++activeCount;
        } else if (st == WorkerState::PausedFullscreen) {
            ++pausedCount;
        } else if (st == WorkerState::Error) {
            ++errorCount;
            if (firstError.empty()) firstError = GetDisplayWorkerError(d.id);
        }
    }

    if (errorCount > 0 && activeCount == 0 && pausedCount == 0) {
        return firstError.empty() ? L"Capture unavailable on selected display" : firstError;
    }

    if (activeCount == 0 && pausedCount == 0) {
        return L"No displays sharpened (original output)";
    }

    std::wstring s;
    if (activeCount > 0) {
        s = L"Active on " + std::to_wstring(activeCount) + L" of " +
            std::to_wstring(displays_.size()) + (displays_.size() == 1 ? L" display" : L" displays");
    }
    if (pausedCount > 0) {
        if (!s.empty()) s += L" · ";
        s += std::to_wstring(pausedCount) + L" paused (fullscreen)";
    }
    return s;
}

} // namespace pms
