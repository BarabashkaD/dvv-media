#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/json.h"
#include "dvv/base/uri.h"
#include "dvv/media/display.h"
#include "dvv/media/format_json.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/reference.h"
#include "dvv/media/source.h"
#include "dvv/media/source_factory.h"
#include "dvv/media/types.h"
#include "dvv/platform/file_reader.h"
#include "dvv/registry/capabilities.h"
#include "dvv/registry/component_registry.h"
#include "patterns.h"
#include "source_harness.h"
#include "sources/file_read.h"
#include "temporary_directory.h"

namespace media = dvv::media;
namespace detail = dvv::media::detail;
namespace platform = dvv::platform;
namespace fs = std::filesystem;

namespace {

// The default content: yuv420p10le 33x17, noise with seed 4.
constexpr std::string_view FORMAT = "yuv420p10le";
constexpr std::uint32_t WIDTH = 33;
constexpr std::uint32_t HEIGHT = 17;
constexpr std::uint64_t SEED = 4;

media::FormatId idOf(std::string_view name) {
    const auto id = media::findFormat(name);
    REQUIRE_MESSAGE(id.has_value(), std::string(name));
    return id.value_or(media::FormatId{0});
}

// pack(generate(Noise, ...)) of frame `index`.
std::vector<std::byte> packedFrame(std::string_view format, std::uint32_t width,
                                   std::uint32_t height, std::uint64_t seed, std::int64_t index) {
    const auto values =
        detail::generate(detail::Pattern::Noise, idOf(format), width, height, seed, index);
    REQUIRE_MESSAGE(values.has_value(), values.error().describe());
    auto bytes = media::pack(*values);
    REQUIRE_MESSAGE(bytes.has_value(), bytes.error().describe());
    return *bytes;
}

// Frames 0..count-1 back to back.
std::vector<std::byte> packedFrames(std::string_view format, std::uint32_t width,
                                    std::uint32_t height, std::uint64_t seed, std::int64_t count) {
    std::vector<std::byte> bytes;
    for (std::int64_t index = 0; index < count; ++index) {
        const std::vector<std::byte> frame = packedFrame(format, width, height, seed, index);
        bytes.insert(bytes.end(), frame.begin(), frame.end());
    }
    return bytes;
}

void writeFile(const fs::path& path, std::span<const std::byte> bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE(file);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file);
}

void writeText(const fs::path& path, std::string_view text) {
    writeFile(path, std::as_bytes(std::span(text)));
}

// The path as UTF-8 URI text (a bare path).
std::string utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.c_str()), text.size()};
}

// The default 3-frame file in `directory`.
fs::path writeDefault(const fs::path& directory, std::string_view name = "clip.bin") {
    const fs::path path = directory / name;
    writeFile(path, packedFrames(FORMAT, WIDTH, HEIGHT, SEED, 3));
    return path;
}

std::vector<std::byte> frameBytes(SourceHarness& harness,
                                  const std::shared_ptr<media::ISource>& source,
                                  std::int64_t index) {
    auto bytes = harness.originalBytes(source, index);
    REQUIRE_MESSAGE(bytes.has_value(), bytes.error().describe());
    return *bytes;
}

// The error of an open that must fail.
dvv::Error openError(SourceHarness& harness, std::string_view uri) {
    INFO(std::string(uri));
    auto opened = harness.tryOpen(uri);
    REQUIRE_FALSE(opened.has_value());
    return opened.error();
}

// The raw provider's own match for a URI (no head), outside the factory.
media::Match rawMatch(std::string_view uri, std::optional<media::FormatHint> hint = {},
                      std::optional<media::Sidecar> sidecar = {}) {
    INFO(std::string(uri));
    const auto registry = dvv::ComponentRegistry::create(dvv::Capabilities{});
    auto provider = registry->createComponent<media::ISourceProvider>("raw");
    REQUIRE_MESSAGE(provider.has_value(), provider.error().describe());
    auto parsed = dvv::parseUri(uri);
    REQUIRE_MESSAGE(parsed.has_value(), parsed.error().describe());
    const media::SourceRequest request{.uri = std::move(*parsed),
                                       .hint = hint,
                                       .fileReads = platform::FileReadMode::Auto,
                                       .sidecar = std::move(sidecar)};
    return (*provider)->match(request, {});
}

void checkPlan(const detail::FrameRead& plan, const detail::FrameRead& expected) {
    CHECK(plan.alignedOffset == expected.alignedOffset);
    CHECK(plan.rangeBytes == expected.rangeBytes);
    CHECK(plan.dataOffset == expected.dataOffset);
    CHECK(plan.frameBytes == expected.frameBytes);
}

}  // namespace

TEST_CASE("raw: frame read plan") {
    checkPlan(detail::planFrameRead(5000, 1000, 0, 4096), {4096, 4096, 904, 1000});
    checkPlan(detail::planFrameRead(0, 4096, 0, 4096), {0, 4096, 0, 4096});
    checkPlan(detail::planFrameRead(100, 10, 6, 1), {94, 16, 6, 10});
    checkPlan(detail::planFrameRead(4100, 10, 6, 4096), {0, 8192, 4100, 10});
}

