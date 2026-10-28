#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "dvv/base/error.h"
#include "dvv/gpu/device_id.h"
#include "dvv/media/colour_preset.h"
#include "dvv/media/display.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"

namespace media = dvv::media;

using media::ColourMatrix;
using media::ColourParams;
using media::ColourRange;
using media::Primaries;
using media::TransferFn;

namespace {

media::FormatId idOf(std::string_view name) {
    const auto id = media::findFormat(name);
    REQUIRE_MESSAGE(id.has_value(), std::string(name));
    return id.value_or(media::FormatId{0});
}

void checkNear(double actual, double expected, double tolerance) {
    CHECK_MESSAGE(std::abs(actual - expected) <= tolerance, "actual ", actual, ", expected ",
                  expected, " +- ", tolerance);
}

void checkRgb(const std::array<float, 3>& actual, const std::array<double, 3>& expected,
              double tolerance) {
    for (std::size_t i = 0; i < 3; ++i) {
        CAPTURE(i);
        checkNear(actual[i], expected[i], tolerance);
    }
}

// matrix * (c0, c1, c2) + offset, without the clamp.
std::array<float, 3> applyAffine(const media::DisplayAffine& affine, float c0, float c1, float c2) {
    std::array<float, 3> result{};
    for (std::size_t r = 0; r < 3; ++r) {
        result[r] = affine.matrix[3 * r] * c0 + affine.matrix[3 * r + 1] * c1 +
                    affine.matrix[3 * r + 2] * c2 + affine.offset[r];
    }
    return result;
}

std::array<float, 3> rgbOf(const std::array<float, 4>& rgba) {
    return {rgba[0], rgba[1], rgba[2]};
}

media::FrameDesc frame(std::string_view format, std::uint32_t width, std::uint32_t height,
                       const ColourParams& colour = {}) {
    return media::FrameDesc{.format = idOf(format),
                            .colour = media::internColour(colour),
                            .width = width,
                            .height = height};
}

void checkFormatError(const dvv::Result<void>& result) {
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == dvv::Errc::Format);
}

}  // namespace

