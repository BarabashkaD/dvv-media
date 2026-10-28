#pragma once

// The shared code-value generators (M2 design D16): the GPU tests (Task 6) and the synthetic
// source (Task 10) produce the same frames from them.

#include <cstddef>
#include <cstdint>

#include "dvv/base/error.h"
#include "dvv/media/reference.h"
#include "dvv/media/types.h"

namespace dvv::media::detail {

enum class Pattern : std::uint8_t { Gradient, Noise };

// SplitMix64 finaliser step: z += 0x9E3779B97F4A7C15; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9;
// z = (z ^ (z >> 27)) * 0x94D049BB133111EB; return z ^ (z >> 31). (All arithmetic mod 2^64.)
[[nodiscard]] constexpr std::uint64_t splitMix64(std::uint64_t z) noexcept {
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

// D16 gradient: maxv = 2^bits - 1; ramp = (x + y) * maxv / max(grid_width + grid_height - 2, 1);
// value = (ramp + uint64(index) + component * ((maxv + 1) / 4)) mod (maxv + 1).
[[nodiscard]] std::uint16_t gradientValue(std::uint8_t bits, std::size_t component, std::uint32_t x,
                                          std::uint32_t y, std::uint32_t grid_width,
                                          std::uint32_t grid_height, std::int64_t index) noexcept;
// D16 noise: h = splitMix64(seed); h = splitMix64(h ^ uint64(index)); h = splitMix64(h ^
// component); h = splitMix64(h ^ y); h = splitMix64(h ^ x); value = h & maxv.
[[nodiscard]] std::uint16_t noiseValue(std::uint8_t bits, std::uint64_t seed, std::int64_t index,
                                       std::size_t component, std::uint32_t x,
                                       std::uint32_t y) noexcept;
// Code values of a whole frame (each component on its componentSize() grid). The geometry() checks
// apply (Format for an odd v210 width, Internal for an unknown id). `seed` is ignored for Gradient.
[[nodiscard]] Result<CodeValues> generate(Pattern pattern, FormatId format, std::uint32_t width,
                                          std::uint32_t height, std::uint64_t seed,
                                          std::int64_t index);

}  // namespace dvv::media::detail
