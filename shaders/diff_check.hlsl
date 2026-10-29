// PerMonSharp - Early-Exit DirectCompute Dirty-Region Pixel-Diff Check
// Prevents duplicate DWM wakeup frames (e.g. Win11 24H2 WDA_EXCLUDEFROMCAPTURE echo) from causing
// redundant shading or Present1 feedback loops, ensuring true 0 FPS idle operation.

cbuffer DiffCB : register(b0)
{
    int2 regionOffset;
    int2 regionSize;
};

Texture2D<float4> curTex  : register(t0);
Texture2D<float4> prevTex : register(t1);
RWByteAddressBuffer diffFlag : register(u0);

[numthreads(16, 16, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= (uint)regionSize.x || dtid.y >= (uint)regionSize.y)
        return;

    // Fast early exit if any thread group has already detected a changed pixel
    if (diffFlag.Load(0) != 0u)
        return;

    int3 p = int3(regionOffset + int2(dtid.xy), 0);
    float3 a = curTex.Load(p).rgb;
    float3 b = prevTex.Load(p).rgb;

    if (any(a != b))
    {
        uint prev;
        diffFlag.InterlockedOr(0, 1u, prev);
    }
}
