#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/uri.h"
#include "dvv/media/colour_preset.h"
#include "dvv/media/display.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/reference.h"
#include "dvv/media/source.h"
#include "dvv/media/source_factory.h"
#include "dvv/media/types.h"
#include "dvv/registry/capabilities.h"
#include "dvv/registry/component_registry.h"
#include "patterns.h"
#include "source_harness.h"
#include "sources/y4m_header.h"
#include "temporary_directory.h"

namespace media = dvv::media;
namespace detail = dvv::media::detail;
namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t WIDTH = 33;
constexpr std::uint32_t HEIGHT = 17;
constexpr std::uint64_t SEED = 9;
// The header of the "every tag" case: yuv420p10le 33x17 at 30000/1001, full range.
constexpr std::string_view FULL_HEADER =
    "YUV4MPEG2 W33 H17 F30000:1001 Ip A1:1 C420p10 XYSCSS=420P10 XCOLORRANGE=FULL\n";
constexpr std::string_view FULL_FORMAT = "yuv420p10le";

media::FormatId idOf(std::string_view name) {
    const auto id = media::findFormat(name);
    REQUIRE_MESSAGE(id.has_value(), std::string(name));
    return id.value_or(media::FormatId{0});
}

// pack(generate(Noise, ...)) of frame `index`.
std::vector<std::byte> packedFrame(std::string_view format, std::int64_t index) {
    const auto values =
        detail::generate(detail::Pattern::Noise, idOf(format), WIDTH, HEIGHT, SEED, index);
    REQUIRE_MESSAGE(values.has_value(), values.error().describe());
    auto bytes = media::pack(*values);
    REQUIRE_MESSAGE(bytes.has_value(), bytes.error().describe());
    return *bytes;
}

void append(std::vector<std::byte>& bytes, std::string_view text) {
    const auto view = std::as_bytes(std::span(text));
    bytes.insert(bytes.end(), view.begin(), view.end());
}

// The header text, then per frame its marker (FRAME\n unless `markers` names another) and the
// packed noise of `format`.
std::vector<std::byte> y4mBytes(std::string_view header, std::string_view format,
                                std::int64_t frames, const std::vector<std::string>& markers = {}) {
    std::vector<std::byte> bytes;
    append(bytes, header);
    for (std::int64_t index = 0; index < frames; ++index) {
        const auto slot = static_cast<std::size_t>(index);
        append(bytes, slot < markers.size() ? std::string_view(markers[slot]) : "FRAME\n");
        const std::vector<std::byte> frame = packedFrame(format, index);
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

// The path as UTF-8 URI text (a bare path).
std::string utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.c_str()), text.size()};
}

// The default file: FULL_HEADER and 3 frames.
fs::path writeFull(const fs::path& directory, std::string_view name = "clip.y4m") {
    const fs::path path = directory / name;
    writeFile(path, y4mBytes(FULL_HEADER, FULL_FORMAT, 3));
    return path;
}

std::vector<std::byte> frameBytes(SourceHarness& harness,
                                  const std::shared_ptr<media::ISource>& source,
                                  std::int64_t index) {
    auto bytes = harness.originalBytes(source, index);
    REQUIRE_MESSAGE(bytes.has_value(), bytes.error().describe());
    return *bytes;
}

dvv::Error openError(SourceHarness& harness, std::string_view uri) {
    INFO(std::string(uri));
    auto opened = harness.tryOpen(uri);
    REQUIRE_FALSE(opened.has_value());
    return opened.error();
}

dvv::Result<detail::Y4mHeader> parse(std::string_view text) {
    return detail::parseY4mHeader(std::as_bytes(std::span(text)));
}

// The error of a header that must not parse.
dvv::Error headerError(std::string_view text) {
    INFO(std::string(text));
    auto header = parse(text);
    REQUIRE_FALSE(header.has_value());
    INFO(header.error().describe());
    return header.error();
}

// A provider's own match for a URI and a head, outside the factory.
media::Match matchOf(std::string_view id, std::string_view uri, std::span<const std::byte> head) {
    INFO(std::string(uri));
    const auto registry = dvv::ComponentRegistry::create(dvv::Capabilities{});
    auto provider = registry->createComponent<media::ISourceProvider>(id);
    REQUIRE_MESSAGE(provider.has_value(), provider.error().describe());
    auto parsed = dvv::parseUri(uri);
    REQUIRE_MESSAGE(parsed.has_value(), parsed.error().describe());
    const media::SourceRequest request{.uri = std::move(*parsed), .hint = {}};
    return (*provider)->match(request, head);
}

}  // namespace

