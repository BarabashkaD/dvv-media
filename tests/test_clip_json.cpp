// The public header comes first: it must compile on its own.
#include "dvv/media/clip_json.h"

// The test framework and what the cases use.
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

#include "dvv/base/error.h"
#include "dvv/media/colour_preset.h"
#include "dvv/media/types.h"
#include "dvv/media/uri_colour.h"
#include "temporary_directory.h"

namespace media = dvv::media;

namespace {

// A neutral fake root: the parser never touches the file system.
constexpr std::string_view CLIP = "/clips/demo/clip.json";

// The message of a refused text, without the "clip description '<file>': " prefix.
std::string refusal(std::string_view text) {
    const auto parsed = media::parseClipJson(text, CLIP);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == dvv::Errc::Format);
    const std::string prefix = "clip description '/clips/demo/clip.json': ";
    const std::string& message = parsed.error().message;
    REQUIRE(message.starts_with(prefix));
    return message.substr(prefix.size());
}

media::ClipDescription accepted(std::string_view text) {
    auto parsed = media::parseClipJson(text, CLIP);
    REQUIRE_MESSAGE(parsed.has_value(), parsed.error().message);
    return std::move(*parsed);
}

}  // namespace

TEST_CASE("clip JSON: the design's examples") {
    const media::ClipDescription pq = accepted(
        R"({ "rawreelClip": 1, "format": "yuv420p10le", "size": { "width": 3840, "height": 2160 },
             "frameRate": "60000/1001", "colour": { "standard": "bt2100-pq" } })");
    CHECK(pq == media::ClipDescription{
                    .format = "yuv420p10le",
                    .size = std::pair<std::uint32_t, std::uint32_t>{3840, 2160},
                    .fps = media::Rational{.num = 60000, .den = 1001},
                    .colour = media::ColourSpec{.preset = media::ColourPreset::Bt2100Pq,
                                                .range = media::ColourRange::Unspecified}});

    const media::ClipDescription hlg = accepted(
        R"({ "rawreelClip": 1, "format": "@formats/v210_custom.json",
             "colour": { "standard": "bt2100-hlg", "range": "full" } })");
    CHECK(hlg.format == "@/clips/demo/formats/v210_custom.json");
    CHECK_FALSE(hlg.size.has_value());
    CHECK_FALSE(hlg.fps.has_value());
    CHECK(hlg.colour == media::ColourSpec{.preset = media::ColourPreset::Bt2100Hlg,
                                          .range = media::ColourRange::Full});

    CHECK(accepted(R"({ "rawreelClip": 1, "colour": { "standard": "bt601-pal" } })") ==
          media::ClipDescription{
              .format = std::nullopt,
              .size = std::nullopt,
              .fps = std::nullopt,
              .colour = media::ColourSpec{.preset = media::ColourPreset::Bt601Pal,
                                          .range = media::ColourRange::Unspecified}});
}

TEST_CASE("clip JSON: range goes with every standard (U-D5-9)") {
    for (const media::ColourPresetInfo& info : media::colourPresets()) {
        for (const auto& [text, range] : {std::pair{"full", media::ColourRange::Full},
                                          std::pair{"limited", media::ColourRange::Limited}}) {
            const std::string json = std::string(R"({"rawreelClip": 1, "colour": {"standard": ")") +
                                     std::string(info.uriName) + R"(", "range": ")" + text +
                                     R"("}})";
            CAPTURE(json);
            CHECK(accepted(json).colour == media::ColourSpec{.preset = info.id, .range = range});
        }
    }
}
TEST_CASE("clip JSON: @ paths") {
    // Relative: joined to the clip's folder, '\' read as '/'.
    CHECK(accepted(R"({"rawreelClip": 1, "format": "@f\\x.json"})").format ==
          "@/clips/demo/f/x.json");
    CHECK(accepted(R"({"rawreelClip": 1, "format": "@x.json"})").format == "@/clips/demo/x.json");
    // Absolute ones are kept.
    CHECK(accepted(R"({"rawreelClip": 1, "format": "@/formats/x.json"})").format ==
          "@/formats/x.json");
    CHECK(accepted(R"({"rawreelClip": 1, "format": "@X:/formats/x.json"})").format ==
          "@X:/formats/x.json");
    // A clip path without a folder leaves the path as written.
    const auto bare = media::parseClipJson(R"({"rawreelClip": 1, "format": "@x.json"})", "c.json");
    REQUIRE(bare.has_value());
    CHECK(bare->format == "@x.json");
    CHECK(refusal(R"({"rawreelClip": 1, "format": "@"})") == "format: empty path after '@'");
}

