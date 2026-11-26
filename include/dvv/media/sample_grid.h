#pragma once

// The sample grid of a component: which sample of a subsampled component an image pixel belongs to
// (nearest, no filtering: sample = pixel >> log2Sub per axis), which image pixels a sample covers,
// and where a sample is stored in the original bytes (its word and the word's padding). One rule,
// written once: shaders/sample_grid.slang (`import dvv_media.sample_grid;`) mirrors these functions
// statement by statement; the ToDisplay shader reads its planes with it and the GPU unpack reads
// the stored words with it. The arithmetic is 32-bit unsigned, as in the shader, except byte
// offsets (64-bit).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "dvv/base/error.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"
#include "dvv/media/view.h"
#include "dvv/platform/pixel_size.h"

namespace dvv::media {

struct SamplePosition {  // a sample in its component's own grid
    std::uint32_t x = 0, y = 0;
    bool operator==(const SamplePosition&) const = default;
};

struct PixelBlock {  // the image pixels of one sample: [x, x + width) x [y, y + height)
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
    bool operator==(const PixelBlock&) const = default;
};

// The sample of `component` that image pixel `pixel` belongs to: {x >> log2SubX, y >> log2SubY}.
[[nodiscard]] constexpr SamplePosition sampleOf(const ComponentDesc& component,
                                                ImagePixel pixel) noexcept {
    return {.x = pixel.x >> component.log2SubX, .y = pixel.y >> component.log2SubY};
}

// The image pixels that share `sample` of `component` in an image of size `image`: per axis
// [s << sub, min((s + 1) << sub, size)). Empty ({0, 0, 0, 0}) outside the component's grid, i.e.
// where s << sub >= size.
[[nodiscard]] PixelBlock blockOf(const ComponentDesc& component, SamplePosition sample,
                                 dvv::platform::PixelSize image) noexcept;

// Where a sample of a component is stored in the original bytes of a frame (the tight geometry()
// layout): its word, the sample's bits in it and the word's padding. Derived from the PlaneDesc
// slots, for every format. shaders/sample_grid.slang computes the same from a StoredWordLayout.
struct StoredWord {
    std::uint8_t plane = 0;
    // The word's first byte, from the frame start (geometry() layout).
    std::uint64_t byteOffset = 0;
    std::uint8_t wordBytes = 1;  // 1, 2 or 4
    ByteOrder order = ByteOrder::Little;
    std::uint8_t shift = 0, bits = 0;  // the sample's LSB in the word and its significant bits
    std::uint32_t paddingMask = 0;     // the word's bits that no slot of the group covers
    std::array<std::uint8_t, MAX_SLOTS> sharedShifts{};  // every sample in the word (this one too),
    std::uint8_t sharedCount = 0;                        // by ascending shift
    bool operator==(const StoredWord&) const = default;
};

// The stored word of `sample` of `component` in a width x height frame. Errc::Internal for an
// unknown format (through its empty descriptor), a bad component, or a sample outside the
// component's grid; Errc::Format from geometry().
[[nodiscard]] Result<StoredWord> storedWord(FormatId format, std::uint32_t width,
                                            std::uint32_t height, std::size_t component,
                                            SamplePosition sample);

// The word at `word.byteOffset - first_byte` in `bytes`, byte order applied; 0 when the word does
// not lie wholly in `bytes`. `first_byte` is the frame offset of bytes[0], so a caller can pass a
// copied window of the frame.
[[nodiscard]] std::uint32_t readStoredWord(std::span<const std::byte> bytes,
                                           std::uint64_t first_byte,
                                           const StoredWord& word) noexcept;

// The code of a sample in its stored word: `bits` (1..16) bits from bit `shift`.
[[nodiscard]] constexpr std::uint32_t codeOf(std::uint32_t word, std::uint8_t shift,
                                             std::uint8_t bits) noexcept {
    return (word >> shift) & ((1U << bits) - 1U);
}

// The stored-word rule of one component as shader data (the GPU unpack's push constants, the
// values layer). 72 bytes, 8-aligned; the same layout as `StoredWordLayout` in
// shaders/sample_grid.slang (its arrays are `uint[]`, so the layout is the same in std430 and
// scalar blocks).
struct StoredWordLayout {
    std::uint64_t planeOffset = 0;  // byte offset of the component's plane inside the frame
    std::uint32_t stride = 0;       // bytes per plane row
    std::uint32_t groupBytes = 0;
    std::uint32_t wordBytes = 0;        // 1, 2 or 4
    std::uint32_t bigEndian = 0;        // 1: words are big-endian
    std::uint32_t bits = 0;             // significant bits of the component
    std::uint32_t samplesPerGroup = 0;  // samples of the component per group
    // Sample k: (word << 8 | shift) in bits 16 * (k % 2) of [k / 2].
    std::array<std::uint32_t, 3> slots{};
    std::array<std::uint32_t, 6> paddingMasks{};  // sample k: the padding mask of its word
    std::uint32_t pad = 0;
};
static_assert(sizeof(StoredWordLayout) == 72 && alignof(StoredWordLayout) == 8);
static_assert(offsetof(StoredWordLayout, planeOffset) == 0 &&
              offsetof(StoredWordLayout, stride) == 8 &&
              offsetof(StoredWordLayout, samplesPerGroup) == 28 &&
              offsetof(StoredWordLayout, slots) == 32 &&
              offsetof(StoredWordLayout, paddingMasks) == 44 &&
              offsetof(StoredWordLayout, pad) == 68);
static_assert(std::tuple_size_v<decltype(StoredWordLayout::paddingMasks)> == MAX_COMPONENT_SAMPLES);

// The stored-word rule of `component` in a width x height frame. Errc::Internal for an unknown
// format or a component without samples, one whose samples span planes, or more than
// MAX_COMPONENT_SAMPLES per group, indices that are not 0..n-1, or n != groupPixels >> log2SubX;
// Errc::Format from geometry().
[[nodiscard]] Result<StoredWordLayout> storedWordLayout(FormatId format, std::uint32_t width,
                                                        std::uint32_t height,
                                                        std::size_t component);

}  // namespace dvv::media
