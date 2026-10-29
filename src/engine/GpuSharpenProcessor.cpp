#include "engine/GpuSharpenProcessor.h"

// Precompiled DXBC shader bytecode generated at build time by fxc.exe
#include "luma_cas_vs.h"
#include "luma_cas_ps.h"
#include "diff_check_cs.h"

namespace pms {

bool GpuSharpenProcessor::Initialize(ID3D11Device* device) {
    if (!device) return false;
    Reset();

    if (FAILED(device->CreateVertexShader(g_LumaCasVS, sizeof(g_LumaCasVS), nullptr, &vs_)))
        return false;
    if (FAILED(device->CreatePixelShader(g_LumaCasPS, sizeof(g_LumaCasPS), nullptr, &ps_)))
        return false;
    if (FAILED(device->CreateComputeShader(g_DiffCheckCS, sizeof(g_DiffCheckCS), nullptr, &diffCs_)))
        return false;

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(SharpenConstantBuffer);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cbd, nullptr, &sharpenCb_)))
        return false;

    cbd.ByteWidth = sizeof(DiffConstantBuffer);
    if (FAILED(device->CreateBuffer(&cbd, nullptr, &diffCb_)))
        return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rd, &scissorRs_)))
        return false;

    // 4-byte raw UAV buffer + 4-byte staging readback buffer for DiffCheck CS
    D3D11_BUFFER_DESC ubd{};
    ubd.ByteWidth = 4;
    ubd.Usage = D3D11_USAGE_DEFAULT;
    ubd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ubd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(device->CreateBuffer(&ubd, nullptr, &diffGpuBuf_)))
        return false;

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavd{};
    uavd.Format = DXGI_FORMAT_R32_TYPELESS;
    uavd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavd.Buffer.FirstElement = 0;
    uavd.Buffer.NumElements = 1;
    uavd.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(device->CreateUnorderedAccessView(diffGpuBuf_.Get(), &uavd, &diffUav_)))
        return false;

    D3D11_BUFFER_DESC sbd{};
    sbd.ByteWidth = 4;
    sbd.Usage = D3D11_USAGE_STAGING;
    sbd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateBuffer(&sbd, nullptr, &diffStagingBuf_)))
        return false;

    return true;
}

void GpuSharpenProcessor::Reset() {
    hasValidCb_ = false;
    diffStagingBuf_.Reset();
    diffUav_.Reset();
    diffGpuBuf_.Reset();
    scissorRs_.Reset();
    diffCb_.Reset();
    sharpenCb_.Reset();
    diffCs_.Reset();
    ps_.Reset();
    vs_.Reset();
}

void GpuSharpenProcessor::RenderRegions(
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
    size_t scissorCount)
{
    if (!ctx || !srcSrv || !dstRtv || dstWidth <= 0 || dstHeight <= 0 || scissorCount == 0) {
        return;
    }

    uint32_t newFlags = (enabled ? 1u : 0u) | (isHdrScRgb ? 2u : 0u);
    int32_t newRot = static_cast<int32_t>(rotation);

    if (!hasValidCb_ ||
        cachedCb_.strength != normalizedStrength ||
        cachedCb_.flags != newFlags ||
        cachedCb_.srcWidth != srcWidth ||
        cachedCb_.srcHeight != srcHeight ||
        cachedCb_.rotation != newRot)
    {
        D3D11_MAPPED_SUBRESOURCE ms{};
        if (SUCCEEDED(ctx->Map(sharpenCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            cachedCb_.strength = normalizedStrength;
            cachedCb_.flags = newFlags;
            cachedCb_.srcWidth = srcWidth;
            cachedCb_.srcHeight = srcHeight;
            cachedCb_.rotation = newRot;
            cachedCb_.pad0 = cachedCb_.pad1 = cachedCb_.pad2 = 0;
            *static_cast<SharpenConstantBuffer*>(ms.pData) = cachedCb_;
            ctx->Unmap(sharpenCb_.Get(), 0);
            hasValidCb_ = true;
        }
    }

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = static_cast<float>(dstWidth);
    vp.Height = static_cast<float>(dstHeight);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(scissorRs_.Get());
    ctx->OMSetRenderTargets(1, &dstRtv, nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);

    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(ps_.Get(), nullptr, 0);

    ID3D11Buffer* cbs[] = { sharpenCb_.Get() };
    ctx->PSSetConstantBuffers(0, 1, cbs);
    ctx->PSSetShaderResources(0, 1, &srcSrv);

    for (size_t i = 0; i < scissorCount; ++i) {
        RECT r = scissorRects[i];
        if (r.right > r.left && r.bottom > r.top) {
            ctx->RSSetScissorRects(1, &r);
            ctx->Draw(3, 0);
        }
    }

    // Unbind SRV and RTV cleanly to avoid resource hazard warnings on subsequent copy/present
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);
    ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
}

bool GpuSharpenProcessor::HasRegionChanged(
    ID3D11DeviceContext* ctx,
    ID3D11ShaderResourceView* curSrv,
    ID3D11ShaderResourceView* prevSrv,
    const RECT& region)
{
    int w = region.right - region.left;
    int h = region.bottom - region.top;
    if (!ctx || !curSrv || !prevSrv || w <= 0 || h <= 0) {
        return false;
    }

    const UINT zero[4] = { 0, 0, 0, 0 };
    ctx->ClearUnorderedAccessViewUint(diffUav_.Get(), zero);

    D3D11_MAPPED_SUBRESOURCE ms{};
    if (SUCCEEDED(ctx->Map(diffCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        auto* cb = static_cast<DiffConstantBuffer*>(ms.pData);
        cb->offsetX = region.left;
        cb->offsetY = region.top;
        cb->width = w;
        cb->height = h;
        ctx->Unmap(diffCb_.Get(), 0);
    }

    ID3D11Buffer* cbs[] = { diffCb_.Get() };
    ID3D11ShaderResourceView* srvs[] = { curSrv, prevSrv };
    ID3D11UnorderedAccessView* uavs[] = { diffUav_.Get() };

    ctx->CSSetShader(diffCs_.Get(), nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, cbs);
    ctx->CSSetShaderResources(0, 2, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

    UINT groupsX = static_cast<UINT>((w + 15) / 16);
    UINT groupsY = static_cast<UINT>((h + 15) / 16);
    ctx->Dispatch(groupsX, groupsY, 1);

    ID3D11ShaderResourceView* nullSrvs[2] = { nullptr, nullptr };
    ID3D11UnorderedAccessView* nullUavs[1] = { nullptr };
    ctx->CSSetShaderResources(0, 2, nullSrvs);
    ctx->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);

    ctx->CopyResource(diffStagingBuf_.Get(), diffGpuBuf_.Get());
    if (SUCCEEDED(ctx->Map(diffStagingBuf_.Get(), 0, D3D11_MAP_READ, 0, &ms))) {
        uint32_t flag = *static_cast<const uint32_t*>(ms.pData);
        ctx->Unmap(diffStagingBuf_.Get(), 0);
        return flag != 0u;
    }

    return true;
}

} // namespace pms
