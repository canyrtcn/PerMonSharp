#include "engine/MonitorWorker.h"
#include "common/Version.h"

#include <roapi.h>
#include <windows.graphics.capture.h>
#include <Windows.Graphics.Capture.Interop.h>

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

// Standard Windows SDK COM interface to unwrap ID3D11Texture2D from WinRT IDirect3DSurface
namespace {
struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")) IDirect3DDxgiInterfaceAccess : ::IUnknown {
    virtual HRESULT __stdcall GetInterface(REFIID iid, void** p) = 0;
};

using CreateDirect3D11DeviceFromDXGIDeviceFn = HRESULT(WINAPI*)(IDXGIDevice*, IInspectable**);

// Minimal self-contained Windows.Graphics.Capture fallback session (used on Win11 if DXGI DuplicateOutput is unavailable)
class WgcFallbackSession {
public:
    ~WgcFallbackSession() { Shutdown(); }

    bool Initialize(ID3D11Device* d3dDevice, HMONITOR hMonitor) {
        Shutdown();
        if (!pms::IsWindows11OrGreater() || !d3dDevice || !hMonitor) return false;

        HMODULE d3d11Mod = GetModuleHandleW(L"d3d11.dll");
        if (!d3d11Mod) return false;
        auto createWinRtDev = reinterpret_cast<CreateDirect3D11DeviceFromDXGIDeviceFn>(
            GetProcAddress(d3d11Mod, "CreateDirect3D11DeviceFromDXGIDevice"));
        if (!createWinRtDev) return false;

        ComPtr<IDXGIDevice> dxgiDev;
        if (FAILED(d3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) return false;

        ComPtr<IInspectable> inspectableDev;
        if (FAILED(createWinRtDev(dxgiDev.Get(), &inspectableDev))) return false;
        if (FAILED(inspectableDev.As(&winrtDevice_))) return false;

        // Create GraphicsCaptureItem for HMONITOR
        HSTRING hClassItem = nullptr;
        const wchar_t* kItemClass = RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem;
        if (FAILED(WindowsCreateString(kItemClass, static_cast<UINT32>(wcslen(kItemClass)), &hClassItem)))
            return false;

        ComPtr<IGraphicsCaptureItemInterop> interop;
        HRESULT hr = RoGetActivationFactory(hClassItem, IID_PPV_ARGS(&interop));
        WindowsDeleteString(hClassItem);
        if (FAILED(hr) || !interop) return false;

        if (FAILED(interop->CreateForMonitor(
                hMonitor,
                __uuidof(ABI::Windows::Graphics::Capture::IGraphicsCaptureItem),
                reinterpret_cast<void**>(item_.GetAddressOf())))) {
            return false;
        }

        ABI::Windows::Graphics::SizeInt32 itemSize{};
        if (FAILED(item_->get_Size(&itemSize)) || itemSize.Width <= 0 || itemSize.Height <= 0) {
            return false;
        }
        lastSize_ = itemSize;

        HSTRING hClassPool = nullptr;
        const wchar_t* kPoolClass = RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool;
        if (FAILED(WindowsCreateString(kPoolClass, static_cast<UINT32>(wcslen(kPoolClass)), &hClassPool)))
            return false;

        ComPtr<ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics2> poolStatics;
        hr = RoGetActivationFactory(hClassPool, IID_PPV_ARGS(&poolStatics));
        WindowsDeleteString(hClassPool);
        if (FAILED(hr) || !poolStatics) return false;

        hr = poolStatics->CreateFreeThreaded(
            winrtDevice_.Get(),
            ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized,
            2,
            itemSize,
            &framePool_);
        if (FAILED(hr) || !framePool_) return false;

        if (FAILED(framePool_->CreateCaptureSession(item_.Get(), &session_)) || !session_) {
            return false;
        }

        // Disable captured cursor (hardware cursor renders natively at 0ms latency above overlay)
        ComPtr<ABI::Windows::Graphics::Capture::IGraphicsCaptureSession2> sess2;
        if (SUCCEEDED(session_.As(&sess2)) && sess2) {
            sess2->put_IsCursorCaptureEnabled(FALSE);
        }

        // Disable yellow capture border on Windows 11
        ComPtr<ABI::Windows::Graphics::Capture::IGraphicsCaptureSession3> sess3;
        if (SUCCEEDED(session_.As(&sess3)) && sess3) {
            sess3->put_IsBorderRequired(FALSE);
        }

        if (FAILED(session_->StartCapture())) {
            Shutdown();
            return false;
        }

        active_ = true;
        return true;
    }

    bool TryAcquireTexture(ComPtr<ID3D11Texture2D>& outTex, bool& outSizeChanged) {
        outTex.Reset();
        outSizeChanged = false;
        if (!active_ || !framePool_) return false;

        // Drain to latest queued frame if multiple accumulated
        ComPtr<ABI::Windows::Graphics::Capture::IDirect3D11CaptureFrame> latestFrame;
        for (int i = 0; i < 3; ++i) {
            ComPtr<ABI::Windows::Graphics::Capture::IDirect3D11CaptureFrame> f;
            if (FAILED(framePool_->TryGetNextFrame(&f)) || !f) break;
            latestFrame = f;
        }
        if (!latestFrame) return false;

        ABI::Windows::Graphics::SizeInt32 contentSize{};
        if (SUCCEEDED(latestFrame->get_ContentSize(&contentSize))) {
            if (contentSize.Width != lastSize_.Width || contentSize.Height != lastSize_.Height) {
                outSizeChanged = true;
                return false;
            }
        }

        ComPtr<ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface> surface;
        if (FAILED(latestFrame->get_Surface(&surface)) || !surface) return false;

        ComPtr<IDirect3DDxgiInterfaceAccess> access;
        if (FAILED(surface.As(&access)) || !access) return false;

        return SUCCEEDED(access->GetInterface(IID_PPV_ARGS(&outTex))) && outTex != nullptr;
    }

    void Shutdown() {
        active_ = false;
        if (session_) {
            ComPtr<ABI::Windows::Foundation::IClosable> c;
            if (SUCCEEDED(session_.As(&c)) && c) c->Close();
            session_.Reset();
        }
        if (framePool_) {
            ComPtr<ABI::Windows::Foundation::IClosable> c;
            if (SUCCEEDED(framePool_.As(&c)) && c) c->Close();
            framePool_.Reset();
        }
        item_.Reset();
        winrtDevice_.Reset();
    }

    bool IsActive() const { return active_; }

private:
    bool active_ = false;
    ABI::Windows::Graphics::SizeInt32 lastSize_{};
    ComPtr<ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice> winrtDevice_;
    ComPtr<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem> item_;
    ComPtr<ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool> framePool_;
    ComPtr<ABI::Windows::Graphics::Capture::IGraphicsCaptureSession> session_;
};

void RegisterOverlayClassOnce(HINSTANCE hInst, WNDPROC proc) {
    static std::once_flag s_once;
    std::call_once(s_once, [&]() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = proc;
        wc.hInstance = hInst;
        wc.lpszClassName = pms::kOverlayWndClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
    });
}