TEST_CASE("raw: a truncated frame is Io") {
    const TemporaryDirectory directory;
    const fs::path path = directory.path() / "short.bin";
    writeFile(path, std::vector<std::byte>(100, std::byte{7}));

    for (const FileReadCase& mode : fileReadCases()) {
        CAPTURE(mode.reader);
        auto reader = platform::FileReader::open(path, mode.mode);
        REQUIRE_MESSAGE(reader.has_value(), reader.error().describe());
        CHECK(reader->modeText() == mode.reader);
        const detail::FrameRead plan = detail::planFrameRead(50, 80, 0, reader->alignment());
        auto memory = platform::allocateAligned(plan.rangeBytes, reader->alignment());
        REQUIRE_MESSAGE(memory.has_value(), memory.error().describe());
        const auto read = detail::readFrame(
            *reader, plan, std::span(memory->get(), plan.rangeBytes), 3, utf8(path));
        REQUIRE_FALSE(read.has_value());
        INFO(read.error().describe());
        CHECK(read.error().code == dvv::Errc::Io);
        CHECK(read.error().message.find("truncated frame 3 of") != std::string::npos);
    }
}

TEST_CASE("raw: query size and format") {
    const TemporaryDirectory directory;
    const std::string uri = utf8(writeDefault(directory.path())) + "?fmt=yuv420p10le&size=33x17";
    CHECK(rawMatch(uri).confidence == 95);

    SourceHarness harness;
    const auto source = harness.open(uri);
    const media::SourceInfo& info = source->info();
    CHECK(info.provider == "raw");
    CHECK(info.frameCount == 3);
    CHECK(info.desc ==
          media::FrameDesc{.format = idOf(FORMAT), .colour = 0, .width = WIDTH, .height = HEIGHT});
    CHECK(info.fps == media::Rational{25, 1});
    CHECK(info.access == media::SourceAccess::RandomCheap);
    CHECK(info.warnings.empty());
    CHECK(frameBytes(harness, source, 1) == packedFrame(FORMAT, WIDTH, HEIGHT, SEED, 1));
}

TEST_CASE("raw: file-name detection") {
    const TemporaryDirectory directory;
    SourceHarness harness;

    // Size, format and fps from the name.
    const fs::path v210 = directory.path() / "clip_34x17_v210_50.yuv";
    writeFile(v210, packedFrames("v210", 34, 17, SEED, 2));
    CHECK(rawMatch(utf8(v210)).confidence == 70);
    const auto named = harness.open(utf8(v210));
    CHECK(named->info().fps == media::Rational{50, 1});
    CHECK(named->info().desc.format == idOf("v210"));
    CHECK(named->info().frameCount == 2);
    CHECK(frameBytes(harness, named, 1) == packedFrame("v210", 34, 17, SEED, 1));

    // Size only: the format is a guess, so the factory asks.
    const fs::path size_only = writeDefault(directory.path(), "clip_33x17.yuv");
    auto opened = harness.tryOpen(utf8(size_only));
    REQUIRE_MESSAGE(opened.has_value(), opened.error().describe());
    const auto* ambiguous = std::get_if<media::Ambiguous>(&*opened);
    REQUIRE(ambiguous != nullptr);
    REQUIRE(ambiguous->candidates.size() == 1);
    CHECK(ambiguous->candidates[0].component == "raw");
    CHECK(ambiguous->candidates[0].confidence == 30);
    CHECK(ambiguous->candidates[0].guess.format == idOf("yuv420p"));
    CHECK(ambiguous->candidates[0].guess.width == 33);
    CHECK(ambiguous->candidates[0].guess.height == 17);

    // The query wins over the name.
    const fs::path nv12 = writeDefault(directory.path(), "clip_33x17_nv12.yuv");
    const auto queried = harness.open(utf8(nv12) + "?fmt=yuv420p10le");
    CHECK(queried->info().desc.format == idOf(FORMAT));
    CHECK(frameBytes(harness, queried, 2) == packedFrame(FORMAT, WIDTH, HEIGHT, SEED, 2));

    // A hint counts as explicit; nothing known is 0.
    const media::FormatHint hint{
        .format = idOf(FORMAT), .width = WIDTH, .height = HEIGHT, .fps = std::nullopt};
    CHECK(rawMatch(utf8(directory.path() / "clip.bin"), hint).confidence == 95);
    const media::Match nothing = rawMatch(utf8(directory.path() / "clip.bin"));
    CHECK(nothing.confidence == 0);
    CHECK(nothing.why == "no frame size: name the file <name>_WxH[_fmt] or pass ?size=WxH&fmt=");
}

