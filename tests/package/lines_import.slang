// A consumer's shader that imports dvv-media's exported line rule.
import dvv_media.view_lines;

using namespace dvv::media;

[[vk::binding(0, 0)]] [format("r32ui")] RWTexture2D<uint> image;

[shader("compute")]
[numthreads(1, 1, 1)]
void main()
{
    const AxisView axis = { 0, 2.0, 8u, 16u };
    image[uint2(0, 0)] = asuint(lineSpan(axis, 4u, 1u).begin);
}