TEST_CASE("y4m: header with every tag") {
    const TemporaryDirectory directory;
    const fs::path path = writeFull(directory.path());
    const std::vector<std::byte> head = y4mBytes(FULL_HEADER, FULL_FORMAT, 1);
    const media::Match match = matchOf("y4m", utf8(path), head);
    CHECK(match.confidence == 100);
    CHECK(match.why == "YUV4MPEG2 magic");
    CHECK(match.guess.format == idOf(FULL_FORMAT));
    CHECK(match.guess.width == WIDTH);
    CHECK(match.guess.height == HEIGHT);
    CHECK(match.guess.fps == media::Rational{30000, 1001});

    SourceHarness harness;
    const auto source = harness.open(utf8(path));
    const media::SourceInfo& info = source->info();
    CHECK(info.provider == "y4m");
    CHECK(info.desc.format == idOf(FULL_FORMAT));
    CHECK(info.desc.width == WIDTH);
    CHECK(info.desc.height == HEIGHT);
    CHECK(info.fps == media::Rational{30000, 1001});
    CHECK(media::colourParams(info.desc.colour).range == media::ColourRange::Full);
    CHECK(info.frameCount == 3);
    CHECK(info.access == media::SourceAccess::RandomCheap);
    CHECK(info.warnings.empty());
    CHECK(info.reader == fileReadCases().back().reader);  // the default, Auto
    for (std::int64_t index = 0; index < 3; ++index) {
        INFO(index);
        CHECK(frameBytes(harness, source, index) == packedFrame(FULL_FORMAT, index));
    }
    const auto outside = harness.originalBytes(source, 3);
    REQUIRE_FALSE(outside.has_value());
    CHECK(outside.error().code == dvv::Errc::Internal);
}

TEST_CASE("y4m: every C mapping") {
    struct Mapping {
        std::string_view colourspace;
        std::string_view format;
    };
    constexpr Mapping MAPPINGS[] = {
        {"420jpeg", "yuv420p"},    {"420paldv", "yuv420p"},   {"420mpeg2", "yuv420p"},
        {"420", "yuv420p"},        {"422", "yuv422p"},        {"444", "yuv444p"},
        {"mono", "gray"},          {"420p10", "yuv420p10le"}, {"422p10", "yuv422p10le"},
        {"444p10", "yuv444p10le"}, {"420p12", "yuv420p12le"}, {"422p12", "yuv422p12le"},
        {"444p12", "yuv444p12le"}, {"420p16", "yuv420p16le"}, {"444p16", "yuv444p16le"},
        {"mono16", "gray16le"},
    };
    static_assert(std::size(MAPPINGS) == 16);
    for (const Mapping& mapping : MAPPINGS) {
        const std::string text = "YUV4MPEG2 W33 H17 C" + std::string(mapping.colourspace) + "\n";
        INFO(text);
        const auto header = parse(text);
        REQUIRE_MESSAGE(header.has_value(), header.error().describe());
        CHECK(header->format == idOf(mapping.format));
        CHECK(header->bytes == text.size());
    }

    const auto missing = parse("YUV4MPEG2 W33 H17\n");
    REQUIRE_MESSAGE(missing.has_value(), missing.error().describe());
    CHECK(missing->format == idOf("yuv420p"));
    CHECK(missing->fps == media::Rational{25, 1});
    CHECK(missing->width == 33);
    CHECK(missing->height == 17);

    for (const std::string_view text :
         {"YUV4MPEG2 W33 H17 C411\n", "YUV4MPEG2 W33 H17 Cmono10\n"}) {
        const dvv::Error error = headerError(text);
        CHECK(error.code == dvv::Errc::Unsupported);
        CHECK(error.message.find("is not supported") != std::string::npos);
    }
    CHECK(headerError("YUV4MPEG2 W33 H17 C411\n").message ==
          "Y4M colourspace '411' is not supported");
}