TEST_CASE("raw: custom format from JSON") {
    const TemporaryDirectory directory;
    const fs::path clip = writeDefault(directory.path());
    dvv::json::Value value = media::formatToJson(idOf(FORMAT));
    value["name"] = "my10le";
    const fs::path json = directory.path() / "my10le.json";
    writeText(json, dvv::json::dump(value, 2));

    SourceHarness harness;
    const auto source =
        harness.open(utf8(clip) + "?fmt=@" + dvv::percentEncode(utf8(json)) + "&size=33x17");
    CHECK(media::descriptor(source->info().desc.format).name == "my10le");
    // A query value keeps '/', ':', '@' and '\'; the path keeps '/', ':' and '\'.
    CHECK(source->info().resolvedUri == "file:" + dvv::percentEncode(utf8(clip)) + "?fmt=@" +
                                            dvv::percentEncode(utf8(json), "/:@\\") +
                                            "&size=33x17&fps=25");
    CHECK(frameBytes(harness, source, 2) == packedFrame(FORMAT, WIDTH, HEIGHT, SEED, 2));

    const fs::path broken = directory.path() / "broken.json";
    writeText(broken, "{");
    CHECK(
        openError(harness, utf8(clip) + "?fmt=@" + dvv::percentEncode(utf8(broken)) + "&size=33x17")
            .code == dvv::Errc::Format);
    CHECK(openError(harness, utf8(clip) + "?fmt=@" +
                                 dvv::percentEncode(utf8(directory.path() / "missing.json")) +
                                 "&size=33x17")
              .code == dvv::Errc::Io);
    const fs::path large = directory.path() / "large.json";
    writeText(large, std::string((1U << 20U) + 1U, ' '));
    const dvv::Error too_large =
        openError(harness, utf8(clip) + "?fmt=@" + dvv::percentEncode(utf8(large)) + "&size=33x17");
    CHECK(too_large.code == dvv::Errc::Format);
    CHECK(too_large.message.find("is too large") != std::string::npos);
}

TEST_CASE("raw: trailing bytes warning") {
    const TemporaryDirectory directory;
    const fs::path path = directory.path() / "trailing.bin";
    std::vector<std::byte> bytes = packedFrames(FORMAT, WIDTH, HEIGHT, SEED, 2);
    bytes.insert(bytes.end(), 5, std::byte{1});
    writeFile(path, bytes);

    SourceHarness harness;
    for (const FileReadCase& mode : fileReadCases()) {
        CAPTURE(mode.reader);
        const auto source = harness.open(utf8(path) + "?fmt=yuv420p10le&size=33x17", mode.mode);
        CHECK(source->info().reader == mode.reader);
        CHECK(source->info().frameCount == 2);
        CHECK(source->info().warnings == std::vector<std::string>{"5 trailing bytes ignored"});
        // The last whole frame ends 5 bytes before the end of the file.
        CHECK(frameBytes(harness, source, 1) == packedFrame(FORMAT, WIDTH, HEIGHT, SEED, 1));
    }
}

TEST_CASE("raw: one-frame and zero-frame files") {
    const TemporaryDirectory directory;
    SourceHarness harness;
    constexpr std::string_view QUERY = "?fmt=yuv420p10le&size=33x17";

    const fs::path one = directory.path() / "one.bin";
    writeFile(one, packedFrames(FORMAT, WIDTH, HEIGHT, SEED, 1));
    const auto single = harness.open(utf8(one) + std::string(QUERY));
    CHECK(single->info().frameCount == 1);
    CHECK(single->info().warnings.empty());
    CHECK(frameBytes(harness, single, 0) == packedFrame(FORMAT, WIDTH, HEIGHT, SEED, 0));
    const auto outside = harness.originalBytes(single, 1);
    REQUIRE_FALSE(outside.has_value());
    INFO(outside.error().describe());
    CHECK(outside.error().code == dvv::Errc::Internal);

    const fs::path tiny = directory.path() / "tiny.bin";
    writeFile(tiny, std::vector<std::byte>(10, std::byte{3}));
    const auto ten = harness.open(utf8(tiny) + std::string(QUERY));
    CHECK(ten->info().frameCount == 0);
    CHECK(ten->info().warnings == std::vector<std::string>{"10 trailing bytes ignored"});

    const fs::path empty = directory.path() / "empty.bin";
    writeFile(empty, {});
    const auto none = harness.open(utf8(empty) + std::string(QUERY));
    CHECK(none->info().frameCount == 0);
    CHECK(none->info().warnings.empty());
}

TEST_CASE("raw: unaligned frame offsets") {
    // yuv420p 33x17 is 867 bytes a frame: frames cross the 4096-byte boundaries.
    const TemporaryDirectory directory;
    const fs::path path = directory.path() / "odd.bin";
    writeFile(path, packedFrames("yuv420p", WIDTH, HEIGHT, SEED, 5));

    // Every read mode gives the bytes of the CPU reference (U-IO2); the last frame ends at the end
    // of the file, inside a partial last sector.
    SourceHarness harness;
    for (const FileReadCase& mode : fileReadCases()) {
        CAPTURE(mode.reader);
        const auto source = harness.open(utf8(path) + "?fmt=yuv420p&size=33x17", mode.mode);
        CHECK(source->info().reader == mode.reader);
        REQUIRE(source->info().frameCount == 5);
        for (std::int64_t index = 0; index < 5; ++index) {
            INFO(index);
            CHECK(frameBytes(harness, source, index) ==
                  packedFrame("yuv420p", WIDTH, HEIGHT, SEED, index));
        }
    }
}

