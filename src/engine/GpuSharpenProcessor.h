#pragma once

#include "engine/SharpenShaderParams.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstddef>

namespace pms {

class GpuSharpenProcessor {
public:
    GpuSharpenProcessor() = default;
    ~GpuSharpenProcessor() = default;

    bool Initialize(ID3D11Device* device);
    void Reset();

    // Renders the 9-tap Desktop Luma-CAS shader from srcSrv into dstRtv across one or more scissor rectangles.
    void RenderRegions(
        ID3D11DeviceContext* ctx,
        ID3D11ShaderResourceView* srcSrv,
        ID3D11RenderTargetView* dstRtv,
        int dstWidth,
        int dstHeight,
        int srcWidth,
        int srcHeight,
        float normalizedStrength,
        bool enabled,
        bool isHdrScRgb,
        DXGI_MODE_ROTATION rotation,
        const RECT* scissorRects,
        size_t scissorCount);

    // Executes the early-exit DirectCompute diff check over `region` between curSrv and prevSrv.
    // Returns true if at least one pixel RGB differs inside `region`.
    bool HasRegionChanged(
        ID3D11DeviceContext* ctx,
        ID3D11ShaderResourceView* curSrv,
        ID3D11ShaderResourceView* prevSrv,
        const RECT& region);

private:
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> diffCs_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> sharpenCb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> diffCb_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> scissorRs_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> diffGpuBuf_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> diffUav_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> diffStagingBuf_;

    bool hasValidCb_ = false;
    SharpenConstantBuffer cachedCb_{};
};

} // namespace pms
