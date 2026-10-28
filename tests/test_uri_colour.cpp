// The public header comes first: it must compile on its own.
#include "dvv/media/uri_colour.h"

// The test framework and what the cases use.
#include <doctest/doctest.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/uri.h"
#include "dvv/media/colour_preset.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"

namespace media = dvv::media;

TEST_CASE("uri_colour.h: the public colour query") {
    using Parameters = std::vector<std::pair<std::string, std::string>>;
    CHECK(media::COLOUR_PARAMETERS == std::array<std::string_view, 2>{"colour", "range"});

    const auto uri = dvv::parseUri("file:a.mkv?colour=bt601-pal&range=full");
    REQUIRE(uri.has_value());
    const auto colour = media::colourFromQuery(*uri);
    REQUIRE(colour.has_value());
    CHECK(media::colourParameters(*colour) == Parameters{{"range", "full"},
                                                         {"matrix", "bt601"},
                                                         {"transfer", "bt1886"},
                                                         {"primaries", "bt601-625"}});

    const auto wide = dvv::parseUri("file:a.mkv?range=wide");
    REQUIRE(wide.has_value());
    const auto bad = media::colourFromQuery(*wide);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == dvv::Errc::Format);
}

TEST_CASE("uri_colour.h: unknownParameterText names colour= for the removed keys") {
    for (const std::string_view key : {"matrix", "kr", "kb", "transfer", "primaries"}) {
        CHECK(media::unknownParameterText(key) ==
              std::string("unknown parameter '") + std::string(key) +
                  "' (set the colour with colour=<standard>: bt601-ntsc, bt601-pal, bt709, "
                  "bt2020, bt2100-pq, bt2100-hlg)");
    }
    CHECK(media::unknownParameterText("gamma") == "unknown parameter 'gamma'");
    CHECK(media::unknownParameterText("colour") == "unknown parameter 'colour'");
}

TEST_CASE("uri_colour.h: colour= keeps the query's form") {
    using Parameters = std::vector<std::pair<std::string, std::string>>;
    const auto uri = dvv::parseUri("file:a.mkv?range=full&colour=bt709");
    REQUIRE(uri.has_value());
    const auto spec = media::colourSpecFromQuery(*uri);
    REQUIRE(spec.has_value());
    CHECK(*spec == media::ColourSpec{.preset = media::ColourPreset::Bt709,
                                     .range = media::ColourRange::Full});
    CHECK(media::merged(*spec) == media::ColourParams{.matrix = media::ColourMatrix::Bt709,
                                                      .range = media::ColourRange::Full,
                                                      .transfer = media::TransferFn::Bt1886,
                                                      .primaries = media::Primaries::Bt709,
                                                      .kr = 0,
                                                      .kb = 0});
    CHECK(media::colourFromQuery(*uri) == media::merged(*spec));
    CHECK(media::colourParameters(*spec) == Parameters{{"colour", "bt709"}, {"range", "full"}});
    CHECK(media::colourParameters(media::ColourSpec{}).empty());
    CHECK(media::colourParameters(
              media::ColourSpec{.preset = std::nullopt, .range = media::ColourRange::Limited}) ==
          Parameters{{"range", "limited"}});

    // The exported name tables.
    CHECK(media::colourName(media::ColourMatrix::Bt2020Ncl) == "bt2020");
    CHECK(media::colourName(media::ColourRange::Limited) == "limited");
    CHECK(media::colourName(media::TransferFn::Bt1886) == "bt1886");
    CHECK(media::colourName(media::Primaries::Bt601_525) == "bt601-525");
    CHECK(media::colourName(media::Primaries::Unspecified).empty());
    CHECK(media::parseColourMatrix("rgb") == media::ColourMatrix::Rgb);
    CHECK(media::parseColourRange("full") == media::ColourRange::Full);
    CHECK(media::parseTransferFn("hlg") == media::TransferFn::Hlg);
    CHECK(media::parsePrimaries("bt2020") == media::Primaries::Bt2020);
    const auto wrong = media::parsePrimaries("p3");
    REQUIRE_FALSE(wrong.has_value());
    CHECK(wrong.error().message == "bad primaries 'p3' (bt601-525, bt601-625, bt709, bt2020)");
}

TEST_CASE("uri_colour.h: merged() never takes the range from the standard (D5R §1.3)") {
    using media::ColourMatrix;
    using media::ColourModel;
    using media::ColourRange;
    using media::Primaries;
    using media::TransferFn;
    const auto spec = [](std::optional<media::ColourPreset> preset, ColourRange range) {
        return media::ColourSpec{.preset = preset, .range = range};
    };
    const auto weights = [](media::ColourPreset preset) {
        const media::ColourParams& colour = media::colourPresetInfo(preset).colour;
        return std::pair{colour.kr, colour.kb};
    };

    // YUV: matrix, transfer and primaries from the standard; the range stays Unspecified.
    const media::ColourSpec bt2020 = spec(media::ColourPreset::Bt2020, ColourRange::Unspecified);
    CHECK(media::merged(bt2020, ColourModel::Yuv) == media::merged(bt2020));
    CHECK(media::merged(bt2020) ==
          media::ColourParams{.matrix = ColourMatrix::Bt2020Ncl,
                              .range = ColourRange::Unspecified,
                              .transfer = TransferFn::Bt1886,
                              .primaries = Primaries::Bt2020,
                              .kr = weights(media::ColourPreset::Bt2020).first,
                              .kb = weights(media::ColourPreset::Bt2020).second});
    // RGB and gray: no matrix either.
    const media::ColourParams flat{.matrix = ColourMatrix::Unspecified,
                                   .range = ColourRange::Unspecified,
                                   .transfer = TransferFn::Bt1886,
                                   .primaries = Primaries::Bt2020,
                                   .kr = 0,
                                   .kb = 0};
    CHECK(media::merged(bt2020, ColourModel::Rgb) == flat);
    CHECK(media::merged(bt2020, ColourModel::Gray) == flat);

    // range= is taken with every standard and every model (U-D5-9).
    for (const media::ColourPresetInfo& info : media::colourPresets()) {
        INFO(std::string(info.uriName));
        for (const ColourModel model : {ColourModel::Yuv, ColourModel::Rgb, ColourModel::Gray}) {
            CHECK(media::merged(spec(info.id, ColourRange::Full), model).range ==
                  ColourRange::Full);
            CHECK(media::merged(spec(info.id, ColourRange::Limited), model).range ==
                  ColourRange::Limited);
        }
    }
    const media::ColourSpec hlg_full = spec(media::ColourPreset::Bt2100Hlg, ColourRange::Full);
    CHECK(media::merged(hlg_full).transfer == TransferFn::Hlg);
    CHECK(media::merged(hlg_full).matrix == ColourMatrix::Bt2020Ncl);

    // No standard: only the range.
    CHECK(media::merged(spec(std::nullopt, ColourRange::Limited), ColourModel::Rgb) ==
          media::ColourParams{.matrix = ColourMatrix::Unspecified,
                              .range = ColourRange::Limited,
                              .transfer = TransferFn::Unspecified,
                              .primaries = Primaries::Unspecified,
                              .kr = 0,
                              .kb = 0});
    CHECK(media::merged(media::ColourSpec{}, ColourModel::Gray) == media::ColourParams{});
}
