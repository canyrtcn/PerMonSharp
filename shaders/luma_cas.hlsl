// PerMonSharp - 9-Tap Isotropic Luminance-Preserving Contrast-Adaptive Sharpening (Luma-CAS)
// Engineered specifically for Windows desktop content (ClearType & grayscale text, UI, browsers, media).
// Supports SDR (8-bit / 10-bit UNORM), HDR (16-bit float linear scRGB), and 0/90/180/270 display rotation.

cbuffer SharpenCB : register(b0)
{
    float strength;      // Normalized sharpness [0.0 .. 1.0]
    uint  flags;         // Bit 0: enabled, Bit 1: HDR linear scRGB mode
    int   srcWidth;      // Unrotated source texture width
    int   srcHeight;     // Unrotated source texture height
    int   rotation;      // DXGI_MODE_ROTATION: 1=Identity, 2=Rotate90, 3=Rotate180, 4=Rotate270
    int3  reserved;
};

Texture2D<float4> srcTex : register(t0);

struct VSOut
{
    float4 pos : SV_Position;
};

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float PerceptualLuma(float3 c, bool isHdr)
{
    float y = dot(max(c, 0.0), float3(0.2126, 0.7152, 0.0722));
    // In linear scRGB HDR, evaluate contrast in sqrt (~gamma 2.0) perceptual domain
    return isHdr ? sqrt(y) : y;
}

int2 MapRotatedCoord(int2 dstPos)
{
    if (rotation == 2) // DXGI_MODE_ROTATION_ROTATE90
    {
        return int2(dstPos.y, (srcHeight - 1) - dstPos.x);
    }
    else if (rotation == 3) // DXGI_MODE_ROTATION_ROTATE180
    {
        return int2((srcWidth - 1) - dstPos.x, (srcHeight - 1) - dstPos.y);
    }
    else if (rotation == 4) // DXGI_MODE_ROTATION_ROTATE270
    {
        return int2((srcWidth - 1) - dstPos.y, dstPos.x);
    }
    return dstPos; // Identity
}

float3 LoadClamped(int2 coord, int2 maxCoord)
{
    int2 c = clamp(coord, int2(0, 0), maxCoord);
    return srcTex.Load(int3(c, 0)).rgb;
}

// 2nd-order smooth knee: preserves 100% of fine linear micro-detail when |x| << limit,
// while smoothly approaching 'limit' without hard-clipping artifacts or sluggish 1st-order attenuation.
float SmoothLimit(float x, float limit)
{
    return (x * limit) * rsqrt(x * x + limit * limit);
}

