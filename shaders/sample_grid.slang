// The sample grid of a component, for shaders: `import dvv_media.sample_grid;`, then
// `dvv::media::sampleOf` or `using namespace dvv::media;`. Must match dvv/media/sample_grid.h: the
// same names in the same namespace, the same statements. The header documents the functions.

namespace dvv::media
{

// The sample of a component that image pixel `pixel` belongs to.
public uint2 sampleOf(uint2 pixel, uint log2SubX, uint log2SubY)
{
    return uint2(pixel.x >> log2SubX, pixel.y >> log2SubY);
}

// The image pixels that share `sample`: (x, y, width, height); all 0 outside the component's grid.
public uint4 blockOf(uint2 sample, uint log2SubX, uint log2SubY, uint2 image)
{
    const uint x0 = sample.x << log2SubX;
    const uint y0 = sample.y << log2SubY;
    if (x0 >= image.x || y0 >= image.y)
    {
        return uint4(0u, 0u, 0u, 0u);
    }
    const uint x1 = min((sample.x + 1u) << log2SubX, image.x);
    const uint y1 = min((sample.y + 1u) << log2SubY, image.y);
    return uint4(x0, y0, x1 - x0, y1 - y0);
}

// The stored-word rule of one component (dvv::media::StoredWordLayout, 72 bytes). The arrays are
// uint[] (4-aligned in std430 and scalar blocks), so the struct is 8-aligned and nests at an
// 8-byte offset in a push block.
public struct StoredWordLayout
{
    public uint64_t planeOffset;  // byte offset of the component's plane inside the frame
    public uint stride;           // bytes per plane row
    public uint groupBytes;
    public uint wordBytes;        // 1, 2 or 4
    public uint bigEndian;        // 1: words are big-endian
    public uint bits;             // significant bits of the component
    public uint samplesPerGroup;  // samples of the component per group
    public uint slots[3];         // sample k: (word << 8 | shift) in bits 16 * (k % 2) of [k / 2]
    public uint paddingMasks[6];  // sample k: the padding mask of its word
    public uint pad;
};

// The frame byte offset of the stored word of `sample` (in the component's own grid); `shift` is
// the sample's LSB in the word, `paddingMask` the word's bits that no slot of the group covers.
public uint64_t storedWordAddress(StoredWordLayout layout, uint2 sample, out uint shift,
                                  out uint paddingMask)
{
    const uint group = sample.x / layout.samplesPerGroup;
    const uint k = sample.x - group * layout.samplesPerGroup;
    const uint slot = (layout.slots[k >> 1u] >> ((k & 1u) * 16u)) & 0xFFFFu;
    const uint word = slot >> 8u;
    shift = slot & 0xFFu;
    paddingMask = layout.paddingMasks[k];
    return layout.planeOffset + uint64_t(sample.y) * uint64_t(layout.stride) +
           uint64_t(group) * uint64_t(layout.groupBytes) +
           uint64_t(word) * uint64_t(layout.wordBytes);
}

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

// The word of `wordBytes` bytes at `byteAddress` from `base` (4-aligned), byte order applied. Loads
// the two dwords around it, so the word needs no alignment and the buffer 4 bytes past the frame.
public uint readStoredWord(uint* base, uint64_t byteAddress, uint wordBytes, uint bigEndian)
{
    // 64-bit pointer arithmetic: no 32-bit dword index, so no wrap past 16 GiB.
    uint* at = base + (byteAddress >> 2ull);
    const uint64_t pair = (uint64_t(at[1]) << 32ull) | uint64_t(at[0]);
    uint value = uint(pair >> ((byteAddress & 3ull) * 8ull));
    if (wordBytes < 4u)
    {
        value &= (1u << (wordBytes * 8u)) - 1u;
    }
    if (bigEndian != 0u)
    {
        value = swapBytes(value, wordBytes);
    }
    return value;
}

// The code of a sample in its stored word: `bits` bits from bit `shift`.
public uint codeOf(uint word, uint shift, uint bits)
{
    return (word >> shift) & ((1u << bits) - 1u);
}

}
