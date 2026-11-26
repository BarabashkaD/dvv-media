// Writes sampleOf and blockOf for every pixel of an image into an R32_UINT image, one invocation
// per pixel: the GPU half of "sample grid: the shader matches the C++ rule"
// (gpu_test_sample_grid.cpp).
import dvv_media.sample_grid;

using namespace dvv::media;

struct ProbePush
{
    uint2 image;
    uint log2SubX;
    uint log2SubY;
};

[[vk::push_constant]] ConstantBuffer<ProbePush> pc;

// Six planes of image.y rows each, cell (x, y) of each: sampleOf(pixel (x, y)) x and y, then the
// x, y, width and height of blockOf(sample position (x, y)), empty beyond the grid.
[[vk::binding(0, 0)]] [format("r32ui")] RWTexture2D<uint> cells;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 id: SV_DispatchThreadID)
{
    if (id.x >= pc.image.x || id.y >= pc.image.y)
    {
        return;
    }
    const uint2 pixel = id.xy;
    const uint2 sample = sampleOf(pixel, pc.log2SubX, pc.log2SubY);
    const uint4 block = blockOf(pixel, pc.log2SubX, pc.log2SubY, pc.image);
    const uint h = pc.image.y;
    cells[uint2(id.x, id.y)] = sample.x;
    cells[uint2(id.x, id.y + h)] = sample.y;
    cells[uint2(id.x, id.y + 2u * h)] = block.x;
    cells[uint2(id.x, id.y + 3u * h)] = block.y;
    cells[uint2(id.x, id.y + 4u * h)] = block.z;
    cells[uint2(id.x, id.y + 5u * h)] = block.w;
}
