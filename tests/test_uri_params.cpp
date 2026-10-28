#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/uri.h"
#include "dvv/media/colour_preset.h"
#include "dvv/media/types.h"
#include "dvv/media/uri_colour.h"
#include "sources/uri_params.h"

namespace media = dvv::media;
namespace params = dvv::media::detail;

namespace {

dvv::Uri uri(std::string_view text) {
    auto parsed = dvv::parseUri(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

}  // namespace

TEST_CASE("uri params: parseSize reads WxH") {
    const auto size = params::parseSize("34x17");
    REQUIRE(size.has_value());
    CHECK(*size == std::pair<std::uint32_t, std::uint32_t>{34, 17});

    for (const std::string_view bad : {"0x5", "34x", "34*17"}) {
        CAPTURE(bad);
        const auto parsed = params::parseSize(bad);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == dvv::Errc::Format);
    }
    const auto large = params::parseSize("65536x1");
    REQUIRE_FALSE(large.has_value());
    CHECK(large.error().message == "bad size '65536x1' (expected WxH)");
}

TEST_CASE("uri params: parseFps reads integers, fractions and decimals") {
    CHECK(params::parseFps("25") == media::Rational{25, 1});
    CHECK(params::parseFps("30000/1001") == media::Rational{30000, 1001});
    CHECK(params::parseFps("29.97") == media::Rational{2997, 100});
    CHECK(params::parseFps("50/2") == media::Rational{25, 1});
    CHECK(params::parseFps("12.5") == media::Rational{25, 2});

    for (const std::string_view bad : {"0", "1/0", "x"}) {
        CAPTURE(bad);
        const auto parsed = params::parseFps(bad);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == dvv::Errc::Format);
        CHECK(parsed.error().message == "bad fps '" + std::string(bad) + "'");
    }
}

TEST_CASE("uri params: formatFps writes the resolved-URI form") {
    CHECK(params::formatFps(media::Rational{25, 1}) == "25");
    CHECK(params::formatFps(media::Rational{30000, 1001}) == "30000/1001");
}

TEST_CASE("uri params: parseDimension and parseUnsigned name the parameter") {
    CHECK(params::parseDimension("w", "3") == std::uint32_t{3});
    const auto zero = params::parseDimension("w", "0");
    REQUIRE_FALSE(zero.has_value());
    CHECK(zero.error().code == dvv::Errc::Format);
    CHECK(zero.error().message.starts_with("bad w "));

    CHECK(params::parseUnsigned("frames", "10") == std::uint64_t{10});
    const auto negative = params::parseUnsigned("frames", "-1");
    REQUIRE_FALSE(negative.has_value());
    CHECK(negative.error().code == dvv::Errc::Format);
    CHECK(negative.error().message.starts_with("bad frames "));
}

TEST_CASE("uri params: colourFromQuery reads colour= and range=") {
    const auto colour = media::colourFromQuery(uri("synthetic:x?range=full&colour=bt2020"));
    REQUIRE(colour.has_value());
    CHECK(*colour == media::ColourParams{.matrix = media::ColourMatrix::Bt2020Ncl,
                                         .range = media::ColourRange::Full,
                                         .transfer = media::TransferFn::Bt1886,
                                         .primaries = media::Primaries::Bt2020,
                                         .kr = 0,
                                         .kb = 0});
    CHECK(media::colourFromQuery(uri("synthetic:x")) == media::ColourParams{});

    const auto wide = media::colourFromQuery(uri("synthetic:x?range=wide"));
    REQUIRE_FALSE(wide.has_value());
    CHECK(wide.error().code == dvv::Errc::Format);
    CHECK(wide.error().message == "bad range 'wide' (full, limited)");
}

TEST_CASE("uri params: colour= gives the standard; range= goes with every token") {
    using Parameters = std::vector<std::pair<std::string, std::string>>;
    for (const media::ColourPresetInfo& info : media::colourPresets()) {
        CAPTURE(info.uriName);
        const auto colour =
            media::colourFromQuery(uri("synthetic:x?colour=" + std::string(info.uriName)));
        REQUIRE(colour.has_value());
        // The standard's fields but its range: the range comes only from range= (D5R §1.3).
        media::ColourParams expected = info.colour;
        expected.range = media::ColourRange::Unspecified;
        CHECK(*colour == expected);

        for (const std::string_view range : {"full", "limited"}) {
            CAPTURE(range);
            const std::string query =
                "colour=" + std::string(info.uriName) + "&range=" + std::string(range);
            const auto spec = media::colourSpecFromQuery(uri("synthetic:x?" + query));
            REQUIRE(spec.has_value());
            CHECK(spec->preset == info.id);
            CHECK(media::colourParameters(*spec) ==
                  Parameters{{"colour", std::string(info.uriName)}, {"range", std::string(range)}});
        }
    }

    // The query's form: colour first, then range; range alone stays alone.
    const auto reversed = media::colourSpecFromQuery(uri("synthetic:x?range=full&colour=bt709"));
    REQUIRE(reversed.has_value());
    CHECK(media::colourParameters(*reversed) == Parameters{{"colour", "bt709"}, {"range", "full"}});
    CHECK(media::colourSpecFromQuery(uri("synthetic:x")) == media::ColourSpec{});
    const auto range_only = media::colourSpecFromQuery(uri("synthetic:x?range=limited"));
    REQUIRE(range_only.has_value());
    CHECK(*range_only ==
          media::ColourSpec{.preset = std::nullopt, .range = media::ColourRange::Limited});
    CHECK(media::colourParameters(*range_only) == Parameters{{"range", "limited"}});

    for (const std::string_view bad :
         {"colour=BT709", "colour=", "colour=bt601", "colour=bt2100", "colour=jpeg"}) {
        CAPTURE(bad);
        const auto colour = media::colourSpecFromQuery(uri("synthetic:x?" + std::string(bad)));
        REQUIRE_FALSE(colour.has_value());
        CHECK(colour.error().code == dvv::Errc::Format);
        CHECK(colour.error().message ==
              "bad colour '" + std::string(bad.substr(7)) +
                  "' (bt601-ntsc, bt601-pal, bt709, bt2020, bt2100-pq, bt2100-hlg)");
    }
    // The removed keys are not read here (checkParameters refuses them, below).
    CHECK(media::colourSpecFromQuery(uri("synthetic:x?matrix=bt601&transfer=pq")) ==
          media::ColourSpec{});
}

