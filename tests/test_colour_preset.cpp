// The public header comes first: it must compile on its own.
#include "dvv/media/colour_preset.h"

// The test framework and what the cases use.
#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

#include "dvv/media/types.h"

namespace media = dvv::media;
using media::Bt601Hint;
using media::ColourMatrix;
using media::ColourModel;
using media::ColourPreset;
using media::ColourRange;
using media::FileHint;
using media::Primaries;
using media::TransferFn;

namespace {

constexpr media::ColourParams fields(ColourMatrix matrix, TransferFn transfer, Primaries primaries,
                                     ColourRange range = ColourRange::Unspecified) {
    return {.matrix = matrix,
            .range = range,
            .transfer = transfer,
            .primaries = primaries,
            .kr = 0,
            .kb = 0};
}

constexpr FileHint NO_HINT{.bt601 = Bt601Hint::None, .nonStandard = false};

std::optional<ColourPreset> yuvStandard(const media::ColourParams& given, FileHint hint = NO_HINT) {
    return media::standardOf(given, ColourModel::Yuv, hint);
}

}  // namespace

TEST_CASE("colour standards: the table of D5R section 1.1") {
    struct Row {
        ColourPreset id;
        std::string_view uriName;
        std::string_view label;
        media::ColourParams colour;
    };
    constexpr std::array<Row, 6> EXPECTED{{
        {ColourPreset::Bt601Ntsc, "bt601-ntsc", "BT.601 (NTSC)",
         fields(ColourMatrix::Bt601, TransferFn::Bt1886, Primaries::Bt601_525,
                ColourRange::Limited)},
        {ColourPreset::Bt601Pal, "bt601-pal", "BT.601 (PAL)",
         fields(ColourMatrix::Bt601, TransferFn::Bt1886, Primaries::Bt601_625,
                ColourRange::Limited)},
        {ColourPreset::Bt709, "bt709", "BT.709",
         fields(ColourMatrix::Bt709, TransferFn::Bt1886, Primaries::Bt709, ColourRange::Limited)},
        {ColourPreset::Bt2020, "bt2020", "BT.2020",
         fields(ColourMatrix::Bt2020Ncl, TransferFn::Bt1886, Primaries::Bt2020,
                ColourRange::Limited)},
        {ColourPreset::Bt2100Pq, "bt2100-pq", "BT.2100 PQ",
         fields(ColourMatrix::Bt2020Ncl, TransferFn::Pq, Primaries::Bt2020, ColourRange::Limited)},
        {ColourPreset::Bt2100Hlg, "bt2100-hlg", "BT.2100 HLG",
         fields(ColourMatrix::Bt2020Ncl, TransferFn::Hlg, Primaries::Bt2020, ColourRange::Limited)},
    }};
    const auto presets = media::colourPresets();
    REQUIRE(presets.size() == EXPECTED.size());
    for (std::size_t i = 0; i < EXPECTED.size(); ++i) {
        CAPTURE(EXPECTED[i].uriName);
        CHECK(presets[i].id == EXPECTED[i].id);
        CHECK(presets[i].uriName == EXPECTED[i].uriName);
        CHECK(presets[i].label == EXPECTED[i].label);
        CHECK(presets[i].colour == EXPECTED[i].colour);
        CHECK(media::validateColour(presets[i].colour).has_value());
        CHECK(&media::colourPresetInfo(EXPECTED[i].id) == &presets[i]);
    }
}

TEST_CASE("colour standards: find by token and match by value, both ways") {
    for (const media::ColourPresetInfo& info : media::colourPresets()) {
        CAPTURE(info.uriName);
        const std::optional<ColourPreset> found = media::findColourPreset(info.uriName);
        REQUIRE(found.has_value());
        CHECK(found == info.id);
        CHECK(media::matchColourPreset(info.colour) == info.id);
        // REQUIRE above stops the test case first; value_or keeps the access checked.
        CHECK(media::colourPresetInfo(found.value_or(info.id)).colour == info.colour);
    }
    // jpeg and the old BT.601 tokens are gone; there is no bt2100 token without the curve.
    for (const std::string_view bad :
         {"", "BT709", "bt601", "bt2100", "srgb", "jpeg", "bt601-525", "bt601-625"}) {
        CAPTURE(bad);
        CHECK(media::findColourPreset(bad) == std::nullopt);
    }
    CHECK(media::matchColourPreset(fields(ColourMatrix::Bt709, TransferFn::Srgb, Primaries::Bt709,
                                          ColourRange::Limited)) == std::nullopt);
    CHECK(media::matchColourPreset(media::ColourParams{}) == std::nullopt);
}

