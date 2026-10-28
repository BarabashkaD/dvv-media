#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/reference.h"
#include "dvv/media/source.h"
#include "dvv/media/types.h"
#include "patterns.h"
#include "source_harness.h"

namespace media = dvv::media;
namespace detail = dvv::media::detail;

namespace {

media::FormatId idOf(std::string_view name) {
    const auto id = media::findFormat(name);
    REQUIRE_MESSAGE(id.has_value(), std::string(name));
    return id.value_or(media::FormatId{0});
}

// pack(generate(...)) of one frame: what the synthetic source must produce.
std::vector<std::byte> expectedBytes(detail::Pattern pattern, std::string_view format,
                                     std::uint32_t width, std::uint32_t height, std::uint64_t seed,
                                     std::int64_t index) {
    const auto values = detail::generate(pattern, idOf(format), width, height, seed, index);
    REQUIRE_MESSAGE(values.has_value(), values.error().describe());
    auto bytes = media::pack(*values);
    REQUIRE_MESSAGE(bytes.has_value(), bytes.error().describe());
    return *bytes;
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

}  // namespace

TEST_CASE("synthetic: frames equal the pattern generators") {
    SourceHarness harness;
    const auto noise = harness.open("synthetic:noise?fmt=yuv420p10le&size=33x17&seed=7");
    CHECK(frameBytes(harness, noise, 3) ==
          expectedBytes(detail::Pattern::Noise, "yuv420p10le", 33, 17, 7, 3));
    const auto gradient = harness.open("synthetic:gradient?fmt=v210&size=34x17");
    CHECK(frameBytes(harness, gradient, 0) ==
          expectedBytes(detail::Pattern::Gradient, "v210", 34, 17, 0, 0));
}

TEST_CASE("synthetic: deterministic") {
    SourceHarness harness;
    constexpr std::string_view URI = "synthetic:noise?fmt=yuv422p10le&size=16x8&seed=42";
    const auto first = harness.open(URI);
    const auto second = harness.open(URI);
    const std::vector<std::byte> two = frameBytes(harness, first, 2);
    CHECK(two == frameBytes(harness, second, 2));
    CHECK(two != frameBytes(harness, first, 3));
    CHECK(first->info().deterministic);
    CHECK(first->info().access == media::SourceAccess::RandomCheap);
}

TEST_CASE("synthetic: every catalogue format produces in-range code values") {
    SourceHarness harness;
    for (const media::FormatId format : media::catalogue()) {
        const media::PixelFormatDescriptor& descriptor = media::descriptor(format);
        INFO(descriptor.name);
        const auto source = harness.open("synthetic:gradient?fmt=" + descriptor.name + "&size=6x2");
        CHECK(source->info().desc.format == format);
        const std::vector<std::byte> bytes = frameBytes(harness, source, 0);
        const auto values = media::unpack(format, 6, 2, bytes);
        REQUIRE_MESSAGE(values.has_value(), values.error().describe());
        for (std::size_t c = 0; c < descriptor.componentCount; ++c) {
            const std::uint32_t limit = 1U << descriptor.components[c].bits;
            for (const std::uint16_t value : values->components[c]) {
                CHECK(value < limit);
            }
        }
    }
}

TEST_CASE("synthetic: resolved URI") {
    SourceHarness harness;
    const auto source = harness.open("synthetic:noise?w=34&h=17&fmt=v210&seed=9&range=full");
    const media::SourceInfo& info = source->info();
    CHECK(info.resolvedUri ==
          "synthetic:noise?fmt=v210&size=34x17&fps=25&frames=10&seed=9&range=full");
    CHECK(info.provider == "synthetic");
    CHECK(info.warnings.empty());
    CHECK(info.fps == media::Rational{25, 1});
    CHECK(info.frameCount == 10);
    CHECK(info.desc ==
          media::FrameDesc{.format = idOf("v210"),
                           .colour = media::internColour({.range = media::ColourRange::Full}),
                           .width = 34,
                           .height = 17});

    const auto reopened = harness.open(info.resolvedUri);
    CHECK(reopened->info().desc == info.desc);
    CHECK(reopened->info().fps == info.fps);
    CHECK(reopened->info().frameCount == info.frameCount);
    CHECK(reopened->info().resolvedUri == info.resolvedUri);
    CHECK(frameBytes(harness, reopened, 1) == frameBytes(harness, source, 1));
}

TEST_CASE("synthetic: colour from range alone") {
    SourceHarness harness;
    const auto source = harness.open("synthetic:gradient?fmt=yuv420p&size=4x2&range=limited");
    CHECK(media::colourParams(source->info().desc.colour) ==
          media::ColourParams{.matrix = media::ColourMatrix::Unspecified,
                              .range = media::ColourRange::Limited,
                              .transfer = media::TransferFn::Unspecified,
                              .primaries = media::Primaries::Unspecified,
                              .kr = 0,
                              .kb = 0});
    CHECK(source->info().resolvedUri ==
          "synthetic:gradient?fmt=yuv420p&size=4x2&fps=25&frames=10&seed=1&range=limited");
}

TEST_CASE("synthetic: colour parameters") {
    SourceHarness harness;
    const auto source =
        harness.open("synthetic:noise?fmt=yuv420p&size=4x2&range=full&colour=bt2100-pq");
    const media::SourceInfo& info = source->info();
    CHECK(media::colourParams(info.desc.colour) ==
          media::ColourParams{.matrix = media::ColourMatrix::Bt2020Ncl,
                              .range = media::ColourRange::Full,
                              .transfer = media::TransferFn::Pq,
                              .primaries = media::Primaries::Bt2020,
                              .kr = 0,
                              .kb = 0});
    // The resolved URI keeps the query's form: colour first, then range.
    CHECK(info.resolvedUri ==
          "synthetic:noise?fmt=yuv420p&size=4x2&fps=25&frames=10&seed=1&colour=bt2100-pq&"
          "range=full");
    const auto reopened = harness.open(info.resolvedUri);
    CHECK(reopened->info().desc == info.desc);
    CHECK(reopened->info().resolvedUri == info.resolvedUri);

    // The removed keys name colour=.
    const dvv::Error error =
        openError(harness, "synthetic:noise?fmt=yuv420p&size=4x2&transfer=linear");
    CHECK(error.code == dvv::Errc::Format);
    CHECK(error.message ==
          "unknown parameter 'transfer' (set the colour with colour=<standard>: "
          "bt601-ntsc, bt601-pal, bt709, bt2020, bt2100-pq, bt2100-hlg)");

    // A standard alone leaves the range Unspecified (the default applies at display).
    const auto preset = harness.open("synthetic:noise?fmt=yuv420p&size=4x2&colour=bt601-pal");
    CHECK(media::colourParams(preset->info().desc.colour) ==
          media::ColourParams{.matrix = media::ColourMatrix::Bt601,
                              .range = media::ColourRange::Unspecified,
                              .transfer = media::TransferFn::Bt1886,
                              .primaries = media::Primaries::Bt601_625,
                              .kr = 0,
                              .kb = 0});
    CHECK(preset->info().resolvedUri ==
          "synthetic:noise?fmt=yuv420p&size=4x2&fps=25&frames=10&seed=1&colour=bt601-pal");
}

TEST_CASE("synthetic: parameter errors are Format") {
    SourceHarness harness;
    struct Case {
        std::string_view uri;
        std::string_view message;  // a part of the error message
    };
    const std::vector<Case> cases{
        {"synthetic:gradient?size=4x2", "synthetic source needs fmt"},
        {"synthetic:gradient?fmt=nope&size=4x2", "unknown pixel format 'nope'"},
        {"synthetic:gradient?fmt=yuv420p", "synthetic source needs size=WxH or w= and h="},
        {"synthetic:gradient?fmt=yuv420p&size=0x5", "bad size '0x5'"},
        {"synthetic:gradient?fmt=yuv420p&size=4x2&frames=0", "bad frames '0'"},
        {"synthetic:gradient?fmt=yuv420p&size=4x2&fps=0", "bad fps '0'"},
        {"synthetic:bars?fmt=yuv420p&size=4x2",
         "unknown synthetic pattern 'bars' (gradient, noise)"},
        {"synthetic:gradient?fmt=yuv420p&size=4x2&foo=1", "unknown parameter 'foo'"},
        {"synthetic:gradient?fmt=v210&size=33x17", "multiple of 2"},
    };
    for (const Case& entry : cases) {
        const dvv::Error error = openError(harness, entry.uri);
        INFO(error.describe());
        CHECK(error.code == dvv::Errc::Format);
        CHECK(error.message.find(entry.message) != std::string::npos);
    }
}

TEST_CASE("synthetic: an index outside the source is Internal") {
    SourceHarness harness;
    const auto source = harness.open("synthetic:noise?fmt=gray&size=4x2&frames=2");
    CHECK(source->info().frameCount == 2);
    CHECK(frameBytes(harness, source, 1).size() == 8);
    const auto outside = harness.originalBytes(source, 2);
    REQUIRE_FALSE(outside.has_value());
    INFO(outside.error().describe());
    CHECK(outside.error().code == dvv::Errc::Internal);
}
