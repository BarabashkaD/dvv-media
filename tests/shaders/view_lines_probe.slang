// Writes every view_lines function of one axis into an R32_UINT image, one invocation per column
// i in [0, width): the GPU half of "view lines: the shader matches the C++ mirror"
// (gpu_test_view_lines.cpp, which lists the rows). Signed values are stored with asuint.
import dvv_media.view_lines;

using namespace dvv::media;

struct ProbePush
{
    int offset;  // the AxisView
    float zoom;
    uint size;
    uint extent;
    uint4 levels[3];  // LineLevel: stepLog2, thickness, drawn, halo
    uint width;
    float minimum;  // levelVisible's minimum spacing
    uint pad0;
    uint pad1;
};

[[vk::push_constant]] ConstantBuffer<ProbePush> pc;

[[vk::binding(0, 0)]] [format("r32ui")] RWTexture2D<uint> rows;

void writeSpan(uint x, uint row, PixelSpan span)
{
    rows[uint2(x, row)] = asuint(span.begin);
    rows[uint2(x, row + 1u)] = asuint(span.end);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id: SV_DispatchThreadID)
{
    const uint i = id.x;
    if (i >= pc.width)
    {
        return;
    }
    AxisView axis = { pc.offset, pc.zoom, pc.size, pc.extent };
    LineLevel levels[LINE_LEVELS];
    for (uint l = 0u; l < LINE_LEVELS; ++l)
    {
        const uint4 v = pc.levels[l];
        LineLevel level = { v.x, v.y, v.z != 0u, v.w != 0u };
        levels[l] = level;
    }
    const int pixel = int(i);
    rows[uint2(i, 0u)] = asuint(boundaryAt(axis, i));
    rows[uint2(i, 1u)] = asuint(imageCoordinateAt(axis, pixel));
    rows[uint2(i, 2u)] = asuint(firstPixelOf(axis, i));
    for (uint t = 1u; t <= 4u; ++t)
    {
        writeSpan(i, 1u + 2u * t, lineSpan(axis, i, t));
    }
    writeSpan(i, 11u, haloSpan(axis, i, 2u));
    writeSpan(i, 13u, cursorSpan(axis, i));
    writeSpan(i, 15u, imageSpan(axis));
    const LineHit hit = lineAt(axis, levels, pixel);
    rows[uint2(i, 17u)] = hit.hit ? hit.level | hit.k << 2u | (hit.halo ? 1u : 0u) << 31u
                                  : 0xFFFFFFFFu;
    rows[uint2(i, 18u)] = tickLevel(i, levels);
    rows[uint2(i, 19u)] = levelVisible(i, pc.zoom, pc.minimum) ? 1u : 0u;
}
