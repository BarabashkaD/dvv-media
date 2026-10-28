// Generic unpack (M2 design §5.4): one dispatch per output component. Each invocation reads one
// sample of the component from the original bytes (by buffer device address) and writes its
// LSB-aligned code value into the component's R8_UINT or R16_UINT storage image. The two dwords
// around the sample's word are loaded at the absolute byte address, so planes and words need no
// alignment beyond the 4-aligned frame base (buffer size rule: offset + alignUp(bytes, 4) + 4).
struct UnpackPush
{
    uint* base;            // device address of the frame's first byte; 4-aligned
    uint64_t planeOffset;  // byte offset of the component's plane inside the frame
    uint stride;           // bytes per plane row
    uint groupBytes;       // bytes per group
    uint wordBytes;        // 1, 2 or 4
    uint bigEndian;        // 1: words are big-endian
    uint bits;             // significant bits of the component
    uint width;            // the component's grid size
    uint height;
    uint samplesPerGroup;  // samples of the component per group
    uint3 slots;           // slot k: (word << 8 | shift) in bits 16 * (k % 2) of slots[k / 2]
    uint pad;              // explicit padding to 64 bytes
};

[[vk::push_constant]] ConstantBuffer<UnpackPush> pc;

[[vk::binding(0, 0)]] [format("unknown")] RWTexture2D<uint> plane;

uint swapBytes(uint word, uint bytes)
{
    if (bytes == 2u)
    {
        return ((word & 0xFFu) << 8u) | (word >> 8u);
    }
    if (bytes == 4u)
    {
        return (word << 24u) | ((word & 0xFF00u) << 8u) | ((word >> 8u) & 0xFF00u) | (word >> 24u);
    }
    return word;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 id: SV_DispatchThreadID)
{
    if (id.x >= pc.width || id.y >= pc.height)
    {
        return;
    }
    const uint group = id.x / pc.samplesPerGroup;
    const uint sample = id.x - group * pc.samplesPerGroup;
    const uint slot = (pc.slots[sample >> 1u] >> ((sample & 1u) * 16u)) & 0xFFFFu;
    const uint word = slot >> 8u;
    const uint shift = slot & 0xFFu;
    const uint64_t byteAddress = pc.planeOffset + uint64_t(id.y) * uint64_t(pc.stride) +
                                 uint64_t(group) * uint64_t(pc.groupBytes) +
                                 uint64_t(word) * uint64_t(pc.wordBytes);
    const uint dword = uint(byteAddress >> 2ull);
    const uint64_t pair = (uint64_t(pc.base[dword + 1u]) << 32ull) | uint64_t(pc.base[dword]);
    uint value = uint(pair >> ((byteAddress & 3ull) * 8ull));
    if (pc.wordBytes < 4u)
    {
        value &= (1u << (pc.wordBytes * 8u)) - 1u;
    }
    if (pc.bigEndian != 0u)
    {
        value = swapBytes(value, pc.wordBytes);
    }
    plane[id.xy] = (value >> shift) & ((1u << pc.bits) - 1u);
}
