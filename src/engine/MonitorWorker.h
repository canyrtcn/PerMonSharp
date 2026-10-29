#pragma once

#include "display/DisplayInfo.h"
#include "engine/GpuSharpenProcessor.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <dcomp.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pms {

enum class WorkerState {
    Stopped = 0,
    Starting,
    Active,
    PausedFullscreen,
    WaitingForDesktop, // e.g. UAC Secure Desktop / Lock Screen / display mode switch
    Error
};

class MonitorWorker {
public:
    MonitorWorker(const DisplayInfo& display, int initialSharpnessPercent);
    ~MonitorWorker();

    MonitorWorker(const MonitorWorker&) = delete;
    MonitorWorker& operator=(const MonitorWorker&) = delete;

    bool Start();
    void Stop();

    void SetSharpnessPercent(int percent);
    int GetSharpnessPercent() const { return sharpnessPercent_.load(std::memory_order_relaxed); }

    void SetPausedForFullscreen(bool paused);
    bool IsPausedForFullscreen() const { return pausedForFullscreen_.load(std::memory_order_relaxed); }

    const DisplayInfo& GetDisplayInfo() const { return display_; }
    WorkerState GetState() const { return state_.load(std::memory_order_acquire); }
    std::wstring GetErrorText() const;

private:
    void ThreadMain();
    static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    DisplayInfo display_;
    std::atomic<int> sharpnessPercent_{ 35 };
    std::atomic<uint64_t> paramGeneration_{ 1 };
    std::atomic<DWORD> lastSliderTouchTick_{ 0 };

    std::atomic<bool> stopRequested_{ false };
    std::atomic<bool> pausedForFullscreen_{ false };
    std::atomic<WorkerState> state_{ WorkerState::Stopped };

    mutable std::mutex errorMutex_;
    std::wstring errorText_;

    HANDLE wakeEvent_ = nullptr;
    std::thread workerThread_;

    void SetError(WorkerState st, const std::wstring& msg);
};

} // namespace pms