TEST_CASE("uri params: colourParameters lists the given fields") {
    using Parameters = std::vector<std::pair<std::string, std::string>>;
    const media::ColourParams colour{.matrix = media::ColourMatrix::Custom,
                                     .range = media::ColourRange::Full,
                                     .transfer = media::TransferFn::Pq,
                                     .primaries = media::Primaries::Bt2020,
                                     .kr = 0.212f,
                                     .kb = 0.087f};
    CHECK(media::colourParameters(colour) == Parameters{{"range", "full"},
                                                        {"matrix", "custom"},
                                                        {"kr", "0.212"},
                                                        {"kb", "0.087"},
                                                        {"transfer", "pq"},
                                                        {"primaries", "bt2020"}});
    CHECK(media::colourParameters(media::ColourParams{}).empty());
    CHECK(media::colourParameters({.matrix = media::ColourMatrix::Bt2020Ncl,
                                   .transfer = media::TransferFn::Bt1886,
                                   .primaries = media::Primaries::Bt601_625}) ==
          Parameters{{"matrix", "bt2020"}, {"transfer", "bt1886"}, {"primaries", "bt601-625"}});
}

TEST_CASE("uri params: checkParameters rejects unknown names") {
    constexpr std::array<std::string_view, 1> ALLOWED{"fmt"};
    CHECK(params::checkParameters(uri("synthetic:x?fmt=a&impl=raw"), ALLOWED).has_value());

    const auto unknown = params::checkParameters(uri("synthetic:x?foo=1"), ALLOWED);
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == dvv::Errc::Format);
    CHECK(unknown.error().message == "unknown parameter 'foo'");

    // colour and range are the only colour keys; the removed ones name colour= (D5R §3.1).
    constexpr auto WITH_COLOUR = params::withColourParameters(ALLOWED);
    CHECK(params::checkParameters(uri("synthetic:x?fmt=a&colour=bt709&range=full"), WITH_COLOUR)
              .has_value());
    for (const std::string_view key : {"matrix", "kr", "kb", "transfer", "primaries"}) {
        CAPTURE(key);
        const auto removed = params::checkParameters(
            uri("synthetic:x?colour=bt709&" + std::string(key) + "=1"), WITH_COLOUR);
        REQUIRE_FALSE(removed.has_value());
        CHECK(removed.error().code == dvv::Errc::Format);
        CHECK(removed.error().message == "unknown parameter '" + std::string(key) +
                                             "' (set the colour with colour=<standard>: "
                                             "bt601-ntsc, bt601-pal, bt709, bt2020, bt2100-pq, "
                                             "bt2100-hlg)");
    }
}

TEST_CASE("uri params: sizeFromQuery takes size= or w= and h=") {
    using Size = std::pair<std::uint32_t, std::uint32_t>;
    const auto both = params::sizeFromQuery(uri("synthetic:x?w=3&h=4"));
    REQUIRE(both.has_value());
    CHECK(*both == Size{3, 4});

    const auto size = params::sizeFromQuery(uri("synthetic:x?size=3x4"));
    REQUIRE(size.has_value());
    CHECK(*size == Size{3, 4});

    const auto width_only = params::sizeFromQuery(uri("synthetic:x?w=3"));
    REQUIRE_FALSE(width_only.has_value());
    CHECK(width_only.error().code == dvv::Errc::Format);

    const auto twice = params::sizeFromQuery(uri("synthetic:x?size=3x4&w=3&h=4"));
    REQUIRE_FALSE(twice.has_value());
    CHECK(twice.error().code == dvv::Errc::Format);

    const auto none = params::sizeFromQuery(uri("synthetic:x"));
    REQUIRE(none.has_value());
    CHECK_FALSE(none->has_value());
}
