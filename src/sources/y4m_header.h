#pragma once

// The YUV4MPEG2 header parser of the y4m source provider (M2 design §5.6, D15, R10, F6); private,
// declared here for the unit tests.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "dvv/base/error.h"
#include "dvv/media/types.h"

namespace dvv::media::detail {

inline constexpr std::string_view Y4M_MAGIC = "YUV4MPEG2 ";
// The header must end within this many bytes of the file.
inline constexpr std::size_t Y4M_MAX_HEADER = 4096;

struct Y4mHeader {
    std::size_t bytes = 0;  // header length including its '\n'
    std::uint32_t width = 0, height = 0;
    Rational fps{25, 1};
    FormatId format = 0;
    ColourRange range = ColourRange::Unspecified;
};

// `head` = the first bytes of the file (up to 4096; more are ignored). Tags W H F I A C X,
// separated by single spaces. Errc::Format: no magic ("not a YUV4MPEG2 file"), no '\n' in the first
// 4096 bytes, a missing or bad W or H (1..65535), a bad F<n>:<d>, an XCOLORRANGE other than FULL or
// LIMITED, an empty tag, or another tag letter ("unknown Y4M header tag 'Q'"). Errc::Unsupported:
// a header ending in "\r\n", or a C value outside the D15 mapping ("Y4M colourspace 'X' is not
// supported"). F is reduced by the gcd; a missing F is 25/1, a missing C 420jpeg (yuv420p).
[[nodiscard]] Result<Y4mHeader> parseY4mHeader(std::span<const std::byte> head);

}  // namespace dvv::media::detail