RECT ExpandAndClampRect(const RECT& r, int expand, int maxW, int maxH) {
    RECT out{};
    out.left   = std::clamp(static_cast<int>(r.left)   - expand, 0, maxW);
    out.top    = std::clamp(static_cast<int>(r.top)    - expand, 0, maxH);
    out.right  = std::clamp(static_cast<int>(r.right)  + expand, 0, maxW);
    out.bottom = std::clamp(static_cast<int>(r.bottom) + expand, 0, maxH);
    return out;
}

} // namespace

namespace pms {

MonitorWorker::MonitorWorker(const DisplayInfo& display, int initialSharpnessPercent)
    : display_(display)
    , sharpnessPercent_(std::clamp(initialSharpnessPercent, kMinSharpnessPercent, kMaxSharpnessPercent))
{
    wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

MonitorWorker::~MonitorWorker() {
    Stop();
    if (wakeEvent_) {
        CloseHandle(wakeEvent_);
        wakeEvent_ = nullptr;
    }
}

bool MonitorWorker::Start() {
    if (workerThread_.joinable()) return true;
    stopRequested_.store(false, std::memory_order_release);
    state_.store(WorkerState::Starting, std::memory_order_release);
    workerThread_ = std::thread(&MonitorWorker::ThreadMain, this);
    return true;
}

void MonitorWorker::Stop() {
    stopRequested_.store(true, std::memory_order_release);
    if (wakeEvent_) SetEvent(wakeEvent_);
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    state_.store(WorkerState::Stopped, std::memory_order_release);
}

void MonitorWorker::SetSharpnessPercent(int percent) {
    int clamped = std::clamp(percent, kMinSharpnessPercent, kMaxSharpnessPercent);
    int prev = sharpnessPercent_.exchange(clamped, std::memory_order_acq_rel);
    if (prev != clamped) {
        lastSliderTouchTick_.store(GetTickCount(), std::memory_order_relaxed);
        paramGeneration_.fetch_add(1, std::memory_order_acq_rel);
        if (wakeEvent_) SetEvent(wakeEvent_);
    }
}

void MonitorWorker::SetPausedForFullscreen(bool paused) {
    bool prev = pausedForFullscreen_.exchange(paused, std::memory_order_acq_rel);
    if (prev != paused) {
        if (wakeEvent_) SetEvent(wakeEvent_);
    }
}

std::wstring MonitorWorker::GetErrorText() const {
    std::lock_guard<std::mutex> lock(errorMutex_);
    return errorText_;
}

void MonitorWorker::SetError(WorkerState st, const std::wstring& msg) {
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        errorText_ = msg;
    }
    state_.store(st, std::memory_order_release);
}

LRESULT CALLBACK MonitorWorker::OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void MonitorWorker::ThreadMain() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    HRESULT hrRo = RoInitialize(RO_INIT_MULTITHREADED);