TEST_CASE("y4m: header errors") {
    struct Case {
        std::string text;
        dvv::Errc code;
    };
    const std::vector<Case> cases{
        {"YUV4MPEG2 H17\n", dvv::Errc::Format},
        {"YUV4MPEG2 W0 H17\n", dvv::Errc::Format},
        {"YUV4MPEG2 W33 H17 F30:0\n", dvv::Errc::Format},
        {"YUV4MPEG2 W33 H17 Q1\n", dvv::Errc::Format},
        {"YUV4MPEG2 W33 H17 XCOLORRANGE=WIDE\n", dvv::Errc::Format},
        {"YUV4MPEG2 W33 H17\r\n", dvv::Errc::Unsupported},
        {"YUV4MPEG2 W33 H17 " + std::string(4096, 'X'), dvv::Errc::Format},
        {"YUV4MPEG W33 H17\n", dvv::Errc::Format},
    };
    for (const Case& entry : cases) {
        CHECK(headerError(entry.text).code == entry.code);
    }
    CHECK(headerError("YUV4MPEG2 W33 H17 Q1\n").message == "unknown Y4M header tag 'Q'");
    CHECK(headerError("YUV4MPEG2 W33 H17\r\n").message == "Y4M header ends with CR LF");
    CHECK(headerError("YUV4MPEG2 W33 H17 " + std::string(4096, 'X')).message ==
          "Y4M header longer than 4096 bytes or not terminated");
    CHECK(headerError("RIFF").message == "not a YUV4MPEG2 file");
}

TEST_CASE("y4m: XCOLORRANGE") {
    const auto limited = parse("YUV4MPEG2 W33 H17 XCOLORRANGE=LIMITED\n");
    REQUIRE_MESSAGE(limited.has_value(), limited.error().describe());
    CHECK(limited->range == media::ColourRange::Limited);
    const auto absent = parse("YUV4MPEG2 W33 H17 XOTHER=1\n");
    REQUIRE_MESSAGE(absent.has_value(), absent.error().describe());
    CHECK(absent->range == media::ColourRange::Unspecified);
}

TEST_CASE("y4m: frame parameters are Unsupported") {
    const TemporaryDirectory directory;
    const fs::path path = directory.path() / "params.y4m";
    writeFile(path, y4mBytes("YUV4MPEG2 W33 H17 C420\n", "yuv420p", 3, {"FRAME\n", "FRAME Ip\n"}));

    SourceHarness harness;
    const auto source = harness.open(utf8(path));
    CHECK(frameBytes(harness, source, 0) == packedFrame("yuv420p", 0));
    const auto second = harness.originalBytes(source, 1);
    REQUIRE_FALSE(second.has_value());
    INFO(second.error().describe());
    CHECK(second.error().code == dvv::Errc::Unsupported);
    CHECK(second.error().message.find("frame parameters") != std::string::npos);
    CHECK(second.error().message.find("frame 1 of") != std::string::npos);
}

TEST_CASE("y4m: frames at unaligned offsets") {
    // yuv420p 33x17 is 867 bytes a frame and the header 30-odd bytes: frames cross the 4096-byte
    // boundaries.
    const TemporaryDirectory directory;
    const fs::path path = directory.path() / "odd.y4m";
    constexpr std::string_view HEADER = "YUV4MPEG2 W33 H17 F25:1 Ip A1:1\n";
    writeFile(path, y4mBytes(HEADER, "yuv420p", 5));

    // Every read mode gives the bytes of the CPU reference (U-IO2), the last frame inside a
    // partial last sector.
    SourceHarness harness;
    for (const FileReadCase& mode : fileReadCases()) {
        CAPTURE(mode.reader);
        const auto source = harness.open(utf8(path), mode.mode);
        CHECK(source->info().reader == mode.reader);
        REQUIRE(source->info().frameCount == 5);
        CHECK(source->info().desc.format == idOf("yuv420p"));
        CHECK(media::colourParams(source->info().desc.colour).range ==
              media::ColourRange::Unspecified);
        for (std::int64_t index = 0; index < 5; ++index) {
            INFO(index);
            CHECK(frameBytes(harness, source, index) == packedFrame("yuv420p", index));
        }
    }
}

TEST_CASE("y4m: trailing bytes") {
    const TemporaryDirectory directory;
    const fs::path path = directory.path() / "trailing.y4m";
    std::vector<std::byte> bytes = y4mBytes(FULL_HEADER, FULL_FORMAT, 2);
    bytes.insert(bytes.end(), 3, std::byte{1});
    writeFile(path, bytes);

    SourceHarness harness;
    for (const FileReadCase& mode : fileReadCases()) {
        CAPTURE(mode.reader);
        const auto source = harness.open(utf8(path), mode.mode);
        CHECK(source->info().reader == mode.reader);
        CHECK(source->info().frameCount == 2);
        CHECK(source->info().warnings == std::vector<std::string>{"3 trailing bytes ignored"});
        CHECK(frameBytes(harness, source, 1) == packedFrame(FULL_FORMAT, 1));
    }
}

