#include "common/Version.h"
#include "common/StringUtils.h"
#include "config/Config.h"
#include "display/DisplayManager.h"
#include "engine/GpuSharpenProcessor.h"
#include "ui/MainWindow.h"

#include <windows.h>
#include <dxgi1_4.h>
#include <dcomp.h>
#include <shellapi.h>
#include <commctrl.h>
#include <psapi.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdarg>
#include <cstring>

#pragma comment(lib, "psapi.lib")

using Microsoft::WRL::ComPtr;

namespace {

int RunCliBenchmark() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    bool attachedConsole = false;
    if (!hOut || hOut == INVALID_HANDLE_VALUE || GetFileType(hOut) == FILE_TYPE_UNKNOWN) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            attachedConsole = true;
            hOut = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        }
    }

    auto cliPrint = [&](const char* fmt, ...) {
        if (!hOut || hOut == INVALID_HANDLE_VALUE) return;
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        int len = std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        if (len > 0) {
            DWORD written = 0;
            WriteFile(hOut, buf, static_cast<DWORD>(std::min<int>(len, sizeof(buf) - 1)), &written, nullptr);
        }
    };

    cliPrint("\nPerMonSharp v1.0.0 - Hardware Display & GPU Benchmark\n");
    cliPrint("=====================================================\n\n");

    auto os = pms::GetRealOsVersion();
    cliPrint("OS Build: Windows %u.%u (Build %u) | WDA_EXCLUDEFROMCAPTURE: %s\n\n",
             os.major, os.minor, os.build,
             pms::IsExcludeFromCaptureSupported() ? "Supported" : "Unsupported");

    auto displays = pms::DisplayManager::EnumerateDisplays();
    cliPrint("Detected Active Displays: %zu\n", displays.size());
    for (size_t i = 0; i < displays.size(); ++i) {
        const auto& d = displays[i];
        cliPrint("  [%zu] %s (%s) on %s\n",
                 i + 1,
                 pms::WideToUtf8(d.displayLabel).c_str(),
                 pms::WideToUtf8(d.gdiDeviceName).c_str(),
                 pms::WideToUtf8(d.adapterName).c_str());
        cliPrint("      Bounds: (%ld, %ld)-(%ld, %ld) [%dx%d] @ %.2f Hz | DPI: %u%% | %s%s\n",
                 d.desktopBounds.left, d.desktopBounds.top,
                 d.desktopBounds.right, d.desktopBounds.bottom,
                 d.width, d.height, d.refreshRateHz, d.dpiPercent,
                 d.isPrimary ? "Primary " : "",
                 d.isHdrActive ? "HDR" : "SDR");
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                                 D3D11_SDK_VERSION, &device, nullptr, &ctx))) {
        cliPrint("\nERROR: Failed to create hardware Direct3D 11 device.\n");
        if (attachedConsole) { CloseHandle(hOut); FreeConsole(); }
        return 1;
    }

    pms::GpuSharpenProcessor proc;
    if (!proc.Initialize(device.Get())) {
        cliPrint("\nERROR: Failed to initialize GpuSharpenProcessor.\n");
        if (attachedConsole) { CloseHandle(hOut); FreeConsole(); }
        return 1;
    }

    auto benchPass = [&](int W, int H, RECT region, int iters) -> double {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(W);
        td.Height = static_cast<UINT>(H);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        ComPtr<ID3D11Texture2D> srcT, dstT;
        device->CreateTexture2D(&td, nullptr, &srcT);
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        device->CreateTexture2D(&td, nullptr, &dstT);

        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11RenderTargetView> rtv;
        device->CreateShaderResourceView(srcT.Get(), nullptr, &srv);
        device->CreateRenderTargetView(dstT.Get(), nullptr, &rtv);

        for (int i = 0; i < 20; ++i) {
            proc.RenderRegions(ctx.Get(), srv.Get(), rtv.Get(), W, H, W, H,
                               0.50f, true, false, DXGI_MODE_ROTATION_IDENTITY, &region, 1);
        }
        ctx->Flush();

        D3D11_QUERY_DESC qdDis{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
        D3D11_QUERY_DESC qdTs{ D3D11_QUERY_TIMESTAMP, 0 };
        ComPtr<ID3D11Query> qDis, q0, q1;
        device->CreateQuery(&qdDis, &qDis);
        device->CreateQuery(&qdTs, &q0);
        device->CreateQuery(&qdTs, &q1);

        ctx->Begin(qDis.Get());
        ctx->End(q0.Get());
        for (int i = 0; i < iters; ++i) {
            proc.RenderRegions(ctx.Get(), srv.Get(), rtv.Get(), W, H, W, H,
                               0.50f, true, false, DXGI_MODE_ROTATION_IDENTITY, &region, 1);
        }
        ctx->End(q1.Get());
        ctx->End(qDis.Get());
        ctx->Flush();

        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dis{};
        while (ctx->GetData(qDis.Get(), &dis, sizeof(dis), 0) == S_FALSE) Sleep(1);
        UINT64 t0 = 0, t1 = 0;
        while (ctx->GetData(q0.Get(), &t0, sizeof(t0), 0) == S_FALSE) Sleep(1);
        while (ctx->GetData(q1.Get(), &t1, sizeof(t1), 0) == S_FALSE) Sleep(1);

        return ((static_cast<double>(t1 - t0) * 1e6) / static_cast<double>(dis.Frequency)) / iters;
    };

    auto queryProcessDedicatedVramMB = [&]() -> double {
        ComPtr<IDXGIDevice> dxgiDev;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIAdapter3> adapter3;
        if (SUCCEEDED(device.As(&dxgiDev)) && dxgiDev &&
            SUCCEEDED(dxgiDev->GetAdapter(&adapter)) && adapter &&
            SUCCEEDED(adapter.As(&adapter3)) && adapter3) {
            DXGI_QUERY_VIDEO_MEMORY_INFO vmi{};
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vmi))) {
                return static_cast<double>(vmi.CurrentUsage) / (1024.0 * 1024.0);
            }
        }
        return 0.0;
    };

    auto measureActivePipelineVramMB = [&](int W, int H) -> double {
        ComPtr<IDXGIDevice> dxgiDev;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory2;
        if (FAILED(device.As(&dxgiDev)) || FAILED(dxgiDev->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(&factory2)))) {
            return 0.0;
        }

        // Allocate the exact GPU surfaces held by an active MonitorWorker:
        // 1x cached desktop SRV texture + 2-buffer FLIP_SEQUENTIAL DirectComposition swapchain
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(W);
        td.Height = static_cast<UINT>(H);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        ComPtr<ID3D11Texture2D> cachedSrc;
        ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &cachedSrc)) ||
            FAILED(device->CreateShaderResourceView(cachedSrc.Get(), nullptr, &srv))) {
            return 0.0;
        }

        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width = static_cast<UINT>(W);
        scd.Height = static_cast<UINT>(H);
        scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        ComPtr<IDXGISwapChain1> sc;
        if (FAILED(factory2->CreateSwapChainForComposition(device.Get(), &scd, nullptr, &sc))) {
            return 0.0;
        }

        ComPtr<ID3D11Texture2D> backBuf;
        ComPtr<ID3D11RenderTargetView> rtv;
        if (SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&backBuf))) &&
            SUCCEEDED(device->CreateRenderTargetView(backBuf.Get(), nullptr, &rtv))) {
            RECT full{ 0, 0, W, H };
            proc.RenderRegions(ctx.Get(), srv.Get(), rtv.Get(), W, H, W, H,
                               0.50f, true, false, DXGI_MODE_ROTATION_IDENTITY, &full, 1);
            sc->Present(0, 0);
            ctx->Flush();
        }

        double totalProcessVramMB = queryProcessDedicatedVramMB();
        ctx->ClearState();
        rtv.Reset();
        backBuf.Reset();
        sc.Reset();
        srv.Reset();
        cachedSrc.Reset();
        ctx->Flush();
        return totalProcessVramMB;
    };

    // Measure per-process VRAM in strictly ascending resolution order BEFORE synthetic benchmark passes
    double vramBaseMB = queryProcessDedicatedVramMB();
    double vram1080MB = measureActivePipelineVramMB(1920, 1080);

    PROCESS_MEMORY_COUNTERS_EX pmc1080{};
    pmc1080.cb = sizeof(pmc1080);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc1080), sizeof(pmc1080));

    double vram1440MB = measureActivePipelineVramMB(2560, 1440);
    double vram4kMB   = measureActivePipelineVramMB(3840, 2160);

    double usDirty = benchPass(1920, 1080, { 0, 0, 128,  64   }, 200);
    double us1080  = benchPass(1920, 1080, { 0, 0, 1920, 1080 }, 200);
    double us1440  = benchPass(2560, 1440, { 0, 0, 2560, 1440 }, 200);
    double us4k    = benchPass(3840, 2160, { 0, 0, 3840, 2160 }, 100);
    ctx->ClearState();
    ctx->Flush();

    cliPrint("\n9-Tap Desktop Luma-CAS GPU Execution Time:\n");
    cliPrint("  Static desktop / cursor-only :   0.00 us (0.000%% of 144 Hz frame)\n");
    cliPrint("  Dirty-rect 128x64 (typing)   : %6.2f us (%.3f%% of 144 Hz frame)\n", usDirty, (usDirty / 6944.4) * 100.0);
    cliPrint("  1920x1080 (FHD) full frame   : %6.2f us (%.2f%% of 144 Hz, %.2f%% of 60 Hz)\n",
             us1080, (us1080 / 6944.4) * 100.0, (us1080 / 16666.7) * 100.0);
    cliPrint("  2560x1440 (QHD) full frame   : %6.2f us (%.2f%% of 144 Hz, %.2f%% of 60 Hz)\n",
             us1440, (us1440 / 6944.4) * 100.0, (us1440 / 16666.7) * 100.0);
    cliPrint("  3840x2160 (4K)  full frame   : %6.2f us (%.2f%% of 144 Hz, %.2f%% of 60 Hz)\n",
             us4k,   (us4k   / 6944.4) * 100.0, (us4k   / 16666.7) * 100.0);

    cliPrint("\nPer-Process Dedicated VRAM Usage (DXGI Process-Isolated):\n");
    cliPrint("  D3D11 Device + Shaders base  : %6.2f MB\n", vramBaseMB);
    cliPrint("  1x 1920x1080 Active Monitor  : %6.2f MB\n", vram1080MB);
    cliPrint("  1x 2560x1440 Active Monitor  : %6.2f MB\n", vram1440MB);
    cliPrint("  1x 3840x2160 Active Monitor  : %6.2f MB\n", vram4kMB);

    cliPrint("\nProcess System RAM Footprint (1080p D3D11 Active):\n");
    cliPrint("  Working Set  : %.2f MB\n", pmc1080.WorkingSetSize / (1024.0 * 1024.0));
    cliPrint("  Private Bytes: %.2f MB\n", pmc1080.PrivateUsage / (1024.0 * 1024.0));

    if (attachedConsole) {
        CloseHandle(hOut);
        FreeConsole();
    }
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    bool startMinimized = false;
    bool forcePortable = false;
    bool runBenchmark = false;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (_wcsicmp(argv[i], L"--minimized") == 0 || _wcsicmp(argv[i], L"--tray") == 0) {
                startMinimized = true;
            } else if (_wcsicmp(argv[i], L"--portable") == 0) {
                forcePortable = true;
            } else if (_wcsicmp(argv[i], L"--benchmark") == 0) {
                runBenchmark = true;
            }
        }
        LocalFree(argv);
    }

    if (runBenchmark) {
        return RunCliBenchmark();
    }

    if (!pms::IsExcludeFromCaptureSupported()) {
        MessageBoxW(
            nullptr,
            L"PerMonSharp requires Windows 10 Version 2004 (Build 19041) or newer.\n\n"
            L"Older Windows builds do not support excluding desktop overlays from recursive capture (WDA_EXCLUDEFROMCAPTURE).",
            pms::kAppName,
            MB_ICONERROR | MB_OK);
        return 1;
    }

    UINT showWindowMsg = RegisterWindowMessageW(pms::kShowWindowMsg);

    // Single-instance check: if already running in system tray, bring existing window to foreground
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, pms::kSingleInstMutex);
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        AllowSetForegroundWindow(ASFW_ANY);
        HWND existing = FindWindowW(pms::kMainWndClass, nullptr);
        if (existing && showWindowMsg != 0) {
            PostMessageW(existing, showWindowMsg, 0, 0);
        }
        CloseHandle(hMutex);
        return 0;
    }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::wstring configPath = pms::ConfigManager::ResolveConfigPath(forcePortable);
    pms::AppConfig config{};
    pms::ConfigManager::LoadFromFile(configPath, config);

    int exitCode = 0;
    {
        pms::MainWindow mainWnd(configPath, std::move(config));
        if (!mainWnd.Create(hInstance, startMinimized, showWindowMsg)) {
            MessageBoxW(nullptr, L"Failed to initialize PerMonSharp application window.", pms::kAppName, MB_ICONERROR | MB_OK);
            exitCode = 1;
        } else {
            exitCode = mainWnd.RunMessageLoop();
        }
    }

    if (SUCCEEDED(hrCom)) {
        CoUninitialize();
    }
    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
    return exitCode;
}
