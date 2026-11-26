// ToDisplay (design M3 D3-D5): one invocation per output pixel reads the code values of the
// unpacked planes (nearest chroma: x >> log2SubX), applies the affine map of displayAffine(),
// clamps, applies the transfer (sRGB/BT.1886/linear, or PQ/HLG tone mapped to SDR with the BT.2390
// EETF, 1000 -> 100 nits), clamps and stores linear RGBA16F: RGB and the frame's straight,
// full-range alpha (1 without A). The primaries are information only (D5R §2.1): no gamut
// conversion. Must match dvv::media::toDisplayReference (display.cpp) formula by formula.

import sample_grid;

struct ToDisplayPush
{
    float4 row0;          // R' = dot(row0.xyz, c) + row0.w
    float4 row1;          // G'
    float4 row2;          // B'
    uint4 planeLayout;    // component k: log2SubX | log2SubY << 4 | binding << 8
    uint width;           // output size = frame size
    uint height;
    uint componentCount;  // 1..4 (alpha last, when 2 or 4)
    uint transfer;        // 0 sRGB/unspecified, 1 BT.1886, 2 linear, 3 PQ, 4 HLG
    float alphaScale;     // 1 / (2^bits - 1) of A; 0 without A
    uint hiddenMask;      // bit k: colour component k is hidden
    uint chromaFill;      // hidden c1 code in bits 0..15, hidden c2 code in 16..31 (0: code 0)
    uint pad;
};

[[vk::push_constant]] ConstantBuffer<ToDisplayPush> pc;

[[vk::binding(0, 0)]] [format("unknown")] RWTexture2D<uint> plane0;
[[vk::binding(1, 0)]] [format("unknown")] RWTexture2D<uint> plane1;
[[vk::binding(2, 0)]] [format("unknown")] RWTexture2D<uint> plane2;
[[vk::binding(3, 0)]] [format("unknown")] RWTexture2D<uint> plane3;
[[vk::binding(4, 0)]] [format("rgba16f")] RWTexture2D<float4> display; // DISPLAY_IMAGE_FORMAT (dvv/media/display_vulkan.h)

static const float PQ_M1 = 0.1593017578125;
static const float PQ_M2 = 78.84375;
static const float PQ_C1 = 0.8359375;
static const float PQ_C2 = 18.8515625;
static const float PQ_C3 = 18.6875;
static const float HLG_A = 0.17883277;
static const float HLG_B = 0.28466892;
static const float HLG_C = 0.55991073;
static const float SOURCE_PEAK_NITS = 1000.0;
static const float TARGET_PEAK_NITS = 100.0;

uint readPlane(uint binding, uint2 at)
{
    switch (binding)
    {
    case 0: return plane0[at];
    case 1: return plane1[at];
    case 2: return plane2[at];
    default: return plane3[at];
    }
}

float codeValue(uint k, uint2 pixel)
{
    const uint layout = pc.planeLayout[k];
    const uint2 at = dvv::media::sampleOf(pixel, layout & 15u, (layout >> 4u) & 15u);
    return float(readPlane((layout >> 8u) & 15u, at));
}

float srgbDecode(float x)
{
    x = clamp(x, 0.0, 1.0);
    return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
}

float pqToNits(float e)
{
    e = clamp(e, 0.0, 1.0);
    const float p = pow(e, 1.0 / PQ_M2);
    const float num = max(p - PQ_C1, 0.0);
    return 10000.0 * pow(num / (PQ_C2 - PQ_C3 * p), 1.0 / PQ_M1);
}

float nitsToPq(float nits)
{
    const float y = clamp(nits / 10000.0, 0.0, 1.0);
    const float p = pow(y, PQ_M1);
    return pow((PQ_C1 + PQ_C2 * p) / (1.0 + PQ_C3 * p), PQ_M2);
}

float eetf(float e)
{
    const float source = nitsToPq(SOURCE_PEAK_NITS);
    const float maxLum = nitsToPq(TARGET_PEAK_NITS) / source;
    const float ks = 1.5 * maxLum - 0.5;
    const float e1 = min(e / source, 1.0);
    float e2 = e1;
    if (e1 >= ks)
    {
        const float t = (e1 - ks) / (1.0 - ks);
        const float t2 = t * t;
        const float t3 = t2 * t;
        e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks) +
             (-2.0 * t3 + 3.0 * t2) * maxLum;
    }
    return e2 * source;
}

float pqToLinear(float e) { return pqToNits(eetf(e)) / TARGET_PEAK_NITS; }
float nitsToLinear(float nits) { return pqToLinear(nitsToPq(nits)); }

float hlgInverseOetf(float e)
{
    return e <= 0.5 ? e * e / 3.0 : (exp((e - HLG_C) / HLG_A) + HLG_B) / 12.0;
}

float3 toLinear(float3 v)
{
    switch (pc.transfer)
    {
    case 1u: return float3(pow(v.x, 2.4), pow(v.y, 2.4), pow(v.z, 2.4));
    case 2u: return v;
    case 3u: return float3(pqToLinear(v.x), pqToLinear(v.y), pqToLinear(v.z));
    case 4u:
    {
        const float3 e = float3(hlgInverseOetf(v.x), hlgInverseOetf(v.y), hlgInverseOetf(v.z));
        const float ys = 0.2627 * e.x + 0.6780 * e.y + 0.0593 * e.z;
        const float gain = SOURCE_PEAK_NITS * pow(ys, 0.2);
        return float3(nitsToLinear(gain * e.x), nitsToLinear(gain * e.y), nitsToLinear(gain * e.z));
    }
    default: return float3(srgbDecode(v.x), srgbDecode(v.y), srgbDecode(v.z));
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 id: SV_DispatchThreadID)
{
    if (id.x >= pc.width || id.y >= pc.height)
    {
        return;
    }
    float3 c = float3(0.0, 0.0, 0.0);
    c.x = (pc.hiddenMask & 1u) != 0u ? 0.0 : codeValue(0u, id.xy);
    if (pc.componentCount >= 3u)
    {
        c.y = (pc.hiddenMask & 2u) != 0u ? float(pc.chromaFill & 0xFFFFu) : codeValue(1u, id.xy);
        c.z = (pc.hiddenMask & 4u) != 0u ? float(pc.chromaFill >> 16u) : codeValue(2u, id.xy);
    }
    float3 v = float3(pc.row0.x * c.x + pc.row0.y * c.y + pc.row0.z * c.z + pc.row0.w,
                      pc.row1.x * c.x + pc.row1.y * c.y + pc.row1.z * c.z + pc.row1.w,
                      pc.row2.x * c.x + pc.row2.y * c.y + pc.row2.z * c.z + pc.row2.w);
    v = clamp(v, 0.0, 1.0);
    float3 rgb = toLinear(v);
    // The frame's alpha: straight, full range (also in limited-range YUV), 1 without A.
    float alpha = 1.0;
    if (pc.alphaScale > 0.0)
    {
        alpha = clamp(codeValue(pc.componentCount - 1u, id.xy) * pc.alphaScale, 0.0, 1.0);
    }
    display[id.xy] = float4(clamp(rgb, 0.0, 1.0), alpha);
}