TEST_CASE("y4m: the magic decides") {
    const TemporaryDirectory directory;
    SourceHarness harness;

    const fs::path clip = directory.path() / "clip.y4m";
    writeFile(clip, y4mBytes("YUV4MPEG2 W33 H17 C420jpeg\n", "yuv420p", 2));
    const auto y4m = harness.open(utf8(clip));
    CHECK(y4m->info().provider == "y4m");
    CHECK(frameBytes(harness, y4m, 1) == packedFrame("yuv420p", 1));

    // No magic: raw opens it from the file name.
    const fs::path headerless = directory.path() / "x_33x17_yuv420p.y4m";
    std::vector<std::byte> frames = packedFrame("yuv420p", 0);
    const std::vector<std::byte> second = packedFrame("yuv420p", 1);
    frames.insert(frames.end(), second.begin(), second.end());
    writeFile(headerless, frames);
    const media::Match none = matchOf("y4m", utf8(headerless), frames);
    CHECK(none.confidence == 0);
    CHECK(none.why == "no YUV4MPEG2 magic");
    CHECK(matchOf("raw", utf8(headerless), frames).confidence == 70);
    const auto raw = harness.open(utf8(headerless));
    CHECK(raw->info().provider == "raw");
    CHECK(raw->info().frameCount == 2);

    // A Y4M file with an explicit raw query: both claim it.
    auto opened = harness.tryOpen(utf8(clip) + "?fmt=yuv420p&size=33x17");
    REQUIRE_MESSAGE(opened.has_value(), opened.error().describe());
    const auto* ambiguous = std::get_if<media::Ambiguous>(&*opened);
    REQUIRE(ambiguous != nullptr);
    REQUIRE(ambiguous->candidates.size() == 2);
    CHECK(ambiguous->candidates[0].component == "y4m");
    CHECK(ambiguous->candidates[0].confidence == 100);
    CHECK(ambiguous->candidates[1].component == "raw");
    CHECK(ambiguous->candidates[1].confidence == 95);
}

TEST_CASE("y4m: a sidecar <file>.json is ignored") {
    const TemporaryDirectory directory;
    const fs::path path = writeFull(directory.path());
    for (const std::string_view text :
         {std::string_view(R"({"rawreelClip": 1, "size": {"width": 16, "height": 8}})"),
          std::string_view(R"({"rawreelClip": 1, "size": {"w": 1}})")}) {
        INFO(std::string(text));
        {
            std::ofstream file(fs::path(path) += ".json", std::ios::binary | std::ios::trunc);
            file << text;
            REQUIRE(file);
        }
        SourceHarness harness;
        const auto source = harness.open(utf8(path));
        const media::SourceInfo& info = source->info();
        CHECK(info.provider == "y4m");
        CHECK(info.desc.width == 33);
        CHECK(info.desc.height == 17);
        CHECK(info.warnings.empty());
        CHECK(info.notes.empty());
        CHECK(info.clipFile.empty());
    }
}

TEST_CASE("y4m: resolved URI reopens") {
    const TemporaryDirectory directory;
    const fs::path path = writeFull(directory.path());
    SourceHarness harness;
    const auto source = harness.open(utf8(path));
    const media::SourceInfo& info = source->info();

    CHECK(info.resolvedUri == "file:" + dvv::percentEncode(utf8(path)) +
                                  "?fmt=yuv420p10le&size=33x17&fps=30000/1001&impl=y4m");

    const auto reopened = harness.open(info.resolvedUri);
    CHECK(reopened->info().provider == "y4m");
    CHECK(reopened->info().desc == info.desc);
    CHECK(reopened->info().fps == info.fps);
    CHECK(reopened->info().frameCount == info.frameCount);
    CHECK(reopened->info().resolvedUri == info.resolvedUri);
    for (std::int64_t index = 0; index < 3; ++index) {
        INFO(index);
        CHECK(frameBytes(harness, reopened, index) == frameBytes(harness, source, index));
    }

    // The resolved URI with another format (param() takes the first fmt, so it is not appended).
    const dvv::Error mismatch =
        openError(harness, utf8(path) + "?size=33x17&fps=30000/1001&impl=y4m&fmt=yuv420p");
    INFO(mismatch.describe());
    CHECK(mismatch.code == dvv::Errc::Format);
    CHECK(mismatch.message == "Y4M header says yuv420p10le");
    CHECK(openError(harness, utf8(path) + "?impl=y4m&size=32x17").message ==
          "Y4M header says 33x17");
    CHECK(openError(harness, utf8(path) + "?impl=y4m&fps=30").message ==
          "Y4M header says 30000/1001");
    CHECK(openError(harness, utf8(path) + "?impl=y4m&range=wide").code == dvv::Errc::Format);
    CHECK(openError(harness, utf8(path) + "?impl=y4m&w=33").code == dvv::Errc::Format);
}

