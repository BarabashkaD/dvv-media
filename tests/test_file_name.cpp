#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "dvv/media/colour_preset.h"
#include "dvv/media/file_name.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"

namespace media = dvv::media;

namespace {

struct Expected {
    std::uint32_t width = 0, height = 0;
    std::optional<std::string_view> format;
    std::optional<media::Rational> fps;
    std::optional<media::ColourPreset> colour;
};

void checkParsed(std::string_view path, const Expected& expected) {
    INFO(std::string(path));
    const std::optional<media::FileNameInfo> parsed = media::parseFileName(path);
    REQUIRE(parsed.has_value());
    const media::FileNameInfo info = parsed.value_or(media::FileNameInfo{});
    CHECK(info.width == expected.width);
    CHECK(info.height == expected.height);
    CHECK(info.format.has_value() == expected.format.has_value());
    if (info.format && expected.format) {
        CHECK(media::descriptor(*info.format).name == *expected.format);
    }
    CHECK(info.fps == expected.fps);
    CHECK(info.colour == expected.colour);
}

void checkRejected(std::string_view path) {
    INFO(std::string(path));
    CHECK_FALSE(media::parseFileName(path).has_value());
}

}  // namespace

TEST_CASE("file name: table") {
    checkParsed(
        "clip_34x17_v210_50",
        {.width = 34, .height = 17, .format = "v210", .fps = media::Rational{50, 1}, .colour = {}});
    checkParsed("clip_1920x1080_29.97fps", {.width = 1920,
                                            .height = 1080,
                                            .format = {},
                                            .fps = media::Rational{2997, 100},
                                            .colour = {}});
    checkParsed("10x20", {.width = 10, .height = 20, .format = {}, .fps = {}, .colour = {}});
    checkRejected("a_10x20_bogus");
    checkRejected("a_0x20");
    checkRejected("a_10x20_nv12_v210");

    // The extension and the directories are ignored.
    checkParsed("dir/clip_10x20.yuv",
                {.width = 10, .height = 20, .format = {}, .fps = {}, .colour = {}});
    checkParsed(
        "dir\\sub/clip_34x17_v210_50.yuv",
        {.width = 34, .height = 17, .format = "v210", .fps = media::Rational{50, 1}, .colour = {}});
    checkParsed(
        "clip_1920x1080_25fps.yuv",
        {.width = 1920, .height = 1080, .format = {}, .fps = media::Rational{25, 1}, .colour = {}});
    checkRejected("dir_10x20/clip.yuv");
    checkRejected("");

    // A non-ASCII stem.
    checkParsed("clip_\xC3\xA4\xE6\x97\xA5\xE6\x9C\xAC_33x17_yuv420p",
                {.width = 33, .height = 17, .format = "yuv420p", .fps = {}, .colour = {}});
}

TEST_CASE("file name: the colour token (D5R §3.4)") {
    checkParsed("clip_1920x1080_bt709.yuv", {.width = 1920,
                                             .height = 1080,
                                             .format = {},
                                             .fps = {},
                                             .colour = media::ColourPreset::Bt709});
    checkParsed("dir/clip_3840x2160_yuv420p10le_50_bt2100-pq.yuv",
                {.width = 3840,
                 .height = 2160,
                 .format = "yuv420p10le",
                 .fps = media::Rational{50, 1},
                 .colour = media::ColourPreset::Bt2100Pq});
    checkParsed("a_720x576_yuv422p_bt601-pal", {.width = 720,
                                                .height = 576,
                                                .format = "yuv422p",
                                                .fps = {},
                                                .colour = media::ColourPreset::Bt601Pal});
    checkParsed("a_720x480_29.97_bt601-ntsc.raw", {.width = 720,
                                                   .height = 480,
                                                   .format = {},
                                                   .fps = media::Rational{2997, 100},
                                                   .colour = media::ColourPreset::Bt601Ntsc});
    // Every token is read, and no catalogue format name or fps token equals one.
    for (const media::ColourPresetInfo& info : media::colourPresets()) {
        INFO(std::string(info.uriName));
        CHECK_FALSE(media::findFormat(info.uriName).has_value());
        const std::optional<media::FileNameInfo> parsed =
            media::parseFileName(std::string("x_16x16_") + std::string(info.uriName) + ".yuv");
        REQUIRE(parsed.has_value());
        CHECK(parsed.value_or(media::FileNameInfo{}).colour == info.id);
    }
    // Exact and lower case; the colour comes last; WxH is still required.
    checkRejected("clip_1920x1080_BT709.yuv");
    checkRejected("clip_1920x1080_bt2100.yuv");
    checkRejected("clip_1920x1080_bt709_25.yuv");
    checkRejected("clip_bt709.yuv");
    checkRejected("bt709");
    checkRejected("clip_yuv420p_bt709.yuv");
}
