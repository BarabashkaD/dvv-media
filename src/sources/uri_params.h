#pragma once

// URI-parameter parsing shared by the built-in source providers (dvv_media_sources). Every error
// is Errc::Format with a message that names the parameter or the bad text.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "dvv/base/error.h"
#include "dvv/base/uri.h"
#include "dvv/media/rate_size_text.h"
#include "dvv/media/types.h"
#include "dvv/media/uri_colour.h"

namespace dvv::media::detail {

// Unsigned decimal integer. Errc::Format naming the parameter.
[[nodiscard]] Result<std::uint64_t> parseUnsigned(std::string_view name, std::string_view text);
// A provider's own parameter names followed by COLOUR_PARAMETERS (dvv/media/uri_colour.h), for
// checkParameters().
template <std::size_t N>
[[nodiscard]] constexpr std::array<std::string_view, N + COLOUR_PARAMETERS.size()>
withColourParameters(const std::array<std::string_view, N>& own) {
    std::array<std::string_view, N + COLOUR_PARAMETERS.size()> all{};
    std::ranges::copy(COLOUR_PARAMETERS, std::ranges::copy(own, all.begin()).out);
    return all;
}
// Errc::Format "unknown parameter 'x'" for a query name outside `allowed` ("impl" is always
// allowed).
[[nodiscard]] Result<void> checkParameters(const Uri& uri,
                                           std::span<const std::string_view> allowed);
// Size from size=WxH, or w= and h= (both required together); nullopt when none is given.
// Errc::Format when both forms are given or one of w/h is missing.
[[nodiscard]] Result<std::optional<std::pair<std::uint32_t, std::uint32_t>>> sizeFromQuery(
    const Uri& uri);
// A catalogue format by name: unpacked and custom formats are interned only on use, so accepting
// them would make the same URI open or fail depending on the process's history. Errc::Format
// "unknown pixel format '<name>'".
[[nodiscard]] Result<FormatId> parseFormat(std::string_view name);

}  // namespace dvv::media::detail