TEST_SUITE("display") {
    TEST_CASE("display format and colour") {
        const media::FormatId format = media::displayFormat();
        const media::PixelFormatDescriptor& d = media::descriptor(format);
        CHECK(d.name == "rgba16f@display");
        CHECK(d.model == media::ColourModel::Rgb);
        REQUIRE(d.componentCount == 4);
        for (std::size_t c = 0; c < 4; ++c) {
            CHECK(d.components[c].bits == 16);
        }
        CHECK(d.planeCount == 1);
        CHECK(media::isDisplay(format));
        CHECK_FALSE(media::isDisplay(idOf("rgba64le")));
        const auto frame_geometry = media::geometry(format, 33, 17);
        REQUIRE(frame_geometry.has_value());
        CHECK(frame_geometry->bytes == std::uint64_t{33} * 17 * 8);
        CHECK(media::colourParams(media::displayColour()) ==
              ColourParams{.matrix = ColourMatrix::Rgb,
                           .range = ColourRange::Full,
                           .transfer = TransferFn::Linear,
                           .primaries = Primaries::Bt709});
        const media::Domain domain{
            .kind = media::MemKind::DeviceLocal, .device = dvv::gpu::DeviceId{0}, .generation = 1};
        CHECK(media::displayView(domain) == media::ViewSpec{.format = format,
                                                            .colour = media::displayColour(),
                                                            .layout = media::LayoutPolicy::Any,
                                                            .api = media::Api::VkImage,
                                                            .domain = domain,
                                                            .downscaleLog2 = 0});
        CHECK(media::unpackedFormat(format) == format);
        CHECK_FALSE(media::isUnpacked(format));
        CHECK(media::displayFormat() == format);
        CHECK(media::displayColour() == media::displayColour());
    }

    TEST_CASE("validateColour") {
        const ColourParams custom{.matrix = ColourMatrix::Custom,
                                  .range = ColourRange::Limited,
                                  .kr = 0.212f,
                                  .kb = 0.087f};
        CHECK(media::validateColour(custom).has_value());
        CHECK(media::validateColour(ColourParams{}).has_value());

        ColourParams bad = custom;
        bad.kr = 0;
        checkFormatError(media::validateColour(bad));
        bad = custom;
        bad.kb = -0.1f;
        checkFormatError(media::validateColour(bad));
        bad = custom;
        bad.kr = 0.6f;
        bad.kb = 0.5f;
        checkFormatError(media::validateColour(bad));
        bad = custom;
        bad.kr = std::numeric_limits<float>::quiet_NaN();
        checkFormatError(media::validateColour(bad));

        const auto stray =
            media::validateColour(ColourParams{.matrix = ColourMatrix::Bt709, .kr = 0.2f});
        checkFormatError(stray);
        if (!stray) {
            CHECK(stray.error().message == "kr and kb apply only to the custom matrix");
        }
        const auto sum = media::validateColour(
            ColourParams{.matrix = ColourMatrix::Custom, .kr = 0.6f, .kb = 0.5f});
        REQUIRE_FALSE(sum.has_value());
        CHECK(sum.error().message ==
              "custom matrix needs kr > 0, kb > 0 and kr + kb < 1 (kr=0.6, kb=0.5)");
    }

    TEST_CASE("custom colours intern by value") {
        const ColourParams custom{.matrix = ColourMatrix::Custom,
                                  .range = ColourRange::Limited,
                                  .kr = 0.212f,
                                  .kb = 0.087f};
        const media::ColourId id = media::internColour(custom);
        CHECK(media::internColour(custom) == id);
        ColourParams other = custom;
        other.kb = 0.088f;
        CHECK(media::internColour(other) != id);
        CHECK(media::colourParams(id).kr == 0.212f);
        CHECK(media::colourParams(id).kb == 0.087f);
    }

    TEST_CASE("resolveColour defaults: the BT.709 standard at any size (D5R section 1.3)") {
        const media::ResolvedColour bt709_default{.params = {.matrix = ColourMatrix::Bt709,
                                                             .range = ColourRange::Limited,
                                                             .transfer = TransferFn::Bt1886,
                                                             .primaries = Primaries::Bt709,
                                                             .kr = 0,
                                                             .kb = 0},
                                                  .assumed = 15,
                                                  .standard = media::ColourPreset::Bt709};
        CHECK(media::resolveColour(frame("yuv420p", 1920, 1080)) == bt709_default);
        // No height rule: SD is BT.709 too.
        CHECK(media::resolveColour(frame("yuv420p", 720, 576)) == bt709_default);
        CHECK(media::resolveColour(frame("yuv420p", 352, 288)) == bt709_default);

        // RGB and gray: BT.1886 too, full range by default.
        const media::ResolvedColour rgb_default{
            .params = {.matrix = ColourMatrix::Rgb,
                       .range = ColourRange::Full,
                       .transfer = TransferFn::Bt1886,
                       .primaries = Primaries::Bt709,
                       .kr = 0,
                       .kb = 0},
            .assumed = media::AssumedRange | media::AssumedTransfer | media::AssumedPrimaries,
            .standard = media::ColourPreset::Bt709};
        CHECK(media::resolveColour(frame("rgb24", 4, 2)) == rgb_default);
        CHECK(media::resolveColour(frame("gray", 4, 2)) == rgb_default);

        const ColourParams given{.matrix = ColourMatrix::Bt2020Ncl,
                                 .range = ColourRange::Full,
                                 .transfer = TransferFn::Pq,
                                 .primaries = Primaries::Bt2020,
                                 .kr = 0,
                                 .kb = 0};
        CHECK(media::resolveColour(frame("yuv420p", 1920, 1080, given)) ==
              media::ResolvedColour{
                  .params = given, .assumed = 0, .standard = media::ColourPreset::Bt2100Pq});
    }

    TEST_CASE("resolveColour: the given fields point to a standard that fills the rest") {
        // PQ only: BT.2100 PQ, so the BT.2020 matrix and primaries, not BT.709.
        const ColourParams pq_only{.matrix = ColourMatrix::Unspecified,
                                   .range = ColourRange::Unspecified,
                                   .transfer = TransferFn::Pq,
                                   .primaries = Primaries::Unspecified,
                                   .kr = 0,
                                   .kb = 0};
        CHECK(media::resolveColour(frame("yuv420p10le", 1920, 1080, pq_only)) ==
              media::ResolvedColour{
                  .params = {.matrix = ColourMatrix::Bt2020Ncl,
                             .range = ColourRange::Limited,
                             .transfer = TransferFn::Pq,
                             .primaries = Primaries::Bt2020,
                             .kr = 0,
                             .kb = 0},
                  .assumed = media::AssumedMatrix | media::AssumedRange | media::AssumedPrimaries,
                  .standard = media::ColourPreset::Bt2100Pq});
        // BT.601 matrix only: PAL by default, NTSC with the hint; the curve is BT.1886.
        const ColourParams bt601{.matrix = ColourMatrix::Bt601,
                                 .range = ColourRange::Unspecified,
                                 .transfer = TransferFn::Unspecified,
                                 .primaries = Primaries::Unspecified,
                                 .kr = 0,
                                 .kb = 0};
        const auto pal = media::resolveColour(frame("yuv420p", 720, 576, bt601));
        CHECK(pal.standard == media::ColourPreset::Bt601Pal);
        CHECK(pal.params.primaries == Primaries::Bt601_625);
        CHECK(pal.params.transfer == TransferFn::Bt1886);
        const auto ntsc = media::resolveColour(
            frame("yuv420p", 720, 576, bt601),
            media::FileHint{.bt601 = media::Bt601Hint::Ntsc, .nonStandard = false});
        CHECK(ntsc.standard == media::ColourPreset::Bt601Ntsc);
        CHECK(ntsc.params.primaries == Primaries::Bt601_525);
    }

    TEST_CASE("resolveColour: no standard keeps the given fields and fills the rest from BT.709") {
        const ColourParams srgb{.matrix = ColourMatrix::Bt601,
                                .range = ColourRange::Unspecified,
                                .transfer = TransferFn::Srgb,
                                .primaries = Primaries::Unspecified,
                                .kr = 0,
                                .kb = 0};
        CHECK(media::resolveColour(frame("yuv420p", 720, 576, srgb)) ==
              media::ResolvedColour{.params = {.matrix = ColourMatrix::Bt601,
                                               .range = ColourRange::Limited,
                                               .transfer = TransferFn::Srgb,
                                               .primaries = Primaries::Bt709,
                                               .kr = 0,
                                               .kb = 0},
                                    .assumed = media::AssumedRange | media::AssumedPrimaries,
                                    .standard = std::nullopt});
        const auto odd = media::resolveColour(
            frame("yuv420p", 1920, 1080),
            media::FileHint{.bt601 = media::Bt601Hint::None, .nonStandard = true});
        CHECK(odd.standard == std::nullopt);
        CHECK(odd.params.matrix == ColourMatrix::Bt709);
        CHECK(odd.params.transfer == TransferFn::Bt1886);
    }

    TEST_CASE("resolveColour: the file's range wins over the standard's (all formats)") {
        for (const ColourRange range : {ColourRange::Full, ColourRange::Limited}) {
            const ColourParams given{.matrix = ColourMatrix::Bt709,
                                     .range = range,
                                     .transfer = TransferFn::Bt1886,
                                     .primaries = Primaries::Bt709,
                                     .kr = 0,
                                     .kb = 0};
            for (const std::string_view format : {"yuv420p", "rgb24", "gray"}) {
                CAPTURE(format);
                const auto resolved = media::resolveColour(frame(format, 720, 576, given));
                CHECK(resolved.params.range == range);
                CHECK((resolved.assumed & media::AssumedRange) == 0);
                CHECK(resolved.standard == media::ColourPreset::Bt709);
            }
        }

        const ColourParams custom{.matrix = ColourMatrix::Custom,
                                  .range = ColourRange::Limited,
                                  .kr = 0.212f,
                                  .kb = 0.087f};
        const auto resolved = media::resolveColour(frame("yuv420p", 1920, 1080, custom));
        CHECK(resolved.params.matrix == ColourMatrix::Custom);
        CHECK(resolved.params.kr == 0.212f);
        CHECK(resolved.params.kb == 0.087f);
        CHECK(resolved.assumed == (media::AssumedTransfer | media::AssumedPrimaries));
    }

    TEST_CASE("displayAffine per model and order") {
        const ColourParams full{.matrix = ColourMatrix::Rgb, .range = ColourRange::Full};
        const ColourParams limited{.matrix = ColourMatrix::Rgb, .range = ColourRange::Limited};

        const media::DisplayAffine gray = media::displayAffine(idOf("gray"), full);
        for (std::size_t r = 0; r < 3; ++r) {
            CAPTURE(r);
            CHECK(gray.matrix[3 * r] == 1.0f / 255.0f);
            CHECK(gray.matrix[3 * r + 1] == 0.0f);
            CHECK(gray.matrix[3 * r + 2] == 0.0f);
            CHECK(gray.offset[r] == 0.0f);
        }
        const media::DisplayAffine gray10 = media::displayAffine(idOf("gray10le"), limited);
        for (std::size_t r = 0; r < 3; ++r) {
            CAPTURE(r);
            checkNear(gray10.matrix[3 * r], 1.0 / 876.0, 1e-7);
            checkNear(gray10.offset[r], -64.0 / 876.0, 1e-7);
        }

        const media::DisplayAffine rgb24 = media::displayAffine(idOf("rgb24"), full);
        for (std::size_t r = 0; r < 3; ++r) {
            for (std::size_t c = 0; c < 3; ++c) {
                CAPTURE(r);
                CAPTURE(c);
                CHECK(rgb24.matrix[3 * r + c] == (r == c ? 1.0f / 255.0f : 0.0f));
            }
            CHECK(rgb24.offset[r] == 0.0f);
        }
        for (const std::string_view name : {"bgr24", "gbrp", "rgba", "argb"}) {
            CAPTURE(name);
            const media::DisplayAffine other = media::displayAffine(idOf(name), full);
            CHECK(other.matrix == rgb24.matrix);
            CHECK(other.offset == rgb24.offset);
        }
        const media::DisplayAffine x2rgb = media::displayAffine(idOf("x2rgb10le"), full);
        for (std::size_t r = 0; r < 3; ++r) {
            for (std::size_t c = 0; c < 3; ++c) {
                CHECK(x2rgb.matrix[3 * r + c] == (r == c ? 1.0f / 1023.0f : 0.0f));
            }
        }

        const media::DisplayAffine yuv709 = media::displayAffine(
            idOf("yuv444p"), {.matrix = ColourMatrix::Bt709, .range = ColourRange::Full});
        checkNear(yuv709.matrix[2], 2.0 * (1.0 - 0.2126) / 255.0, 1e-6);
        checkNear(yuv709.offset[0], -2.0 * (1.0 - 0.2126) * 128.0 / 255.0, 1e-6);

        const media::DisplayAffine gbr_in_yuv = media::displayAffine(
            idOf("yuv444p"), {.matrix = ColourMatrix::Rgb, .range = ColourRange::Limited});
        const std::array<std::array<double, 3>, 3> rows{
            {{0, 0, 1.0 / 219.0}, {1.0 / 219.0, 0, 0}, {0, 1.0 / 219.0, 0}}};
        for (std::size_t r = 0; r < 3; ++r) {
            CAPTURE(r);
            for (std::size_t c = 0; c < 3; ++c) {
                CAPTURE(c);
                checkNear(gbr_in_yuv.matrix[3 * r + c], rows[r][c], 1e-7);
            }
            checkNear(gbr_in_yuv.offset[r], -16.0 / 219.0, 1e-7);
        }
    }

    TEST_CASE("known values before the EOTF") {
        const ColourParams bt709_limited{.matrix = ColourMatrix::Bt709,
                                         .range = ColourRange::Limited};
        const media::DisplayAffine ten = media::displayAffine(idOf("yuv444p10le"), bt709_limited);
        checkRgb(applyAffine(ten, 940, 512, 512), {1, 1, 1}, 1e-5);
        checkRgb(applyAffine(ten, 64, 512, 512), {0, 0, 0}, 1e-5);

        const media::DisplayAffine bars = media::displayAffine(
            idOf("yuv444p"), {.matrix = ColourMatrix::Bt601, .range = ColourRange::Limited});
        struct Bar {
            std::string_view name;
            std::array<float, 3> code;
            std::array<double, 3> rgb;
        };
        const std::array<Bar, 7> cases{{
            {"white", {180, 128, 128}, {0.75, 0.75, 0.75}},
            {"yellow", {162, 44, 142}, {0.75, 0.75, 0}},
            {"cyan", {131, 156, 44}, {0, 0.75, 0.75}},
            {"green", {112, 72, 58}, {0, 0.75, 0}},
            {"magenta", {84, 184, 198}, {0.75, 0, 0.75}},
            {"red", {65, 100, 212}, {0.75, 0, 0}},
            {"blue", {35, 212, 114}, {0, 0, 0.75}},
        }};
        for (const Bar& bar : cases) {
            CAPTURE(bar.name);
            checkRgb(applyAffine(bars, bar.code[0], bar.code[1], bar.code[2]), bar.rgb, 0.005);
        }

        const media::DisplayAffine smpte240 =
            media::displayAffine(idOf("yuv444p"), {.matrix = ColourMatrix::Custom,
                                                   .range = ColourRange::Limited,
                                                   .kr = 0.212f,
                                                   .kb = 0.087f});
        checkRgb(applyAffine(smpte240, 120, 110, 150), {0.629672, 0.446285, 0.328154}, 1e-4);
    }

    TEST_CASE("BT.709 and BT.2020 matrix weights") {
        struct Known {
            std::string_view name;
            std::array<float, 3> code;
            std::array<double, 3> rgb;
        };
        // 75 % bars, BT.709 limited 8-bit: the chroma terms need both Kr and Kb.
        const media::DisplayAffine bt709 = media::displayAffine(
            idOf("yuv444p"), {.matrix = ColourMatrix::Bt709, .range = ColourRange::Limited});
        const std::array<Known, 6> bars{{
            {"yellow", {168, 44, 136}, {0.75, 0.75, 0}},
            {"cyan", {145, 147, 44}, {0, 0.75, 0.75}},
            {"green", {134, 63, 52}, {0, 0.75, 0}},
            {"magenta", {63, 193, 204}, {0.75, 0, 0.75}},
            {"red", {51, 109, 212}, {0.75, 0, 0}},
            {"blue", {28, 212, 120}, {0, 0, 0.75}},
        }};
        for (const Known& bar : bars) {
            CAPTURE(bar.name);
            checkRgb(applyAffine(bt709, bar.code[0], bar.code[1], bar.code[2]), bar.rgb, 0.005);
        }
        // BT.2020 NCL limited 10-bit primaries at 75 %. The 10-bit codes are within 0.0005 of the
        // targets, so 0.001 also catches a Kr typo (0.2672 moves red by 0.003).
        const media::DisplayAffine bt2020 = media::displayAffine(
            idOf("yuv444p10le"),
            {.matrix = ColourMatrix::Bt2020Ncl, .range = ColourRange::Limited});
        const std::array<Known, 3> primaries{{
            {"red", {237, 418, 848}, {0.75, 0, 0}},
            {"green", {509, 270, 203}, {0, 0.75, 0}},
            {"blue", {103, 848, 485}, {0, 0, 0.75}},
        }};
        for (const Known& primary : primaries) {
            CAPTURE(primary.name);
            checkRgb(applyAffine(bt2020, primary.code[0], primary.code[1], primary.code[2]),
                     primary.rgb, 0.001);
        }
    }

    TEST_CASE("neutral sRGB round trip") {
        const media::FormatId gray = idOf("gray");
        const ColourParams srgb{.matrix = ColourMatrix::Rgb,
                                .range = ColourRange::Full,
                                .transfer = TransferFn::Srgb,
                                .primaries = Primaries::Bt709};
        for (std::uint16_t v = 0; v <= 255; ++v) {
            CAPTURE(v);
            const std::array<std::uint16_t, 1> code{v};
            CHECK(media::encodeSrgb8(media::toDisplayReference(gray, srgb, code)[0]) == v);
        }
        const std::array<std::uint16_t, 3> pixel{10, 200, 77};
        CHECK(media::toDisplayReference(idOf("gbrp"), srgb, pixel) ==
              media::toDisplayReference(idOf("rgb24"), srgb, pixel));
    }

    TEST_CASE("PQ and HLG tone mapping") {
        const media::FormatId gray16 = idOf("gray16le");
        const auto linear = [gray16](TransferFn transfer, std::uint16_t code) {
            const ColourParams colour{.matrix = ColourMatrix::Rgb,
                                      .range = ColourRange::Full,
                                      .transfer = transfer,
                                      .primaries = Primaries::Bt709};
            const std::array<std::uint16_t, 1> values{code};
            const std::array<float, 4> rgba = media::toDisplayReference(gray16, colour, values);
            CHECK(rgba[0] == rgba[1]);
            CHECK(rgba[0] == rgba[2]);
            CHECK(rgba[3] == 1.0f);
            return rgba[0];
        };
        const auto pq_code = [](double signal) {
            return static_cast<std::uint16_t>(std::lround(signal * 65535.0));
        };
        checkNear(linear(TransferFn::Pq, pq_code(0.299699)), 0.100000, 1e-3);
        checkNear(linear(TransferFn::Pq, pq_code(0.508078)), 0.694544, 1e-3);
        checkNear(linear(TransferFn::Pq, pq_code(0.751827)), 1.000000, 1e-3);
        checkNear(linear(TransferFn::Pq, pq_code(0.902572)), 1.000000, 1e-3);
        checkNear(linear(TransferFn::Hlg, 32768), 0.466106, 1e-3);
        checkNear(linear(TransferFn::Hlg, 65535), 1.000000, 1e-3);
        checkNear(linear(TransferFn::Hlg, 16384), 0.096053, 1e-3);

        float previous = -1.0f;
        for (std::uint32_t code = 0; code <= 65000; code += 1000) {
            CAPTURE(code);
            const float value = linear(TransferFn::Pq, static_cast<std::uint16_t>(code));
            CHECK(value >= previous);
            previous = value;
        }
    }

    TEST_CASE("primaries and alpha") {
        const media::FormatId rgb24 = idOf("rgb24");
        const ColourParams bt2020{.matrix = ColourMatrix::Rgb,
                                  .range = ColourRange::Full,
                                  .transfer = TransferFn::Linear,
                                  .primaries = Primaries::Bt2020};
        const auto convert = [&](std::uint16_t r, std::uint16_t g, std::uint16_t b) {
            const std::array<std::uint16_t, 3> pixel{r, g, b};
            const std::array<float, 4> rgba = media::toDisplayReference(rgb24, bt2020, pixel);
            CHECK(rgba[3] == 1.0f);
            return rgbOf(rgba);
        };
        // The primaries are information only (D5R section 2.1): BT.2020 gives the BT.709 output.
        checkRgb(convert(0, 255, 0), {0, 1, 0}, 1e-6);
        checkRgb(convert(64, 128, 32), {64.0 / 255.0, 128.0 / 255.0, 32.0 / 255.0}, 1e-6);
        checkRgb(convert(90, 90, 90), {90.0 / 255.0, 90.0 / 255.0, 90.0 / 255.0}, 1e-5);
        const ColourParams bt709{.matrix = ColourMatrix::Rgb,
                                 .range = ColourRange::Full,
                                 .transfer = TransferFn::Linear,
                                 .primaries = Primaries::Bt709,
                                 .kr = 0,
                                 .kb = 0};
        const std::array<std::uint16_t, 3> pixel{200, 17, 99};
        CHECK(media::toDisplayReference(rgb24, bt2020, pixel) ==
              media::toDisplayReference(rgb24, bt709, pixel));
    }

    TEST_CASE("sRGB helpers") {
        CHECK(media::srgbEncode(0.0f) == 0.0f);
        checkNear(media::srgbEncode(1.0f), 1.0, 1e-6);
        CHECK(media::srgbEncode(-1.0f) == 0.0f);
        checkNear(media::srgbEncode(2.0f), 1.0, 1e-6);
        for (int step = 0; step <= 1000; ++step) {
            const float x = static_cast<float>(step) * 0.001f;
            CAPTURE(x);
            checkNear(media::srgbDecode(media::srgbEncode(x)), x, 1e-6);
        }
        struct Case {
            float linear;
            std::uint8_t encoded;
        };
        for (const Case entry :
             {Case{0.5f, 188}, Case{0.18f, 118}, Case{0.214f, 127}, Case{0.05f, 63},
              Case{0.9f, 243}, Case{0.002f, 7}, Case{0.0f, 0}, Case{1.0f, 255}}) {
            CAPTURE(entry.linear);
            CHECK(media::encodeSrgb8(entry.linear) == entry.encoded);
        }
    }

    TEST_CASE("half floats") {
        CHECK(media::floatToHalf(1.0f) == 0x3C00);
        CHECK(media::floatToHalf(0.5f) == 0x3800);
        CHECK(media::floatToHalf(-2.0f) == 0xC000);
        CHECK(media::floatToHalf(65504.0f) == 0x7BFF);
        CHECK(media::floatToHalf(65520.0f) == 0x7C00);
        CHECK(media::floatToHalf(std::ldexp(1.0f, -24)) == 0x0001);
        CHECK(media::floatToHalf(std::ldexp(1.0f, -25)) == 0x0000);
        CHECK(media::floatToHalf(std::ldexp(3.0f, -25)) == 0x0002);
        CHECK(media::floatToHalf(1.0f + std::ldexp(1.0f, -11)) == 0x3C00);
        CHECK(media::floatToHalf(1.0f + std::ldexp(3.0f, -11)) == 0x3C02);
        CHECK(media::floatToHalf(std::numeric_limits<float>::infinity()) == 0x7C00);
        const std::uint16_t nan = media::floatToHalf(std::numeric_limits<float>::quiet_NaN());
        CHECK((nan & 0x7C00) == 0x7C00);
        CHECK((nan & 0x03FF) != 0);

        std::uint32_t mismatches = 0;
        for (std::uint32_t h = 0; h <= 0xFFFF; ++h) {
            const auto half = static_cast<std::uint16_t>(h);
            const bool is_nan = (half & 0x7C00) == 0x7C00 && (half & 0x03FF) != 0;
            if (!is_nan && media::floatToHalf(media::halfToFloat(half)) != half) {
                ++mismatches;
            }
        }
        CHECK(mismatches == 0);
        CHECK(media::halfToFloat(0x0001) == std::ldexp(1.0f, -24));
        CHECK(media::halfToFloat(0x7BFF) == 65504.0f);
        CHECK(std::isinf(media::halfToFloat(0xFC00)));
        CHECK(std::signbit(media::halfToFloat(0x8000)));
        CHECK(std::isnan(media::halfToFloat(0x7E00)));
    }
}
