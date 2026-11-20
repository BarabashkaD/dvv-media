#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "dvv/base/error.h"
#include "dvv/media/types.h"

namespace dvv::media {

// "WxH", both 1..65535 decimal. Errc::Format "bad size '<text>' (expected WxH)".
[[nodiscard]] Result<std::pair<std::uint32_t, std::uint32_t>> parseSize(std::string_view text);
// One dimension 1..65535 (w=, h=). Errc::Format naming the parameter.
[[nodiscard]] Result<std::uint32_t> parseDimension(std::string_view name, std::string_view text);
// "N", "N/D" (integers > 0) or "I.F" (up to 9 decimals; "29.97" is 2997/100), reduced by the gcd.
// Errc::Format "bad fps '<text>'".
[[nodiscard]] Result<Rational> parseFps(std::string_view text);
// "N" when den == 1, else "N/D" (the resolved-URI form).
[[nodiscard]] std::string formatFps(Rational fps);

}  // namespace dvv::media