TEST_CASE("raw: errors") {
    const TemporaryDirectory directory;
    const std::string clip = utf8(writeDefault(directory.path()));
    SourceHarness harness;
    struct Case {
        std::string uri;
        dvv::Errc code;
    };
    const std::vector<Case> cases{
        {clip + "?fmt=nope&size=33x17", dvv::Errc::Format},
        {clip + "?fmt=yuv420p&size=12", dvv::Errc::Format},
        {clip + "?fmt=yuv420p&size=33x17&foo=1", dvv::Errc::Format},
        {clip + "?fmt=v210&size=33x17", dvv::Errc::Format},
        {utf8(directory.path() / "missing.yuv") + "?fmt=gray&size=2x2", dvv::Errc::Io},
        {utf8(directory.path()) + "?fmt=gray&size=2x2", dvv::Errc::Io},
    };
    for (const Case& entry : cases) {
        const dvv::Error error = openError(harness, entry.uri);
        INFO(error.describe());
        CHECK(error.code == entry.code);
    }
}

TEST_CASE("raw: non-ASCII path") {
    const TemporaryDirectory directory;
    const fs::path path = directory.path() / fs::path(u8"clip_ä日本_33x17_yuv420p.yuv");
    writeFile(path, packedFrames("yuv420p", WIDTH, HEIGHT, SEED, 2));
    const std::u8string text = path.u8string();
    const std::string uri(reinterpret_cast<const char*>(text.c_str()), text.size());
    CHECK(rawMatch(uri).confidence == 70);

    SourceHarness harness;
    const auto source = harness.open(uri);
    CHECK(source->info().desc.format == idOf("yuv420p"));
    CHECK(source->info().frameCount == 2);
    CHECK(frameBytes(harness, source, 0) == packedFrame("yuv420p", WIDTH, HEIGHT, SEED, 0));
}

TEST_CASE("raw: resolved URI reopens") {
    const TemporaryDirectory directory;
    const fs::path clip = writeDefault(directory.path());
    const std::string uri =
        utf8(clip) + "?w=33&h=17&fmt=yuv420p10le&fps=29.97&range=full&colour=bt709";
    SourceHarness harness;
    const auto source = harness.open(uri);
    const media::SourceInfo& info = source->info();
    CHECK(info.fps == media::Rational{2997, 100});
    CHECK(media::colourParams(info.desc.colour) ==
          media::ColourParams{.matrix = media::ColourMatrix::Bt709,
                              .range = media::ColourRange::Full,
                              .transfer = media::TransferFn::Bt1886,
                              .primaries = media::Primaries::Bt709,
                              .kr = 0,
                              .kb = 0});
    CHECK(info.resolvedUri ==
          "file:" + dvv::percentEncode(utf8(clip)) +
              "?fmt=yuv420p10le&size=33x17&fps=2997/100&colour=bt709&range=full");

    const auto reopened = harness.open(info.resolvedUri);
    CHECK(reopened->info().desc == info.desc);
    CHECK(reopened->info().fps == info.fps);
    CHECK(reopened->info().frameCount == info.frameCount);
    CHECK(reopened->info().resolvedUri == info.resolvedUri);
    CHECK(frameBytes(harness, reopened, 1) == frameBytes(harness, source, 1));
}

TEST_CASE("raw: colour parameters") {
    const TemporaryDirectory directory;
    const fs::path clip = writeDefault(directory.path());
    SourceHarness harness;
    const auto source = harness.open(utf8(clip) + "?fmt=yuv420p10le&size=33x17&colour=bt2100-hlg");
    const media::SourceInfo& info = source->info();
    const media::ColourParams& colour = media::colourParams(info.desc.colour);
    CHECK(colour.transfer == media::TransferFn::Hlg);
    CHECK(colour.primaries == media::Primaries::Bt2020);
    CHECK(info.resolvedUri == "file:" + dvv::percentEncode(utf8(clip)) +
                                  "?fmt=yuv420p10le&size=33x17&fps=25&colour=bt2100-hlg");
    const auto reopened = harness.open(info.resolvedUri);
    CHECK(reopened->info().desc == info.desc);
    CHECK(reopened->info().resolvedUri == info.resolvedUri);

    // The removed keys name colour= (D5R §3.1).
    const dvv::Error removed =
        openError(harness, utf8(clip) + "?fmt=yuv420p10le&size=33x17&primaries=bt2020");
    CHECK(removed.code == dvv::Errc::Format);
    CHECK(removed.message ==
          "unknown parameter 'primaries' (set the colour with "
          "colour=<standard>: bt601-ntsc, bt601-pal, bt709, bt2020, "
          "bt2100-pq, bt2100-hlg)");
}

TEST_CASE("raw: a colour preset with a field key keeps its form in the resolved URI") {
    const TemporaryDirectory directory;
    const fs::path clip = writeDefault(directory.path());
    SourceHarness harness;
    const auto source =
        harness.open(utf8(clip) + "?fmt=yuv420p10le&size=33x17&range=full&colour=bt2100-pq");
    const media::SourceInfo& info = source->info();
    CHECK(media::colourParams(info.desc.colour) ==
          media::ColourParams{.matrix = media::ColourMatrix::Bt2020Ncl,
                              .range = media::ColourRange::Full,
                              .transfer = media::TransferFn::Pq,
                              .primaries = media::Primaries::Bt2020});
    CHECK(info.resolvedUri == "file:" + dvv::percentEncode(utf8(clip)) +
                                  "?fmt=yuv420p10le&size=33x17&fps=25&colour=bt2100-pq&range=full");
    const auto reopened = harness.open(info.resolvedUri);
    CHECK(reopened->info().desc == info.desc);
    CHECK(reopened->info().resolvedUri == info.resolvedUri);
}

