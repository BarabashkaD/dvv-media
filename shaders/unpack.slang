// Generic unpack (M2 design §5.4): one dispatch per output component. Each invocation reads one
// sample of the component from the original bytes (by buffer device address) and writes its
// LSB-aligned code value into the component's R8_UINT or R16_UINT storage image. The stored word
// is found and read through dvv_media.sample_grid (storedWordAddress, readStoredWord, codeOf), the
// rule the values layer and the padding check use too; the read loads the two dwords around the
// word at its absolute byte address, so planes and words need no alignment beyond the 4-aligned
// frame base (buffer size rule: offset + alignUp(bytes, 4) + 4).
import sample_grid;

using namespace dvv::media;

struct UnpackPush
{
    uint* base;                // device address of the frame's first byte; 4-aligned
    StoredWordLayout layout;   // the component's stored-word rule
    uint width;                // the component's grid size
    uint height;
};

[[vk::push_constant]] ConstantBuffer<UnpackPush> pc;

[[vk::binding(0, 0)]] [format("unknown")] RWTexture2D<uint> plane;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 id: SV_DispatchThreadID)
{
    if (id.x >= pc.width || id.y >= pc.height)
    {
        return;
    }
    uint shift;
    uint paddingMask;
    const uint64_t byteAddress = storedWordAddress(pc.layout, id.xy, shift, paddingMask);
    const uint word = readStoredWord(pc.base, byteAddress, pc.layout.wordBytes,
                                     pc.layout.bigEndian);
    plane[id.xy] = codeOf(word, shift, pc.layout.bits);
}
