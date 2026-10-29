#pragma once

#include <cstdint>
#include <algorithm>

namespace pms {

// Must match cbuffer SharpenCB in shaders/luma_cas.hlsl (32 bytes, multiple of 16 bytes)
struct SharpenConstantBuffer {
    float    strength;    // [0.0f .. 1.0f]
    uint32_t flags;       // Bit 0: enabled, Bit 1: HDR scRGB linear float
    int32_t  srcWidth;
    int32_t  srcHeight;
    int32_t  rotation;    // DXGI_MODE_ROTATION (1=Identity, 2=90, 3=180, 4=270)
    int32_t  pad0;
    int32_t  pad1;
    int32_t  pad2;
};
static_assert(sizeof(SharpenConstantBuffer) == 32, "SharpenConstantBuffer must be 32 bytes");

// Must match cbuffer DiffCB in shaders/diff_check.hlsl (16 bytes)
struct DiffConstantBuffer {
    int32_t offsetX;
    int32_t offsetY;
    int32_t width;
    int32_t height;
};
static_assert(sizeof(DiffConstantBuffer) == 16, "DiffConstantBuffer must be 16 bytes");

inline float SharpnessPercentToNormalized(int percent) {
    int clamped = std::clamp(percent, 0, 100);
    return static_cast<float>(clamped) / 100.0f;
}

} // namespace pms