TEST_CASE("raw: a colour preset on an RGB or gray format renders Full range (D5 design §2)") {
    const TemporaryDirectory directory;
    for (const std::string_view format : {"rgb24", "gray"}) {
        CAPTURE(format);
        const fs::path clip = directory.path() / (std::string(format) + ".bin");
        writeFile(clip, packedFrames(format, 8, 4, SEED, 2));
        SourceHarness harness;
        const auto source =
            harness.open(utf8(clip) + "?fmt=" + std::string(format) + "&size=8x4&colour=bt709");
        const media::SourceInfo& info = source->info();
        // The preset leaves the range unset (M4-D5-RGBRANGE); resolveColour() gives Full.
        CHECK(media::colourParams(info.desc.colour) ==
              media::ColourParams{.matrix = media::ColourMatrix::Unspecified,
                                  .range = media::ColourRange::Unspecified,
                                  .transfer = media::TransferFn::Bt1886,
                                  .primaries = media::Primaries::Bt709,
                                  .kr = 0,
                                  .kb = 0});
        const media::ResolvedColour resolved = media::resolveColour(info.desc);
        CHECK(resolved.params.matrix == media::ColourMatrix::Rgb);
        CHECK(resolved.params.range == media::ColourRange::Full);
        const media::DisplayAffine rendered =
            media::displayAffine(info.desc.format, resolved.params);
        const media::DisplayAffine full = media::displayAffine(
            info.desc.format, media::ColourParams{.matrix = media::ColourMatrix::Rgb,
                                                  .range = media::ColourRange::Full,
                                                  .transfer = media::TransferFn::Bt1886,
                                                  .primaries = media::Primaries::Bt709,
                                                  .kr = 0,
                                                  .kb = 0});
        CHECK(rendered.matrix == full.matrix);
        CHECK(rendered.offset == full.offset);
        CHECK(info.resolvedUri == "file:" + dvv::percentEncode(utf8(clip)) + "?fmt=" +
                                      std::string(format) + "&size=8x4&fps=25&colour=bt709");
        const auto reopened = harness.open(info.resolvedUri);
        CHECK(reopened->info().desc == info.desc);
        // An explicit range key still wins over the preset.
        const auto limited = harness.open(utf8(clip) + "?fmt=" + std::string(format) +
                                          "&size=8x4&colour=bt709&range=limited");
        CHECK(media::colourParams(limited->info().desc.colour).range ==
              media::ColourRange::Limited);
        CHECK(media::resolveColour(limited->info().desc).params.range ==
              media::ColourRange::Limited);
    }
}

namespace {

// A clip description that gives every group, for the default content.
constexpr std::string_view FULL_CLIP =
    R"({"rawreelClip": 1, "format": "yuv420p10le", "size": {"width": 33, "height": 17},
        "frameRate": "30000/1001", "colour": {"standard": "bt709"}})";

// The colour id that a query colour gives the default content.
std::uint32_t queryColour(SourceHarness& harness, const fs::path& path, std::string_view colour) {
    const auto source =
        harness.open(utf8(path) + "?fmt=yuv420p10le&size=33x17&" + std::string(colour));
    return source->info().desc.colour;
}

}  // namespace

TEST_CASE("raw: clip= fills the groups") {
    const TemporaryDirectory directory;
    const fs::path path = writeDefault(directory.path());
    const fs::path clip = directory.path() / "desc.json";
    writeText(clip, FULL_CLIP);
    const std::string uri = utf8(path) + "?clip=@" + utf8(clip);
    const media::Match match = rawMatch(uri);
    CHECK(match.confidence == 95);
    CHECK(match.why == "explicit clip description");

    SourceHarness harness;
    const auto source = harness.open(uri);
    const media::SourceInfo& info = source->info();
    CHECK(info.provider == "raw");
    CHECK(info.desc.format == idOf(FORMAT));
    CHECK(info.desc.width == WIDTH);
    CHECK(info.desc.height == HEIGHT);
    CHECK(info.fps == media::Rational{30000, 1001});
    CHECK(info.frameCount == 3);
    CHECK(info.desc.colour == queryColour(harness, path, "colour=bt709"));
    CHECK(info.clipFile == utf8(clip));
    CHECK(info.notes.empty());
    CHECK(info.warnings.empty());
    // The resolved URI keeps clip= and not the clip's colour; it reopens the same.
    CHECK(info.resolvedUri.find("clip=") != std::string::npos);
    CHECK(info.resolvedUri.find("fps=") < info.resolvedUri.find("clip="));
    CHECK(info.resolvedUri.find("colour=") == std::string::npos);
    CHECK(harness.open(info.resolvedUri)->info().desc == info.desc);
    CHECK(frameBytes(harness, source, 2) == packedFrame(FORMAT, WIDTH, HEIGHT, SEED, 2));
}