TEST_CASE("clip JSON: size and frame rate") {
    CHECK(accepted(R"({"rawreelClip": 1, "frameRate": 25})").fps ==
          media::Rational{.num = 25, .den = 1});
    CHECK(accepted(R"({"rawreelClip": 1, "frameRate": "50"})").fps ==
          media::Rational{.num = 50, .den = 1});
    CHECK(accepted(R"({"rawreelClip": 1, "frameRate": "60/2"})").fps ==
          media::Rational{.num = 30, .den = 1});
    CHECK(accepted(R"({"rawreelClip": 1, "size": {"width": 1, "height": 65535}})").size ==
          std::pair<std::uint32_t, std::uint32_t>{1, 65535});

    constexpr std::string_view FPS = R"(frameRate: write N or N/D, e.g. "30000/1001")";
    for (const std::string_view value : {"29.97", R"("29.97")", "0", R"("0")", R"("30/0")",
                                         R"("-30")", R"("")", R"("30/")", "true", "-5"}) {
        CAPTURE(value);
        CHECK(refusal(std::string(R"({"rawreelClip": 1, "frameRate": )") + std::string(value) +
                      "}") == FPS);
    }

    CHECK(refusal(R"({"rawreelClip": 1, "size": {"width": 640}})") == "size: missing key 'height'");
    CHECK(refusal(R"({"rawreelClip": 1, "size": {"height": 480}})") == "size: missing key 'width'");
    CHECK(refusal(R"({"rawreelClip": 1, "size": {"w": 640, "height": 480}})") ==
          "size: unknown key 'w'");
    CHECK(refusal(R"({"rawreelClip": 1, "size": {"width": 0, "height": 480}})") ==
          "size.width: expected an integer 1..65535");
    CHECK(refusal(R"({"rawreelClip": 1, "size": {"width": 640, "height": 65536}})") ==
          "size.height: expected an integer 1..65535");
    CHECK(refusal(R"({"rawreelClip": 1, "size": {"width": 640.5, "height": 480}})") ==
          "size.width: expected an integer 1..65535");
    CHECK(refusal(R"({"rawreelClip": 1, "size": "640x480"})") == "size: not an object");
}

TEST_CASE("clip JSON: the top level") {
    CHECK(refusal(R"({"rawreelClip": 2, "format": "gray"})") ==
          "rawreelClip: unsupported version 2 (this rawreel reads 1)");
    CHECK(refusal(R"({"rawreelClip": "1", "format": "gray"})") ==
          "rawreelClip: expected the integer 1");
    CHECK(refusal(R"({"format": "gray"})") == "missing key 'rawreelClip'");
    CHECK(refusal(R"({"rawreelClip": 1})") ==
          "give at least one of 'format', 'size', 'frameRate' or 'colour'");
    CHECK(refusal(R"({"rawreelClip": 1, "format": "gray", "fps": 25})") == "unknown key 'fps'");
    CHECK(refusal("[1]") == "not an object");
    CHECK_FALSE(refusal("{").empty());  // json::parse's message

    CHECK(refusal(R"({"rawreelClip": 1, "format": "yuv421p"})") ==
          "format: unknown pixel format 'yuv421p'");
    CHECK(refusal(R"({"rawreelClip": 1, "format": "v210@planes"})") ==
          "format: unknown pixel format 'v210@planes'");
    CHECK(refusal(R"({"rawreelClip": 1, "format": 3})") == "format: expected a string");
}

TEST_CASE("clip JSON: colour errors") {
    const auto colour = [](std::string_view value) {
        return refusal(std::string(R"({"rawreelClip": 1, "colour": )") + std::string(value) + "}");
    };
    CHECK(colour(R"({})") == "colour: missing key 'standard'");
    CHECK(colour(R"({"range": "full"})") == "colour: missing key 'standard'");
    CHECK(colour(R"({"custom": {}})") == "colour: unknown key 'custom' (standard, range)");
    CHECK(colour(R"({"preset": "bt709"})") == "colour: unknown key 'preset' (standard, range)");
    CHECK(colour(R"({"standard": "bt709", "matrix": "bt709"})") ==
          "colour: unknown key 'matrix' (standard, range)");
    CHECK(colour(R"({"standard": "bt710"})") ==
          "colour.standard: bad colour 'bt710' (bt601-ntsc, bt601-pal, bt709, bt2020, bt2100-pq, "
          "bt2100-hlg)");
    CHECK(colour(R"({"standard": "jpeg"})") ==
          "colour.standard: bad colour 'jpeg' (bt601-ntsc, bt601-pal, bt709, bt2020, bt2100-pq, "
          "bt2100-hlg)");
    CHECK(colour(R"({"standard": 1})") == "colour.standard: expected a string");
    CHECK(colour(R"({"standard": "bt709", "range": "wide"})") ==
          "colour.range: bad range 'wide' (full, limited)");
    CHECK(colour(R"("bt709")") == "colour: not an object");
    CHECK(colour(R"([])") == "colour: not an object");
}