float4 PSMain(VSOut i) : SV_Target
{
    int2 dstPos = int2(i.pos.xy);
    int2 maxCoord = int2(max(srcWidth - 1, 0), max(srcHeight - 1, 0));
    int2 p = MapRotatedCoord(dstPos);

    float3 e = LoadClamped(p, maxCoord);
    bool isEnabled = (flags & 1u) != 0u;
    if (!isEnabled || strength <= 1e-4)
    {
        return float4(e, 1.0);
    }

    bool isHdr = (flags & 2u) != 0u;

    // 3x3 isotropic neighborhood fetch (clamped at monitor boundaries to prevent false dark edges)
    float3 nw = LoadClamped(p + int2(-1, -1), maxCoord);
    float3 n  = LoadClamped(p + int2( 0, -1), maxCoord);
    float3 ne = LoadClamped(p + int2( 1, -1), maxCoord);
    float3 w  = LoadClamped(p + int2(-1,  0), maxCoord);
    float3 ea = LoadClamped(p + int2( 1,  0), maxCoord);
    float3 sw = LoadClamped(p + int2(-1,  1), maxCoord);
    float3 s  = LoadClamped(p + int2( 0,  1), maxCoord);
    float3 se = LoadClamped(p + int2( 1,  1), maxCoord);

    float yNW = PerceptualLuma(nw, isHdr);
    float yN  = PerceptualLuma(n,  isHdr);
    float yNE = PerceptualLuma(ne, isHdr);
    float yW  = PerceptualLuma(w,  isHdr);
    float yE  = PerceptualLuma(e,  isHdr);
    float yEA = PerceptualLuma(ea, isHdr);
    float ySW = PerceptualLuma(sw, isHdr);
    float yS  = PerceptualLuma(s,  isHdr);
    float ySE = PerceptualLuma(se, isHdr);

    // Isotropic circular stencil: cross weight 1.0, diagonal weight 0.5 (sum = 6.0)
    float yBlur = ((yN + yS + yW + yEA) + 0.5 * (yNW + yNE + ySW + ySE)) * (1.0 / 6.0);
    float detail = yE - yBlur;

    // Local 3x3 neighborhood extrema envelope (strictly prevents halo/overshoot outside edges)
    float yMinCross = min(yE, min(min(yN, yS), min(yW, yEA)));
    float yMaxCross = max(yE, max(max(yN, yS), max(yW, yEA)));
    float yMinDiag  = min(min(yNW, yNE), min(ySW, ySE));
    float yMaxDiag  = max(max(yNW, yNE), max(ySW, ySE));
    float yMin = min(yMinCross, yMinDiag);
    float yMax = max(yMaxCross, yMaxDiag);

    // Micro-dither / 1-LSB banding noise gate: suppress <= 1/255 flat noise while keeping >= 3.5/255 edges active
    float localRange = yMax - yMin;
    float noiseGate = smoothstep(0.0038, 0.0135, localRange);

    // Adaptive anti-aliasing cushion (0.82 at low settings for smooth reading -> 0.96 at 100% for crisp edge definition)
    float aaHeadroom = lerp(0.82, 0.96, strength);

    // Contrast-adaptive micro-acutance headroom for softened mid-tone features (video, images, scaled UI/icons).
    // Automatically collapses to 0.0 on full-contrast [0, 1] edges so black/white text never develops halos.
    float midContrast = saturate(1.0 - localRange);
    float acutanceMargin = min(
        localRange * midContrast * (0.08 + 0.52 * strength * strength),
        0.11 * strength);

    float posRoom = max(0.0, yMax - yE) * aaHeadroom + acutanceMargin;
    float negRoom = max(0.0, yE - yMin) * aaHeadroom + acutanceMargin;

    // Wide dynamic-range perceptual response curve across 0%..100%
    float gain = strength * (2.50 + 2.40 * strength);
    float rawDelta = detail * gain * noiseGate;

    if (!isHdr)
    {
        // Respect exact SDR [0, 1] per-channel headroom so ClearType subpixel chromaticity never clips or shifts hue
        float rgbPosRoom = max(0.0, 1.0 - max(e.r, max(e.g, e.b)));
        float rgbNegRoom = max(0.0, min(e.r, min(e.g, e.b)));

        float maxUp   = min(posRoom, rgbPosRoom);
        float maxDown = min(negRoom, rgbNegRoom);

        float delta = 0.0;
        if (rawDelta > 0.0 && maxUp > 1e-5)
        {
            delta = SmoothLimit(rawDelta, maxUp);
        }
        else if (rawDelta < 0.0 && maxDown > 1e-5)
        {
            delta = -SmoothLimit(-rawDelta, maxDown);
        }

        return float4(saturate(e + delta), 1.0);
    }
    else
    {
        // HDR scRGB linear float path: convert bounded perceptual sqrt-luma delta back to linear delta
        float pDelta = 0.0;
        if (rawDelta > 0.0 && posRoom > 1e-5)
        {
            pDelta = SmoothLimit(rawDelta, posRoom);
        }
        else if (rawDelta < 0.0 && negRoom > 1e-5)
        {
            pDelta = -SmoothLimit(-rawDelta, negRoom);
        }

        float yTargetP = max(0.0, yE + pDelta);
        float linCur = yE * yE;
        float linTgt = yTargetP * yTargetP;
        float linDelta = linTgt - linCur;

        float rgbNegRoom = max(0.0, min(e.r, min(e.g, e.b)));
        if (linDelta < -rgbNegRoom)
        {
            linDelta = -rgbNegRoom;
        }

        return float4(max(0.0, e + linDelta), 1.0);
    }
}