TEST_CASE("raw: field keys beat clip=") {
    const TemporaryDirectory directory;
    const fs::path path = writeDefault(directory.path());
    const fs::path clip = directory.path() / "desc.json";
    writeText(clip, FULL_CLIP);
    const std::string uri = utf8(path) + "?clip=@" + utf8(clip);

    SourceHarness harness;
    const auto keys = harness.open(uri + "&fmt=yuv420p&size=16x8&fps=50&range=full");
    CHECK(keys->info().desc.format == idOf("yuv420p"));
    CHECK(keys->info().desc.width == 16);
    CHECK(keys->info().desc.height == 8);
    CHECK(keys->info().fps == media::Rational{50, 1});
    const auto full = harness.open(utf8(path) + "?fmt=yuv420p&size=16x8&colour=bt709&range=full");
    CHECK(keys->info().desc.colour == full->info().desc.colour);
    // colour= beats every field of the clip's colour.
    CHECK(harness.open(uri + "&colour=bt2020")->info().desc.colour ==
          queryColour(harness, path, "colour=bt2020"));
}

TEST_CASE("raw: clip= beats the sidecar") {
    const TemporaryDirectory directory;
    const fs::path path = writeDefault(directory.path());
    writeText(fs::path(path) += ".json",
              R"({"rawreelClip": 1, "format": "yuv420p", "size": {"width": 16, "height": 8}})");
    const fs::path clip = directory.path() / "desc.json";
    writeText(clip, FULL_CLIP);

    SourceHarness harness;
    const auto source = harness.open(utf8(path) + "?clip=@" + utf8(clip));
    CHECK(source->info().desc.format == idOf(FORMAT));
    CHECK(source->info().desc.width == WIDTH);
    CHECK(source->info().clipFile == utf8(clip));
    CHECK(source->info().notes.empty());
}

TEST_CASE("raw: the sidecar beats the file name") {
    const TemporaryDirectory directory;
    const std::string name = "clip_16x8_yuv420p.yuv";
    const fs::path path = writeDefault(directory.path(), name);
    const fs::path sidecar = fs::path(path) += ".json";
    const std::string text =
        R"({"rawreelClip": 1, "format": "yuv420p10le", "size": {"width": 33, "height": 17},
            "frameRate": 30, "colour": {"standard": "bt2020"}})";
    writeText(sidecar, text);
    const media::Sidecar given{.path = utf8(sidecar), .text = text, .error = {}};
    const media::Match match = rawMatch(utf8(path), {}, given);
    CHECK(match.confidence == 75);
    CHECK(match.why == "format and size from " + name + ".json");
    CHECK(match.guess.width == WIDTH);
    CHECK(match.guess.format == idOf(FORMAT));
    CHECK(match.guess.fps == media::Rational{30, 1});
    CHECK(rawMatch(utf8(path)).confidence == 70);

    // The hint beats the sidecar for the format, the size and the frame rate.
    const media::FormatHint hint{
        .format = idOf("yuv420p"), .width = 16, .height = 8, .fps = media::Rational{50, 1}};
    const media::Match hinted = rawMatch(utf8(path), hint, given);
    CHECK(hinted.guess.format == idOf("yuv420p"));
    CHECK(hinted.guess.width == 16);
    CHECK(hinted.guess.fps == media::Rational{50, 1});

    SourceHarness harness;
    const auto hinted_source = harness.open(utf8(path), platform::FileReadMode::Auto, hint);
    CHECK(hinted_source->info().desc.format == idOf("yuv420p"));
    CHECK(hinted_source->info().desc.width == 16);
    CHECK(hinted_source->info().desc.height == 8);
    CHECK(hinted_source->info().fps == media::Rational{50, 1});

    const auto source = harness.open(utf8(path));
    const media::SourceInfo& info = source->info();
    CHECK(info.desc.format == idOf(FORMAT));
    CHECK(info.desc.width == WIDTH);
    CHECK(info.desc.height == HEIGHT);
    CHECK(info.fps == media::Rational{30, 1});
    CHECK(info.desc.colour == queryColour(harness, path, "colour=bt2020"));
    CHECK(info.clipFile == utf8(sidecar));
    REQUIRE(info.notes.size() == 1);
    CHECK(info.notes[0] ==
          std::format("{}.json describes {} (it wins over the file name)", name, name));
    CHECK(info.warnings.empty());
    // A raw clip carries no file tags: the colour hint stays at its default (D5R F1).
    CHECK(info.colourHint == media::FileHint{});
    // The sidecar leaves no trace in the resolved URI; the sidecar is read again on a reopen.
    CHECK(info.resolvedUri.find("colour=") == std::string::npos);
    CHECK(info.resolvedUri.find("clip=") == std::string::npos);
    CHECK(harness.open(info.resolvedUri)->info().desc == info.desc);

    // Without a name that gives anything, the note has no bracket.
    const fs::path plain = writeDefault(directory.path(), "plain.bin");
    writeText(fs::path(plain) += ".json", text);
    const auto plain_source = harness.open(utf8(plain));
    REQUIRE(plain_source->info().notes.size() == 1);
    CHECK(plain_source->info().notes[0] == "plain.bin.json describes plain.bin");
}