TEST_CASE("standardOf: nothing signalled gives BT.709") {
    CHECK(yuvStandard(media::ColourParams{}) == ColourPreset::Bt709);
    CHECK(media::standardOf(media::ColourParams{}, ColourModel::Rgb, NO_HINT) ==
          ColourPreset::Bt709);
    CHECK(media::standardOf(media::ColourParams{}, ColourModel::Gray, NO_HINT) ==
          ColourPreset::Bt709);
    // The range is not compared.
    CHECK(yuvStandard(fields(ColourMatrix::Unspecified, TransferFn::Unspecified,
                             Primaries::Unspecified, ColourRange::Full)) == ColourPreset::Bt709);
}

TEST_CASE("standardOf: every table row gives itself") {
    for (const media::ColourPresetInfo& info : media::colourPresets()) {
        CAPTURE(info.uriName);
        CHECK(yuvStandard(info.colour) == info.id);
        CHECK(yuvStandard(info.colour, FileHint{.bt601 = Bt601Hint::Pal, .nonStandard = false}) ==
              info.id);
    }
}

TEST_CASE("standardOf: the curve first, then the matrix, then the primaries") {
    // PQ-only or HLG-only gives BT.2100 (the BT.2020 matrix fills in, not BT.709 by height).
    CHECK(yuvStandard(fields(ColourMatrix::Unspecified, TransferFn::Pq, Primaries::Unspecified)) ==
          ColourPreset::Bt2100Pq);
    CHECK(yuvStandard(fields(ColourMatrix::Unspecified, TransferFn::Hlg, Primaries::Unspecified)) ==
          ColourPreset::Bt2100Hlg);
    CHECK(yuvStandard(fields(ColourMatrix::Bt2020Ncl, TransferFn::Pq, Primaries::Unspecified)) ==
          ColourPreset::Bt2100Pq);
    // By matrix.
    CHECK(yuvStandard(fields(ColourMatrix::Bt709, TransferFn::Unspecified,
                             Primaries::Unspecified)) == ColourPreset::Bt709);
    CHECK(yuvStandard(fields(ColourMatrix::Bt2020Ncl, TransferFn::Bt1886,
                             Primaries::Unspecified)) == ColourPreset::Bt2020);
    // By primaries.
    CHECK(yuvStandard(fields(ColourMatrix::Unspecified, TransferFn::Unspecified,
                             Primaries::Bt2020)) == ColourPreset::Bt2020);
    CHECK(yuvStandard(fields(ColourMatrix::Unspecified, TransferFn::Bt1886, Primaries::Bt709)) ==
          ColourPreset::Bt709);
    CHECK(yuvStandard(fields(ColourMatrix::Unspecified, TransferFn::Unspecified,
                             Primaries::Bt601_525)) == ColourPreset::Bt601Ntsc);
    CHECK(yuvStandard(fields(ColourMatrix::Unspecified, TransferFn::Unspecified,
                             Primaries::Bt601_625)) == ColourPreset::Bt601Pal);
}

TEST_CASE("standardOf: BT.601 picks NTSC or PAL by the primaries, else by the hint") {
    const media::ColourParams bt601 =
        fields(ColourMatrix::Bt601, TransferFn::Unspecified, Primaries::Unspecified);
    CHECK(yuvStandard(bt601) == ColourPreset::Bt601Pal);
    CHECK(yuvStandard(bt601, FileHint{.bt601 = Bt601Hint::Ntsc, .nonStandard = false}) ==
          ColourPreset::Bt601Ntsc);
    CHECK(yuvStandard(bt601, FileHint{.bt601 = Bt601Hint::Pal, .nonStandard = false}) ==
          ColourPreset::Bt601Pal);
    // The primaries win over the (weak) matrix hint.
    CHECK(yuvStandard(fields(ColourMatrix::Bt601, TransferFn::Bt1886, Primaries::Bt601_525),
                      FileHint{.bt601 = Bt601Hint::Pal, .nonStandard = false}) ==
          ColourPreset::Bt601Ntsc);
    // A hint never turns a non-BT.601 colour into BT.601.
    CHECK(yuvStandard(media::ColourParams{},
                      FileHint{.bt601 = Bt601Hint::Ntsc, .nonStandard = false}) ==
          ColourPreset::Bt709);
}