    if (!IsExcludeFromCaptureSupported()) {
        SetError(WorkerState::Error, L"Requires Windows 10 Version 2004 (Build 19041) or newer.");
        if (SUCCEEDED(hrRo)) RoUninitialize();
        return;
    }

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    RegisterOverlayClassOnce(hInst, &MonitorWorker::OverlayWndProc);

    const int deskW = display_.width;
    const int deskH = display_.height;
    if (deskW <= 0 || deskH <= 0) {
        SetError(WorkerState::Error, L"Invalid display dimensions.");
        if (SUCCEEDED(hrRo)) RoUninitialize();
        return;
    }

    // Create click-through, redirection-free DirectComposition overlay window (initially hidden until frame 0 is ready)
    HWND hwndOverlay = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT |
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kOverlayWndClass,
        L"",
        WS_POPUP,
        display_.desktopBounds.left,
        display_.desktopBounds.top,
        deskW,
        deskH,
        nullptr,
        nullptr,
        hInst,
        nullptr);

    if (!hwndOverlay) {
        SetError(WorkerState::Error, L"Failed to create overlay window.");
        if (SUCCEEDED(hrRo)) RoUninitialize();
        return;
    }

    SetLayeredWindowAttributes(hwndOverlay, 0, 255, LWA_ALPHA);
    if (!SetWindowDisplayAffinity(hwndOverlay, WDA_EXCLUDEFROMCAPTURE)) {
        DestroyWindow(hwndOverlay);
        SetError(WorkerState::Error, L"Windows blocked WDA_EXCLUDEFROMCAPTURE on overlay window.");
        if (SUCCEEDED(hrRo)) RoUninitialize();
        return;
    }

    auto pumpOverlayMessages = [&]() {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    };

    // Locate the physical IDXGIAdapter1 and IDXGIOutput owning this display
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        DestroyWindow(hwndOverlay);
        SetError(WorkerState::Error, L"Failed to initialize DXGI factory.");
        if (SUCCEEDED(hrRo)) RoUninitialize();
        return;
    }

    ComPtr<IDXGIAdapter1> targetAdapter;
    ComPtr<IDXGIOutput> targetOutput;
    for (UINT ai = 0; !targetOutput; ++ai) {
        ComPtr<IDXGIAdapter1> ad;
        if (factory->EnumAdapters1(ai, &ad) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        ad->GetDesc1(&desc);
        if (desc.AdapterLuid.LowPart != display_.adapterLuid.LowPart ||
            desc.AdapterLuid.HighPart != display_.adapterLuid.HighPart) {
            continue;
        }
        for (UINT oi = 0; ; ++oi) {
            ComPtr<IDXGIOutput> out;
            if (ad->EnumOutputs(oi, &out) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC od{};
            if (SUCCEEDED(out->GetDesc(&od)) && display_.gdiDeviceName == od.DeviceName) {
                targetAdapter = ad;
                targetOutput = out;
                break;
            }
        }
    }

    if (!targetAdapter || !targetOutput) {
        DestroyWindow(hwndOverlay);
        SetError(WorkerState::Error, L"Display output is no longer attached.");
        if (SUCCEEDED(hrRo)) RoUninitialize();
        return;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(
        targetAdapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        &fl,
        1,
        D3D11_SDK_VERSION,
        &device,
        nullptr,
        &ctx);

    if (FAILED(hr) || !device || !ctx) {
        DestroyWindow(hwndOverlay);
        SetError(WorkerState::Error, L"Failed to create Direct3D 11 device for display adapter.");
        if (SUCCEEDED(hrRo)) RoUninitialize();
        return;
    }

    // Minimize DXGI frame queue depth to 1 for lowest possible presentation latency
    ComPtr<IDXGIDevice1> dxgiDevice1;
    if (SUCCEEDED(device.As(&dxgiDevice1)) && dxgiDevice1) {
        dxgiDevice1->SetMaximumFrameLatency(1);
    }

    GpuSharpenProcessor processor;
    if (!processor.Initialize(device.Get())) {
        DestroyWindow(hwndOverlay);
        SetError(WorkerState::Error, L"Failed to initialize GPU sharpening shaders.");
        if (SUCCEEDED(hrRo)) RoUninitialize();
        return;
    }

    ComPtr<IDCompositionDesktopDevice> dcompDevice;
    ComPtr<IDCompositionTarget> dcompTarget;
    ComPtr<IDCompositionVisual2> dcompVisual;
    if (FAILED(DCompositionCreateDevice2(dxgiDevice1.Get(), IID_PPV_ARGS(&dcompDevice))) ||
        FAILED(dcompDevice->CreateTargetForHwnd(hwndOverlay, TRUE, &dcompTarget)) ||
        FAILED(dcompDevice->CreateVisual(&dcompVisual)) ||
        FAILED(dcompTarget->SetRoot(dcompVisual.Get()))) {
        DestroyWindow(hwndOverlay);
        SetError(WorkerState::Error, L"Failed to create DirectComposition visual target.");
        if (SUCCEEDED(hrRo)) RoUninitialize();
        return;
    }

    // Dynamic session resources (rebuilt cleanly on format change or after UAC / fullscreen resume)
    ComPtr<IDXGIOutputDuplication> dupl;
    WgcFallbackSession wgcFallback;
    bool usingWgc = false;
    DXGI_MODE_ROTATION duplRotation = display_.rotation;

    ComPtr<IDXGISwapChain1> swapChain;
    DXGI_FORMAT activeSwapFormat = DXGI_FORMAT_UNKNOWN;
    bool isHdrScRgb = false;

    ComPtr<ID3D11Texture2D> cachedSrcTex;
    ComPtr<ID3D11ShaderResourceView> cachedSrcSrv;
    int cachedSrcW = 0, cachedSrcH = 0;
    DXGI_FORMAT cachedSrcFmt = DXGI_FORMAT_UNKNOWN;
    bool hasCachedFrame = false;
    bool overlayVisible = false;
    int fullFramesPresented = 0;

    std::vector<BYTE> metaBuffer(16384);
    std::vector<RECT> expandedDirtyRects;
    expandedDirtyRects.reserve(64);

    auto hideOverlayAndReleaseCapture = [&]() {
        if (overlayVisible) {
            ShowWindow(hwndOverlay, SW_HIDE);
            overlayVisible = false;
        }
        dupl.Reset();
        wgcFallback.Shutdown();
        usingWgc = false;
        hasCachedFrame = false;
        fullFramesPresented = 0;
    };

    auto ensureSwapChainAndCache = [&](int srcW, int srcH, DXGI_FORMAT srcFmt) -> bool {
        bool wantHdr = (srcFmt == DXGI_FORMAT_R16G16B16A16_FLOAT);
        DXGI_FORMAT targetSwapFmt = wantHdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;

        if (!swapChain || activeSwapFormat != targetSwapFmt) {
            swapChain.Reset();

            DXGI_SWAP_CHAIN_DESC1 scd{};
            scd.Width = static_cast<UINT>(deskW);
            scd.Height = static_cast<UINT>(deskH);
            scd.Format = targetSwapFmt;
            scd.SampleDesc.Count = 1;
            scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            scd.BufferCount = 2;
            scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

            if (FAILED(factory->CreateSwapChainForComposition(device.Get(), &scd, nullptr, &swapChain))) {
                return false;
            }

            if (wantHdr) {
                ComPtr<IDXGISwapChain3> sc3;
                if (SUCCEEDED(swapChain.As(&sc3)) && sc3) {
                    sc3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
                }
            }

            dcompVisual->SetContent(swapChain.Get());
            dcompDevice->Commit();

            activeSwapFormat = targetSwapFmt;
            isHdrScRgb = wantHdr;
            fullFramesPresented = 0;
        }

        if (!cachedSrcTex || cachedSrcW != srcW || cachedSrcH != srcH || cachedSrcFmt != srcFmt) {
            cachedSrcSrv.Reset();
            cachedSrcTex.Reset();

            D3D11_TEXTURE2D_DESC td{};
            td.Width = static_cast<UINT>(srcW);
            td.Height = static_cast<UINT>(srcH);
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = srcFmt;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            if (FAILED(device->CreateTexture2D(&td, nullptr, &cachedSrcTex))) return false;
            if (FAILED(device->CreateShaderResourceView(cachedSrcTex.Get(), nullptr, &cachedSrcSrv))) return false;

            cachedSrcW = srcW;
            cachedSrcH = srcH;
            cachedSrcFmt = srcFmt;
            hasCachedFrame = false;
            fullFramesPresented = 0;
        }

        return true;
    };

    auto openCaptureSession = [&]() -> bool {
        dupl.Reset();
        wgcFallback.Shutdown();
        usingWgc = false;

        // Primary path: IDXGIOutput5::DuplicateOutput1 (supports HDR scRGB float, 10-bit, and 8-bit SDR without borders)
        ComPtr<IDXGIOutput5> out5;
        HRESULT hrDup = E_FAIL;
        if (SUCCEEDED(targetOutput.As(&out5)) && out5) {
            const DXGI_FORMAT kFormats[] = {
                DXGI_FORMAT_B8G8R8A8_UNORM,
                DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_R10G10B10A2_UNORM
            };
            hrDup = out5->DuplicateOutput1(device.Get(), 0, static_cast<UINT>(std::size(kFormats)), kFormats, &dupl);
        }
        if (FAILED(hrDup)) {
            ComPtr<IDXGIOutput1> out1;
            if (SUCCEEDED(targetOutput.As(&out1)) && out1) {
                hrDup = out1->DuplicateOutput(device.Get(), &dupl);
            }
        }

        if (SUCCEEDED(hrDup) && dupl) {
            DXGI_OUTDUPL_DESC dd{};
            dupl->GetDesc(&dd);
            duplRotation = (dd.Rotation == DXGI_MODE_ROTATION_UNSPECIFIED)
                           ? DXGI_MODE_ROTATION_IDENTITY
                           : dd.Rotation;
            return true;
        }

        // Fallback path on Windows 11: Borderless Windows.Graphics.Capture (handles hybrid GPU / 4-client DDA limit)
        if ((hrDup == DXGI_ERROR_UNSUPPORTED || hrDup == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE || hrDup == E_INVALIDARG) &&
            IsWindows11OrGreater()) {
            if (wgcFallback.Initialize(device.Get(), display_.hMonitor)) {
                usingWgc = true;
                duplRotation = DXGI_MODE_ROTATION_IDENTITY;
                return true;
            }
        }

        // Transient access denial (UAC prompt, Lock Screen, mode switch) -> retry quietly
        if (hrDup == E_ACCESSDENIED || hrDup == DXGI_ERROR_ACCESS_LOST ||
            hrDup == DXGI_ERROR_SESSION_DISCONNECTED || hrDup == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
            SetError(WorkerState::WaitingForDesktop, L"Waiting for desktop session access...");
        } else {
            wchar_t msg[128];
            wsprintfW(msg, L"Desktop capture unavailable (HRESULT 0x%08X).", static_cast<unsigned>(hrDup));
            SetError(WorkerState::Error, msg);
        }
        return false;
    };

    auto renderFullCachedFrame = [&](uint64_t genToApply) -> bool {
        if (!hasCachedFrame || !swapChain || !cachedSrcSrv) return false;

        ComPtr<ID3D11Texture2D> backBuf;
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf)))) return false;

        ComPtr<ID3D11RenderTargetView> rtv;
        if (FAILED(device->CreateRenderTargetView(backBuf.Get(), nullptr, &rtv))) return false;

        int pct = sharpnessPercent_.load(std::memory_order_acquire);
        float normStrength = SharpnessPercentToNormalized(pct);
        RECT fullRect{ 0, 0, deskW, deskH };

        processor.RenderRegions(
            ctx.Get(),
            cachedSrcSrv.Get(),
            rtv.Get(),
            deskW,
            deskH,
            cachedSrcW,
            cachedSrcH,
            normStrength,
            pct > 0,
            isHdrScRgb,
            duplRotation,
            &fullRect,
            1);

        HRESULT hrP = swapChain->Present(0, 0);
        if (FAILED(hrP)) return false;

        fullFramesPresented = std::min(fullFramesPresented + 1, 10);
        if (!overlayVisible) {
            dcompDevice->Commit();
            ShowWindow(hwndOverlay, SW_SHOWNOACTIVATE);
            SetWindowDisplayAffinity(hwndOverlay, WDA_EXCLUDEFROMCAPTURE);
            overlayVisible = true;
        }
        (void)genToApply;
        return true;
    };

    uint64_t appliedParamGen = 0;
    DWORD lastTopmostRefreshTick = GetTickCount();

    while (!stopRequested_.load(std::memory_order_acquire)) {
        pumpOverlayMessages();

        // 1. Handle Fullscreen Game / App Auto-Pause (0% overhead, 0ms latency, native DirectFlip & VRR)
        if (pausedForFullscreen_.load(std::memory_order_acquire)) {
            if (dupl || usingWgc || overlayVisible) {
                hideOverlayAndReleaseCapture();
            }
            SetError(WorkerState::PausedFullscreen, L"");
            WaitForSingleObject(wakeEvent_, 250);
            continue;
        }

        // 2. Ensure capture session is open
        if (!dupl && !usingWgc) {
            if (!openCaptureSession()) {
                // Sleep quietly without spinning CPU while UAC / Lock Screen / error persists
                WaitForSingleObject(wakeEvent_, 500);
                continue;
            }
            SetError(WorkerState::Active, L"");
        }

        // 3. Periodically assert HWND_TOPMOST without stealing focus so new windows don't slip above overlay
        DWORD nowTick = GetTickCount();
        if (overlayVisible && (nowTick - lastTopmostRefreshTick >= 2500)) {
            SetWindowPos(hwndOverlay, HWND_TOPMOST,
                         display_.desktopBounds.left, display_.desktopBounds.top,
                         deskW, deskH,
                         SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER);
            lastTopmostRefreshTick = nowTick;
        }

        // 4. Instant live slider update on static screen using cached desktop texture!
        uint64_t curGen = paramGeneration_.load(std::memory_order_acquire);
        if (curGen != appliedParamGen && hasCachedFrame) {
            if (renderFullCachedFrame(curGen)) {
                appliedParamGen = curGen;
                // Render a second full frame on next pass if needed so both FLIP_SEQUENTIAL buffers match
                renderFullCachedFrame(curGen);
            }
        }

        // 5A. Windows.Graphics.Capture fallback loop (Windows 11)
        if (usingWgc) {
            ComPtr<ID3D11Texture2D> wgcTex;
            bool sizeChanged = false;
            if (!wgcFallback.TryAcquireTexture(wgcTex, sizeChanged)) {
                if (sizeChanged) {
                    hideOverlayAndReleaseCapture();
                    continue;
                }
                DWORD waitMs = (nowTick - lastSliderTouchTick_.load(std::memory_order_relaxed) < 2000) ? 8 : 16;
                WaitForSingleObject(wakeEvent_, waitMs);
                continue;
            }

            D3D11_TEXTURE2D_DESC td{};
            wgcTex->GetDesc(&td);
            if (!ensureSwapChainAndCache(static_cast<int>(td.Width), static_cast<int>(td.Height), td.Format)) {
                hideOverlayAndReleaseCapture();
                continue;
            }

            if (hasCachedFrame) {
                ComPtr<ID3D11ShaderResourceView> curSrv;
                if (SUCCEEDED(device->CreateShaderResourceView(wgcTex.Get(), nullptr, &curSrv))) {
                    RECT fullBox{ 0, 0, cachedSrcW, cachedSrcH };
                    if (!processor.HasRegionChanged(ctx.Get(), curSrv.Get(), cachedSrcSrv.Get(), fullBox)) {
                        continue; // Duplicate echo frame -> skip Present!
                    }
                }
            }

            ctx->CopyResource(cachedSrcTex.Get(), wgcTex.Get());
            hasCachedFrame = true;
            appliedParamGen = paramGeneration_.load(std::memory_order_acquire);
            if (!renderFullCachedFrame(appliedParamGen)) {
                hideOverlayAndReleaseCapture();
            }
            continue;
        }

        // 5B. Primary Path: Event-driven IDXGIOutputDuplication with Dirty-Rect Partial Shading
        bool recentSliderTouch = (nowTick - lastSliderTouchTick_.load(std::memory_order_relaxed)) < 2000;
        UINT timeoutMs = recentSliderTouch ? 8u : 150u;

        DXGI_OUTDUPL_FRAME_INFO fi{};
        ComPtr<IDXGIResource> deskRes;
        HRESULT hrAcq = dupl->AcquireNextFrame(timeoutMs, &fi, &deskRes);

        if (hrAcq == DXGI_ERROR_WAIT_TIMEOUT) {
            continue; // 0.0% GPU, 0.0% CPU while desktop is static!
        }

        if (hrAcq == DXGI_ERROR_ACCESS_LOST || hrAcq == DXGI_ERROR_INVALID_CALL || hrAcq == E_ACCESSDENIED) {
            hideOverlayAndReleaseCapture();
            SetError(WorkerState::WaitingForDesktop, L"Reconnecting after display state change...");
            WaitForSingleObject(wakeEvent_, 200);
            continue;
        }

        if (FAILED(hrAcq) || !deskRes) {
            hideOverlayAndReleaseCapture();
            WaitForSingleObject(wakeEvent_, 250);
            continue;
        }

        // Mouse-only update (hardware cursor renders natively at 0ms latency above overlay -> zero GPU work!)
        if (fi.LastPresentTime.QuadPart == 0 && hasCachedFrame) {
            dupl->ReleaseFrame();
            continue;
        }

        ComPtr<ID3D11Texture2D> deskTex;
        if (FAILED(deskRes.As(&deskTex)) || !deskTex) {
            dupl->ReleaseFrame();
            continue;
        }

        D3D11_TEXTURE2D_DESC srcDesc{};
        deskTex->GetDesc(&srcDesc);
        int srcW = static_cast<int>(srcDesc.Width);
        int srcH = static_cast<int>(srcDesc.Height);

        if (!ensureSwapChainAndCache(srcW, srcH, srcDesc.Format)) {
            dupl->ReleaseFrame();
            hideOverlayAndReleaseCapture();
            continue;
        }

        // Extract dirty rectangles + move destination rectangles
        expandedDirtyRects.clear();
        bool useFullFrame = (!hasCachedFrame) ||
                            (fullFramesPresented < 2) ||
                            (duplRotation != DXGI_MODE_ROTATION_IDENTITY) ||
                            (fi.TotalMetadataBufferSize == 0);

        RECT dirtyUnion{ srcW, srcH, 0, 0 };
        int64_t dirtyAreaSum = 0;

        if (!useFullFrame && fi.TotalMetadataBufferSize > 0) {
            if (metaBuffer.size() < fi.TotalMetadataBufferSize) {
                metaBuffer.resize(fi.TotalMetadataBufferSize);
            }

            UINT moveBytes = static_cast<UINT>(metaBuffer.size());
            HRESULT hrMv = dupl->GetFrameMoveRects(
                moveBytes,
                reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(metaBuffer.data()),
                &moveBytes);
            if (SUCCEEDED(hrMv) && moveBytes > 0) {
                size_t moveCount = moveBytes / sizeof(DXGI_OUTDUPL_MOVE_RECT);
                const auto* moves = reinterpret_cast<const DXGI_OUTDUPL_MOVE_RECT*>(metaBuffer.data());
                for (size_t i = 0; i < moveCount; ++i) {
                    RECT er = ExpandAndClampRect(moves[i].DestinationRect, 1, srcW, srcH);
                    if (er.right > er.left && er.bottom > er.top) {
                        expandedDirtyRects.push_back(er);
                        dirtyUnion.left   = std::min(dirtyUnion.left, er.left);
                        dirtyUnion.top    = std::min(dirtyUnion.top, er.top);
                        dirtyUnion.right  = std::max(dirtyUnion.right, er.right);
                        dirtyUnion.bottom = std::max(dirtyUnion.bottom, er.bottom);
                        dirtyAreaSum += static_cast<int64_t>(er.right - er.left) * (er.bottom - er.top);
                    }
                }
            }

            UINT dirtyBytes = static_cast<UINT>(metaBuffer.size());
            HRESULT hrDr = dupl->GetFrameDirtyRects(
                dirtyBytes,
                reinterpret_cast<RECT*>(metaBuffer.data()),
                &dirtyBytes);
            if (SUCCEEDED(hrDr) && dirtyBytes > 0) {
                size_t rectCount = dirtyBytes / sizeof(RECT);
                const auto* rects = reinterpret_cast<const RECT*>(metaBuffer.data());
                for (size_t i = 0; i < rectCount; ++i) {
                    RECT er = ExpandAndClampRect(rects[i], 1, srcW, srcH);
                    if (er.right > er.left && er.bottom > er.top) {
                        expandedDirtyRects.push_back(er);
                        dirtyUnion.left   = std::min(dirtyUnion.left, er.left);
                        dirtyUnion.top    = std::min(dirtyUnion.top, er.top);
                        dirtyUnion.right  = std::max(dirtyUnion.right, er.right);
                        dirtyUnion.bottom = std::max(dirtyUnion.bottom, er.bottom);
                        dirtyAreaSum += static_cast<int64_t>(er.right - er.left) * (er.bottom - er.top);
                    }
                }
            } else if (FAILED(hrDr)) {
                useFullFrame = true;
            }

            const int64_t totalScreenArea = static_cast<int64_t>(srcW) * srcH;
            if (expandedDirtyRects.empty() ||
                expandedDirtyRects.size() > 24 ||
                dirtyAreaSum > (totalScreenArea * 45) / 100) {
                useFullFrame = true;
            }
        }

        // Circuit-breaker diff check against cachedSrcTex to guarantee 0-FPS idle (stops any Win11 24H2 DWM echo)
        if (hasCachedFrame) {
            ComPtr<ID3D11ShaderResourceView> curDeskSrv;
            if (SUCCEEDED(device->CreateShaderResourceView(deskTex.Get(), nullptr, &curDeskSrv))) {
                RECT checkBox = useFullFrame ? RECT{ 0, 0, srcW, srcH } : dirtyUnion;
                if (checkBox.right > checkBox.left && checkBox.bottom > checkBox.top) {
                    if (!processor.HasRegionChanged(ctx.Get(), curDeskSrv.Get(), cachedSrcSrv.Get(), checkBox)) {
                        dupl->ReleaseFrame();
                        continue; // Zero underlying pixels changed -> skip shader & Present1!
                    }
                }
            }
        }

        if (useFullFrame) {
            ctx->CopyResource(cachedSrcTex.Get(), deskTex.Get());
            dupl->ReleaseFrame();

            hasCachedFrame = true;
            appliedParamGen = paramGeneration_.load(std::memory_order_acquire);
            if (!renderFullCachedFrame(appliedParamGen)) {
                hideOverlayAndReleaseCapture();
            }
        } else {
            // Copy only the expanded dirty rectangles into cachedSrcTex
            for (const RECT& r : expandedDirtyRects) {
                D3D11_BOX box{};
                box.left   = static_cast<UINT>(r.left);
                box.top    = static_cast<UINT>(r.top);
                box.front  = 0;
                box.right  = static_cast<UINT>(r.right);
                box.bottom = static_cast<UINT>(r.bottom);
                box.back   = 1;
                ctx->CopySubresourceRegion(
                    cachedSrcTex.Get(), 0,
                    box.left, box.top, 0,
                    deskTex.Get(), 0,
                    &box);
            }
            dupl->ReleaseFrame();
            hasCachedFrame = true;

            ComPtr<ID3D11Texture2D> backBuf;
            ComPtr<ID3D11RenderTargetView> rtv;
            if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf))) ||
                FAILED(device->CreateRenderTargetView(backBuf.Get(), nullptr, &rtv))) {
                hideOverlayAndReleaseCapture();
                continue;
            }

            int pct = sharpnessPercent_.load(std::memory_order_acquire);
            appliedParamGen = paramGeneration_.load(std::memory_order_acquire);

            processor.RenderRegions(
                ctx.Get(),
                cachedSrcSrv.Get(),
                rtv.Get(),
                deskW,
                deskH,
                cachedSrcW,
                cachedSrcH,
                SharpnessPercentToNormalized(pct),
                pct > 0,
                isHdrScRgb,
                duplRotation,
                expandedDirtyRects.data(),
                expandedDirtyRects.size());

            DXGI_PRESENT_PARAMETERS pp{};
            pp.DirtyRectsCount = static_cast<UINT>(expandedDirtyRects.size());
            pp.pDirtyRects = expandedDirtyRects.data();
            HRESULT hrP = swapChain->Present1(0, 0, &pp);
            if (FAILED(hrP)) {
                // Fallback to full-frame present if driver rejected dirty rect batch
                fullFramesPresented = 0;
                if (!renderFullCachedFrame(appliedParamGen)) {
                    hideOverlayAndReleaseCapture();
                }
            }
        }
    }

    hideOverlayAndReleaseCapture();
    swapChain.Reset();
    dcompVisual.Reset();
    dcompTarget.Reset();
    dcompDevice.Reset();
    processor.Reset();
    ctx.Reset();
    device.Reset();
    DestroyWindow(hwndOverlay);

    if (SUCCEEDED(hrRo)) {
        RoUninitialize();
    }
}

} // namespace pms