TEST_CASE("y4m: colour query wins over the header") {
    const TemporaryDirectory directory;
    const fs::path path = writeFull(directory.path());  // XCOLORRANGE=FULL
    SourceHarness harness;
    const auto source = harness.open(utf8(path) + "?range=limited&colour=bt2100-pq");
    const media::SourceInfo& info = source->info();
    CHECK(media::colourParams(info.desc.colour) ==
          media::ColourParams{.matrix = media::ColourMatrix::Bt2020Ncl,
                              .range = media::ColourRange::Limited,
                              .transfer = media::TransferFn::Pq,
                              .primaries = media::Primaries::Bt2020,
                              .kr = 0,
                              .kb = 0});
    // The resolved URI keeps the query's form: colour first, then range.
    CHECK(info.resolvedUri == "file:" + dvv::percentEncode(utf8(path)) +
                                  "?fmt=yuv420p10le&size=33x17&fps=30000/1001&colour=bt2100-pq&"
                                  "range=limited&impl=y4m");
    const auto reopened = harness.open(info.resolvedUri);
    CHECK(reopened->info().provider == "y4m");
    CHECK(reopened->info().desc == info.desc);
    CHECK(reopened->info().resolvedUri == info.resolvedUri);

    // A standard without range= keeps the header's range (D5R §1.3); range= is taken with every
    // standard (U-D5-9).
    const auto header_range = harness.open(utf8(path) + "?colour=bt709");
    CHECK(media::colourParams(header_range->info().desc.colour) ==
          media::ColourParams{.matrix = media::ColourMatrix::Bt709,
                              .range = media::ColourRange::Full,
                              .transfer = media::TransferFn::Bt1886,
                              .primaries = media::Primaries::Bt709,
                              .kr = 0,
                              .kb = 0});
    CHECK(header_range->info().resolvedUri == "file:" + dvv::percentEncode(utf8(path)) +
                                                  "?fmt=yuv420p10le&size=33x17&fps=30000/1001&"
                                                  "colour=bt709&impl=y4m");
    const auto limited = harness.open(utf8(path) + "?colour=bt601-ntsc&range=limited");
    CHECK(media::colourParams(limited->info().desc.colour).range == media::ColourRange::Limited);

    const auto bad = openError(harness, utf8(path) + "?colour=bt601");
    CHECK(bad.code == dvv::Errc::Format);
    CHECK(bad.message ==
          "bad colour 'bt601' (bt601-ntsc, bt601-pal, bt709, bt2020, bt2100-pq, bt2100-hlg)");
    const auto removed = openError(harness, utf8(path) + "?matrix=bt709");
    CHECK(removed.code == dvv::Errc::Format);
    CHECK(removed.message ==
          "unknown parameter 'matrix' (set the colour with colour=<standard>: "
          "bt601-ntsc, bt601-pal, bt709, bt2020, bt2100-pq, bt2100-hlg)");
}

TEST_CASE("y4m: a preset leaves the range of a gray stream alone (M4-D5-RGBRANGE)") {
    const TemporaryDirectory directory;
    const fs::path limited_path = directory.path() / "limited.y4m";
    writeFile(limited_path, y4mBytes("YUV4MPEG2 W33 H17 Cmono XCOLORRANGE=LIMITED\n", "gray", 2));
    const fs::path untagged_path = directory.path() / "untagged.y4m";
    writeFile(untagged_path, y4mBytes("YUV4MPEG2 W33 H17 Cmono\n", "gray", 2));
    SourceHarness harness;
    const auto range_of = [&](const fs::path& path, std::string_view query) {
        const auto source = harness.open(utf8(path) + std::string(query));
        CHECK(media::colourParams(source->info().desc.colour).matrix ==
              media::ColourMatrix::Unspecified);
        return media::resolveColour(source->info().desc).params.range;
    };
    // A file tagged limited stays limited under a preset.
    CHECK(range_of(limited_path, "?colour=bt709") == media::ColourRange::Limited);
    // An untagged file stays Full (the default) under a preset.
    CHECK(range_of(untagged_path, "?colour=bt709") == media::ColourRange::Full);
    // An explicit range key still wins, over the tag and the default.
    CHECK(range_of(limited_path, "?colour=bt709&range=full") == media::ColourRange::Full);
    CHECK(range_of(untagged_path, "?colour=bt601-pal&range=limited") ==
          media::ColourRange::Limited);
}