TEST_CASE("standardOf: fields that fit no standard give no standard") {
    // sRGB and linear curves belong to no standard.
    CHECK(yuvStandard(fields(ColourMatrix::Unspecified, TransferFn::Srgb,
                             Primaries::Unspecified)) == std::nullopt);
    CHECK(yuvStandard(fields(ColourMatrix::Bt709, TransferFn::Linear, Primaries::Bt709)) ==
          std::nullopt);
    // Mixed standards.
    CHECK(yuvStandard(fields(ColourMatrix::Bt601, TransferFn::Unspecified, Primaries::Bt2020)) ==
          std::nullopt);
    CHECK(yuvStandard(fields(ColourMatrix::Bt601, TransferFn::Bt1886, Primaries::Bt709)) ==
          std::nullopt);
    CHECK(yuvStandard(fields(ColourMatrix::Bt709, TransferFn::Pq, Primaries::Unspecified)) ==
          std::nullopt);
    CHECK(yuvStandard(fields(ColourMatrix::Unspecified, TransferFn::Hlg, Primaries::Bt709)) ==
          std::nullopt);
    CHECK(yuvStandard(fields(ColourMatrix::Custom, TransferFn::Bt1886, Primaries::Bt709)) ==
          std::nullopt);
    // Signalled but outside our fields (e.g. SMPTE 240M): no standard, even when nothing else is.
    CHECK(yuvStandard(media::ColourParams{},
                      FileHint{.bt601 = Bt601Hint::None, .nonStandard = true}) == std::nullopt);
}

TEST_CASE("standardOf: RGB, gray and the GBR matrix compare only curve and primaries") {
    const media::ColourParams gbr =
        fields(ColourMatrix::Rgb, TransferFn::Bt1886, Primaries::Bt2020);
    CHECK(yuvStandard(gbr) == ColourPreset::Bt2020);
    CHECK(media::standardOf(gbr, ColourModel::Rgb, NO_HINT) == ColourPreset::Bt2020);
    CHECK(media::standardOf(fields(ColourMatrix::Bt601, TransferFn::Pq, Primaries::Unspecified),
                            ColourModel::Gray, NO_HINT) == ColourPreset::Bt2100Pq);
    CHECK(media::standardOf(fields(ColourMatrix::Rgb, TransferFn::Srgb, Primaries::Bt709),
                            ColourModel::Rgb, NO_HINT) == std::nullopt);
}

TEST_CASE("colour standards: the range helpers (U-D5-9)") {
    CHECK(media::defaultRange(ColourModel::Yuv) == ColourRange::Limited);
    CHECK(media::defaultRange(ColourModel::Rgb) == ColourRange::Full);
    CHECK(media::defaultRange(ColourModel::Gray) == ColourRange::Full);
    for (const ColourPreset sdr : {ColourPreset::Bt601Ntsc, ColourPreset::Bt601Pal,
                                   ColourPreset::Bt709, ColourPreset::Bt2020}) {
        CHECK(media::rangeNotStandard(sdr, ColourModel::Yuv, ColourRange::Full));
        CHECK_FALSE(media::rangeNotStandard(sdr, ColourModel::Yuv, ColourRange::Limited));
        CHECK_FALSE(media::rangeNotStandard(sdr, ColourModel::Rgb, ColourRange::Full));
    }
    CHECK_FALSE(
        media::rangeNotStandard(ColourPreset::Bt2100Pq, ColourModel::Yuv, ColourRange::Full));
    CHECK_FALSE(
        media::rangeNotStandard(ColourPreset::Bt2100Hlg, ColourModel::Yuv, ColourRange::Full));
}

TEST_CASE("colour standards: the size guess and the size hint (U-D5-8)") {
    CHECK(media::guessStandard(480) == ColourPreset::Bt601Ntsc);
    CHECK(media::guessStandard(486) == ColourPreset::Bt601Pal);
    CHECK(media::guessStandard(576) == ColourPreset::Bt601Pal);
    CHECK(media::guessStandard(577) == ColourPreset::Bt709);
    CHECK(media::guessStandard(1080) == ColourPreset::Bt709);
    CHECK(media::sizeHint(ColourPreset::Bt709, 576) == "SD size: BT.601 is common for SD");
    CHECK(media::sizeHint(ColourPreset::Bt2100Pq, 480) == "SD size: BT.601 is common for SD");
    CHECK(media::sizeHint(ColourPreset::Bt709, 577).empty());
    CHECK(media::sizeHint(ColourPreset::Bt601Pal, 1080) == "HD size: BT.709 is common");
    CHECK(media::sizeHint(ColourPreset::Bt601Ntsc, 576).empty());
}

TEST_CASE("colour standards: standardWeights gives Kr and Kb of the standard matrices") {
    CHECK(media::standardWeights(ColourMatrix::Bt601) == std::array{0.299f, 0.114f});
    CHECK(media::standardWeights(ColourMatrix::Bt709) == std::array{0.2126f, 0.0722f});
    CHECK(media::standardWeights(ColourMatrix::Bt2020Ncl) == std::array{0.2627f, 0.0593f});
    CHECK(media::standardWeights(ColourMatrix::Rgb) == std::nullopt);
    CHECK(media::standardWeights(ColourMatrix::Custom) == std::nullopt);
    CHECK(media::standardWeights(ColourMatrix::Unspecified) == std::nullopt);
}
