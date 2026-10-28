#pragma once

// Private integer helpers of the media layer.

#include <cstdint>

namespace dvv::media::detail {

// ceil(value / divisor); divisor > 0. Never overflows.
[[nodiscard]] constexpr std::uint64_t ceilDiv(std::uint64_t value, std::uint64_t divisor) noexcept {
    return value / divisor + (value % divisor != 0 ? 1U : 0U);
}

// value rounded up to a multiple of alignment; alignment > 0, not necessarily a power of two.
[[nodiscard]] constexpr std::uint64_t alignUp(std::uint64_t value,
                                              std::uint64_t alignment) noexcept {
    return ceilDiv(value, alignment) * alignment;
}

// value rounded down to a multiple of alignment; alignment > 0, not necessarily a power of two.
[[nodiscard]] constexpr std::uint64_t alignDown(std::uint64_t value,
                                                std::uint64_t alignment) noexcept {
    return value / alignment * alignment;
}

}  // namespace dvv::media::detail
