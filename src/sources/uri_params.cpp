#include "sources/uri_params.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "dvv/media/uri_colour.h"

namespace dvv::media::detail {

namespace {

constexpr std::uint64_t MAX_DIMENSION = 65535;
// "I.F" fps: at most 9 fraction digits and an integer part up to 1e9 keep I * 10^9 + F in range.
constexpr std::size_t MAX_FRACTION_DIGITS = 9;
constexpr std::uint64_t MAX_FPS_INTEGER = 1'000'000'000;
constexpr auto MAX_INT64 = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

// A non-empty run of ASCII digits that fits in 64 bits; nothing else (no sign, no spaces).
std::optional<std::uint64_t> parseDigits(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const char* const first = std::to_address(text.begin());
    const char* const end = std::to_address(text.end());
    const auto [last, error] = std::from_chars(first, end, value);
    if (error != std::errc{} || last != end) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::uint32_t> dimension(std::string_view text) {
    const auto value = parseDigits(text);
    if (!value || *value == 0 || *value > MAX_DIMENSION) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*value);
}

}  // namespace

Result<std::pair<std::uint32_t, std::uint32_t>> parseSize(std::string_view text) {
    const std::size_t x = text.find('x');
    if (x != std::string_view::npos) {
        const auto width = dimension(text.substr(0, x));
        const auto height = dimension(text.substr(x + 1));
        if (width && height) {
            return std::pair{*width, *height};
        }
    }
    return makeError(Errc::Format, std::format("bad size '{}' (expected WxH)", text));
}

Result<std::uint32_t> parseDimension(std::string_view name, std::string_view text) {
    if (const auto value = dimension(text)) {
        return *value;
    }
    return makeError(Errc::Format, std::format("bad {} '{}' (expected 1..65535)", name, text));
}

Result<Rational> parseFps(std::string_view text) {
    std::uint64_t num = 0;
    std::uint64_t den = 1;
    if (const std::size_t slash = text.find('/'); slash != std::string_view::npos) {
        const auto n = parseDigits(text.substr(0, slash));
        const auto d = parseDigits(text.substr(slash + 1));
        if (n && d) {
            num = *n;
            den = *d;
        }
    } else if (const std::size_t dot = text.find('.'); dot != std::string_view::npos) {
        const std::string_view fraction = text.substr(dot + 1);
        const auto i = parseDigits(text.substr(0, dot));
        const auto f = parseDigits(fraction);
        if (i && f && *i <= MAX_FPS_INTEGER && fraction.size() <= MAX_FRACTION_DIGITS) {
            den = 1;
            for (std::size_t digit = 0; digit < fraction.size(); ++digit) {
                den *= 10;
            }
            num = *i * den + *f;
        }
    } else if (const auto n = parseDigits(text)) {
        num = *n;
    }
    if (num == 0 || den == 0 || num > MAX_INT64 || den > MAX_INT64) {
        return makeError(Errc::Format, std::format("bad fps '{}'", text));
    }
    const std::uint64_t divisor = std::gcd(num, den);
    return Rational{static_cast<std::int64_t>(num / divisor),
                    static_cast<std::int64_t>(den / divisor)};
}

std::string formatFps(Rational fps) {
    if (fps.den == 1) {
        return std::format("{}", fps.num);
    }
    return std::format("{}/{}", fps.num, fps.den);
}

Result<std::uint64_t> parseUnsigned(std::string_view name, std::string_view text) {
    if (const auto value = parseDigits(text)) {
        return *value;
    }
    return makeError(Errc::Format,
                     std::format("bad {} '{}' (expected an unsigned integer)", name, text));
}

Result<void> checkParameters(const Uri& uri, std::span<const std::string_view> allowed) {
    for (const auto& param : uri.params) {
        const std::string& name = param.first;
        if (name != "impl" && std::ranges::find(allowed, std::string_view(name)) == allowed.end()) {
            return makeError(Errc::Format, unknownParameterText(name));
        }
    }
    return {};
}

Result<std::optional<std::pair<std::uint32_t, std::uint32_t>>> sizeFromQuery(const Uri& uri) {
    const auto size = uri.param("size");
    const auto width = uri.param("w");
    const auto height = uri.param("h");
    if (size) {
        if (width || height) {
            return makeError(Errc::Format, "give the size either as size= or as w= and h=");
        }
        DVV_TRY(const auto parsed, parseSize(*size));
        return parsed;
    }
    if (!width && !height) {
        return std::nullopt;
    }
    if (!width || !height) {
        return makeError(Errc::Format, "w= and h= must be given together");
    }
    DVV_TRY(const std::uint32_t w, parseDimension("w", *width));
    DVV_TRY(const std::uint32_t h, parseDimension("h", *height));
    return std::pair{w, h};
}

}  // namespace dvv::media::detail