TEST_CASE("raw: a bad sidecar warns and opens by the name") {
    const TemporaryDirectory directory;
    const std::string name = "clip_33x17_yuv420p10le.yuv";
    const fs::path path = writeDefault(directory.path(), name);
    const std::string text = R"({"rawreelClip": 1, "size": {"w": 1}})";
    writeText(fs::path(path) += ".json", text);
    const media::Match match = rawMatch(
        utf8(path), {}, media::Sidecar{.path = utf8(path) + ".json", .text = text, .error = {}});
    CHECK(match.confidence == 70);

    SourceHarness harness;
    const auto source = harness.open(utf8(path));
    const media::SourceInfo& info = source->info();
    CHECK(info.desc.format == idOf(FORMAT));
    CHECK(info.desc.width == WIDTH);
    CHECK(info.clipFile.empty());
    CHECK(info.notes.empty());
    // The reason does not repeat the sidecar's path.
    CHECK(info.warnings == std::vector<std::string>{name + ".json ignored: size: unknown key 'w'"});

    // A sidecar that cannot be read (the factory passes its error) warns the same way.
    writeText(fs::path(path) += ".json", std::string((1U << 20U) + 1U, ' '));
    CHECK(rawMatch(utf8(path), {},
                   media::Sidecar{.path = utf8(path) + ".json", .text = {}, .error = "too large"})
              .confidence == 70);
    const auto unreadable = harness.open(utf8(path));
    CHECK(unreadable->info().desc.width == WIDTH);
    CHECK(unreadable->info().clipFile.empty());
    REQUIRE(unreadable->info().warnings.size() == 1);
    const std::string& warning = unreadable->info().warnings[0];
    CHECK(warning.starts_with(name + ".json ignored: "));
    CHECK(warning.find("is too large") != std::string::npos);
    CHECK(warning.find(utf8(directory.path())) == std::string::npos);
}

TEST_CASE("raw: a sidecar whose @ format cannot be read warns and opens by the name") {
    const TemporaryDirectory directory;
    const std::string name = "clip_33x17_yuv420p10le.yuv";
    const fs::path path = writeDefault(directory.path(), name);
    writeText(directory.path() / "broken.json", "{");
    SourceHarness harness;
    for (const std::string_view format : {"@missing.json", "@broken.json"}) {
        CAPTURE(format);
        const std::string text =
            R"({"rawreelClip": 1, "format": ")" + std::string(format) + R"("})";
        writeText(fs::path(path) += ".json", text);
        CHECK(rawMatch(utf8(path), {},
                       media::Sidecar{.path = utf8(path) + ".json", .text = text, .error = {}})
                  .confidence == 75);
        const auto source = harness.open(utf8(path));
        const media::SourceInfo& info = source->info();
        CHECK(info.desc.format == idOf(FORMAT));
        CHECK(info.desc.width == WIDTH);
        CHECK(info.clipFile.empty());
        CHECK(info.notes.empty());
        REQUIRE(info.warnings.size() == 1);
        CHECK(info.warnings[0].starts_with(name + ".json ignored: "));
    }
    // The same format in a clip= file fails the open.
    const fs::path desc = directory.path() / "desc.json";
    writeText(desc, R"({"rawreelClip": 1, "format": "@missing.json"})");
    CHECK(openError(harness, utf8(path) + "?clip=@" + utf8(desc)).code == dvv::Errc::Io);
}

TEST_CASE("raw: a sidecar's relative @ format is read from the sidecar's folder") {
    const TemporaryDirectory directory;
    const fs::path path = writeDefault(directory.path(), "plain.bin");
    dvv::json::Value value = media::formatToJson(idOf(FORMAT));
    value["name"] = "side10le";
    fs::create_directories(directory.path() / "formats");
    writeText(directory.path() / "formats" / "side10le.json", dvv::json::dump(value, 2));
    const std::string text = R"({"rawreelClip": 1, "format": "@formats/side10le.json",
                                 "size": {"width": 33, "height": 17}})";
    writeText(fs::path(path) += ".json", text);
    const media::Match match = rawMatch(
        utf8(path), {}, media::Sidecar{.path = utf8(path) + ".json", .text = text, .error = {}});
    CHECK(match.confidence == 75);
    CHECK(match.why == "format and size from plain.bin.json");

    SourceHarness harness;
    const auto source = harness.open(utf8(path));
    CHECK(media::descriptor(source->info().desc.format).name == "side10le");
    CHECK(source->info().desc.width == WIDTH);
    CHECK(source->info().warnings.empty());
    CHECK(frameBytes(harness, source, 2) == packedFrame(FORMAT, WIDTH, HEIGHT, SEED, 2));
}

TEST_CASE("raw: on RGB and gray the clip's range shows through colour= (M4-D5-RGBRANGE)") {
    const TemporaryDirectory directory;
    for (const std::string_view format : {"rgb24", "gray"}) {
        CAPTURE(format);
        const fs::path clip = directory.path() / (std::string(format) + ".bin");
        writeFile(clip, packedFrames(format, 8, 4, SEED, 2));
        const std::string text =
            R"({"rawreelClip": 1, "format": ")" + std::string(format) +
            R"(", "size": {"width": 8, "height": 4}, "colour": {"standard": "bt2020",
                "range": "limited"}})";
        const fs::path desc = directory.path() / (std::string(format) + ".desc.json");
        writeText(desc, text);
        writeText(fs::path(clip) += ".json", text);
        SourceHarness harness;
        // The sidecar, then clip=.
        for (const std::string& uri :
             {utf8(clip) + "?", utf8(clip) + "?clip=@" + utf8(desc) + "&"}) {
            CAPTURE(uri);
            const auto preset = harness.open(uri + "colour=bt709");
            CHECK(media::colourParams(preset->info().desc.colour).range ==
                  media::ColourRange::Limited);
            CHECK(media::resolveColour(preset->info().desc).params.range ==
                  media::ColourRange::Limited);
            // A field key beats the clip's colour.
            const auto keyed = harness.open(uri + "colour=bt709&range=full");
            CHECK(media::colourParams(keyed->info().desc.colour).range == media::ColourRange::Full);
        }
    }
}