TEST_CASE("clip JSON: version values") {
    for (const std::string_view value : {"0", "-1", "2"}) {
        CAPTURE(value);
        CHECK(refusal(std::string(R"({"rawreelClip": )") + std::string(value) +
                      R"(, "format": "gray"})") ==
              "rawreelClip: unsupported version " + std::string(value) + " (this rawreel reads 1)");
    }
    // A non-integer number is a version too (a future writer's 2.0 is told it is unsupported).
    CHECK(refusal(R"({"rawreelClip": 1.0, "format": "gray"})") ==
          "rawreelClip: unsupported version 1.0 (this rawreel reads 1)");
    CHECK(refusal(R"({"rawreelClip": 2.5, "format": "gray"})") ==
          "rawreelClip: unsupported version 2.5 (this rawreel reads 1)");
    CHECK(refusal(R"({"rawreelClip": true, "format": "gray"})") ==
          "rawreelClip: expected the integer 1");
}

TEST_CASE("clip JSON: separators and absolute @ paths") {
    // A clip path with '\' separators (the usual Windows form) gives the folder.
    const auto windows = media::parseClipJson(R"({"rawreelClip": 1, "format": "@f\\x.json"})",
                                              "clips\\demo\\c.json");
    REQUIRE(windows.has_value());
    CHECK(windows->format == "@clips/demo/f/x.json");
    // '\' is replaced before the absolute test: a rooted path and a UNC path are kept.
    CHECK(accepted(R"({"rawreelClip": 1, "format": "@\\formats\\x.json"})").format ==
          "@/formats/x.json");
    CHECK(accepted(R"({"rawreelClip": 1, "format": "@\\\\server\\share\\x.json"})").format ==
          "@//server/share/x.json");
}

TEST_CASE("clip JSON: more size, frame-rate and colour refusals") {
    constexpr std::string_view FPS = R"(frameRate: write N or N/D, e.g. "30000/1001")";
    for (const std::string_view value : {R"("1/2/3")", R"("+30")", R"(" 30")", R"("30 ")",
                                         R"("123456789012345678901")", "123456789012345678901"}) {
        CAPTURE(value);
        CHECK(refusal(std::string(R"({"rawreelClip": 1, "frameRate": )") + std::string(value) +
                      "}") == FPS);
    }
    CHECK(refusal(R"({"rawreelClip": 1, "size": {"width": -640, "height": 480}})") ==
          "size.width: expected an integer 1..65535");
    CHECK(refusal(R"({"rawreelClip": 1, "size": {"width": 640, "height": "480"}})") ==
          "size.height: expected an integer 1..65535");
    CHECK(refusal(R"({"rawreelClip": 1, "colour": {"standard": "bt709", "range": 1}})") ==
          "colour.range: expected a string");
}

TEST_CASE("clip JSON: the small-file reader and loadClipJson") {
    const TemporaryDirectory directory;
    const auto write = [&](std::string_view name, std::string_view text) {
        const std::filesystem::path path = directory.path() / name;
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
        REQUIRE(file);
        return media::utf8FromPath(path);
    };
    const std::string text = R"({"rawreelClip": 1, "format": "@f.json", "frameRate": 30})";
    const std::string clip = write("c.json", text);
    const auto read = media::readSmallFile(clip);
    REQUIRE_MESSAGE(read.has_value(), read.error().describe());
    CHECK(*read == text);

    const auto loaded = media::loadClipJson(clip);
    REQUIRE_MESSAGE(loaded.has_value(), loaded.error().describe());
    CHECK(loaded->fps == media::Rational{.num = 30, .den = 1});
    std::string format = "@" + media::utf8FromPath(directory.path() / "f.json");
    std::ranges::replace(format, '\\', '/');
    CHECK(loaded->format == format);

    // A missing file is Io; a file over 1 MiB is Format "'<file>' is too large".
    const std::string missing = media::utf8FromPath(directory.path() / "missing.json");
    CHECK(media::readSmallFile(missing).error().code == dvv::Errc::Io);
    CHECK(media::loadClipJson(missing).error().code == dvv::Errc::Io);
    const std::string large = write("large.json", std::string((1U << 20U) + 1U, ' '));
    CHECK(media::readSmallFile(large).error().message == "'" + large + "' is too large");
    CHECK(media::loadClipJson(large).error().code == dvv::Errc::Format);
    CHECK(media::readSmallFile(write("limit.json", std::string(1U << 20U, ' '))).has_value());
    // A parse error names the file.
    CHECK(media::loadClipJson(write("bad.json", "{"))
              .error()
              .message.starts_with("clip description '" +
                                   media::utf8FromPath(directory.path() / "bad.json") + "': "));
}
