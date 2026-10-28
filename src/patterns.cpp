#include "patterns.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>
#include <vector>

#include "dvv/media/pixel_format.h"

namespace dvv::media::detail {

namespace {

// 2^bits - 1 for bits in 1..16 (descriptors are validated).
[[nodiscard]] constexpr std::uint64_t maxValue(std::uint8_t bits) noexcept {
    return (std::uint64_t{1} << bits) - 1U;
}

}  // namespace

std::uint16_t gradientValue(std::uint8_t bits, std::size_t component, std::uint32_t x,
                            std::uint32_t y, std::uint32_t grid_width, std::uint32_t grid_height,
                            std::int64_t index) noexcept {
    const std::uint64_t maxv = maxValue(bits);
    // max(grid_width + grid_height - 2, 1) without going below 0.
    const std::uint64_t span =
        std::max<std::uint64_t>(std::uint64_t{grid_width} + grid_height, 3U) - 2U;
    const std::uint64_t ramp = (std::uint64_t{x} + y) * maxv / span;
    const std::uint64_t value =
        ramp + static_cast<std::uint64_t>(index) + (component * ((maxv + 1U) / 4U));
    return static_cast<std::uint16_t>(value % (maxv + 1U));
}

std::uint16_t noiseValue(std::uint8_t bits, std::uint64_t seed, std::int64_t index,
                         std::size_t component, std::uint32_t x, std::uint32_t y) noexcept {
    std::uint64_t h = splitMix64(seed);
    h = splitMix64(h ^ static_cast<std::uint64_t>(index));
    h = splitMix64(h ^ std::uint64_t{component});
    h = splitMix64(h ^ y);
    h = splitMix64(h ^ x);
    return static_cast<std::uint16_t>(h & maxValue(bits));
}

Result<CodeValues> generate(Pattern pattern, FormatId format, std::uint32_t width,
                            std::uint32_t height, std::uint64_t seed, std::int64_t index) {
    if (auto frame = geometry(format, width, height); !frame) {
        return std::unexpected(std::move(frame.error()));
    }
    const PixelFormatDescriptor& d = descriptor(format);
    CodeValues result{.format = format, .width = width, .height = height};
    for (std::size_t c = 0; c < d.componentCount; ++c) {
        const std::uint8_t bits = d.components[c].bits;
        const auto [cw, ch] = componentSize(format, c, width, height);
        std::vector<std::uint16_t>& values = result.components[c];
        values.reserve(std::size_t{cw} * ch);
        for (std::uint32_t y = 0; y < ch; ++y) {
            for (std::uint32_t x = 0; x < cw; ++x) {
                values.push_back(pattern == Pattern::Gradient
                                     ? gradientValue(bits, c, x, y, cw, ch, index)
                                     : noiseValue(bits, seed, index, c, x, y));
            }
        }
    }
    return result;
}

}  // namespace dvv::media::detail