TEST_CASE("raw: the file-name colour token (D5R §3.4)") {
    const TemporaryDirectory directory;
    const fs::path path = writeDefault(directory.path(), "clip_33x17_yuv420p10le_bt2100-hlg.yuv");
    const fs::path plain = writeDefault(directory.path(), "clip_33x17_yuv420p10le.yuv");
    const fs::path sized = writeDefault(directory.path(), "clip_33x17_bt709.yuv");

    // The colour does not decide the match: the confidence and the why stay.
    const media::Match match = rawMatch(utf8(path));
    CHECK(match.confidence == 70);
    CHECK(match.why == "size and format from the file name");
    CHECK(rawMatch(utf8(plain)).confidence == 70);
    CHECK(rawMatch(utf8(sized)).confidence == 30);
    CHECK(rawMatch(utf8(sized)).why == "size only; format guessed as yuv420p");

    SourceHarness harness;
    const auto source = harness.open(utf8(path));
    const media::SourceInfo& info = source->info();
    CHECK(info.desc.format == idOf(FORMAT));
    CHECK(info.desc.width == WIDTH);
    CHECK(info.desc.colour == queryColour(harness, plain, "colour=bt2100-hlg"));
    // The token gives no range; the resolved URI does not carry it (a reopen reads the name).
    CHECK(media::colourParams(info.desc.colour).range == media::ColourRange::Unspecified);
    CHECK(info.resolvedUri.find("colour=") == std::string::npos);
    CHECK(harness.open(info.resolvedUri)->info().desc == info.desc);
    // colour= beats the token; range= goes with it.
    CHECK(harness.open(utf8(path) + "?colour=bt709")->info().desc.colour ==
          queryColour(harness, plain, "colour=bt709"));
    CHECK(harness.open(utf8(path) + "?range=full")->info().desc.colour ==
          queryColour(harness, plain, "colour=bt2100-hlg&range=full"));
    // Names without a token give no colour.
    CHECK(media::colourParams(harness.open(utf8(plain))->info().desc.colour) ==
          media::ColourParams{});
    // (A size-only name is ambiguous at 30; impl=raw picks the provider.)
    CHECK(harness.open(utf8(sized) + "?impl=raw")->info().desc.colour ==
          harness.open(utf8(sized) + "?impl=raw&colour=bt709")->info().desc.colour);

    // The sidecar's colour beats the token, and clip= beats both.
    writeText(fs::path(path) += ".json",
              R"({"rawreelClip": 1, "colour": {"standard": "bt601-pal"}})");
    CHECK(harness.open(utf8(path))->info().desc.colour ==
          queryColour(harness, plain, "colour=bt601-pal"));
    CHECK(rawMatch(utf8(path)).confidence == 70);
    const fs::path desc = directory.path() / "desc.json";
    writeText(desc, R"({"rawreelClip": 1, "colour": {"standard": "bt2020", "range": "full"}})");
    CHECK(harness.open(utf8(path) + "?clip=@" + utf8(desc))->info().desc.colour ==
          queryColour(harness, plain, "colour=bt2020&range=full"));
    // A clip description without a colour leaves the token in use.
    const fs::path sizeless = directory.path() / "fps.json";
    writeText(sizeless, R"({"rawreelClip": 1, "frameRate": 50})");
    CHECK(harness.open(utf8(path) + "?clip=@" + utf8(sizeless))->info().desc.colour ==
          queryColour(harness, plain, "colour=bt2100-hlg"));
}

TEST_CASE("raw: a bad clip= fails the open") {
    const TemporaryDirectory directory;
    const fs::path path = writeDefault(directory.path());
    const fs::path bad = directory.path() / "bad.json";
    writeText(bad, R"({"rawreelClip": 1, "size": {"w": 1}})");
    const fs::path no_size = directory.path() / "fps.json";
    writeText(no_size, R"({"rawreelClip": 1, "frameRate": 30})");

    SourceHarness harness;
    const dvv::Error parse = openError(harness, utf8(path) + "?clip=@" + utf8(bad));
    CHECK(parse.code == dvv::Errc::Format);
    CHECK(parse.message.find("size: unknown key 'w'") != std::string::npos);
    CHECK(openError(harness, utf8(path) + "?clip=desc.json").message == "clip needs @<file.json>");
    CHECK(openError(harness, utf8(path) + "?clip=@" + utf8(directory.path() / "none.json")).code ==
          dvv::Errc::Io);
    CHECK(openError(harness, utf8(path) + "?clip=@" + utf8(no_size)).message ==
          "no frame size: " + utf8(no_size) +
              " has no size; name the file <name>_WxH[_fmt] or pass ?size=WxH&fmt=");
    // A size key completes it.
    CHECK(harness.open(utf8(path) + "?clip=@" + utf8(no_size) + "&size=33x17&fmt=yuv420p10le")
              ->info()
              .fps == media::Rational{30, 1});
}
