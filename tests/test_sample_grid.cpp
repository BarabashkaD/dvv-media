#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/reference.h"
#include "dvv/media/sample_grid.h"
#include "dvv/media/types.h"
#include "dvv/media/view.h"
#include "dvv/platform/pixel_size.h"
#include "format_table.h"
#include "patterns.h"

namespace media = dvv::media;

namespace {

constexpr std::uint32_t FRAME_WIDTH = 37;
constexpr std::uint32_t FRAME_HEIGHT = 23;

std::uint32_t roundUp(std::uint32_t value, std::uint32_t multiple) {
    return (value + multiple - 1) / multiple * multiple;
}

media::FormatId idOf(std::string_view name) {
    const auto id = media::findFormat(name);
    REQUIRE_MESSAGE(id.has_value(), name);
    return id.value_or(media::FormatId{0});  // REQUIRE above stops the test case first
}

media::StoredWord wordOf(media::FormatId format, std::uint32_t width, std::uint32_t height,
                         std::size_t component, media::SamplePosition sample) {
    const auto word = media::storedWord(format, width, height, component, sample);
    REQUIRE_MESSAGE(word.has_value(), (word.has_value() ? std::string{} : word.error().describe()));
    return *word;
}

std::uint32_t paddingMaskOf(std::string_view name, std::size_t component) {
    return wordOf(idOf(name), 4, 2, component, {}).paddingMask;
}

// Every sample of every component reads back the code that unpack() gives, through its stored
// word; no padding bit is set in packed bytes.
void checkReadBack(media::FormatId format) {
    constexpr std::uint32_t WIDTH = 38;
    constexpr std::uint32_t HEIGHT = 23;
    constexpr std::uint64_t SEED = 7;
    const media::PixelFormatDesc& desc = media::formatDesc(format);
    CAPTURE(desc.name);
    const std::uint32_t width = roundUp(WIDTH, desc.widthMultiple);
    const auto values =
        media::detail::generate(media::detail::Pattern::Noise, format, width, HEIGHT, SEED, 0);
    REQUIRE(values.has_value());
    const auto bytes = media::pack(*values);
    REQUIRE(bytes.has_value());
    const auto codes = media::unpack(format, width, HEIGHT, *bytes);
    REQUIRE(codes.has_value());
    for (std::size_t c = 0; c < desc.componentCount; ++c) {
        CAPTURE(c);
        const auto [grid_width, grid_height] = media::componentSize(format, c, width, HEIGHT);
        for (std::uint32_t y = 0; y < grid_height; ++y) {
            for (std::uint32_t x = 0; x < grid_width; ++x) {
                CAPTURE(x);
                CAPTURE(y);
                const media::StoredWord word = wordOf(format, width, HEIGHT, c, {.x = x, .y = y});
                const std::uint32_t stored = media::readStoredWord(*bytes, 0, word);
                REQUIRE(media::codeOf(stored, word.shift, word.bits) ==
                        codes->components[c][(std::size_t{y} * grid_width) + x]);
                REQUIRE((stored & word.paddingMask) == 0);
            }
        }
        // Outside the grid: Internal.
        const auto outside = media::storedWord(format, width, HEIGHT, c, {.x = grid_width});
        REQUIRE_FALSE(outside.has_value());
        CHECK(outside.error().code == dvv::Errc::Internal);
    }
}

}  // namespace

TEST_CASE("sample grid: sampleOf and blockOf for every catalogue component") {
    for (const media::FormatId format : media::catalogue()) {
        const media::PixelFormatDesc& desc = media::formatDesc(format);
        CAPTURE(desc.name);
        const dvv::platform::PixelSize image{.width = roundUp(FRAME_WIDTH, desc.widthMultiple),
                                             .height = FRAME_HEIGHT};
        for (std::size_t k = 0; k < desc.componentCount; ++k) {
            CAPTURE(k);
            const media::ComponentDesc& component = desc.components[k];
            std::vector<int> covered(std::size_t{image.width} * image.height, 0);
            const auto [sample_width, sample_height] =
                media::componentSize(format, k, image.width, image.height);
            for (std::uint32_t sy = 0; sy < sample_height; ++sy) {
                for (std::uint32_t sx = 0; sx < sample_width; ++sx) {
                    const media::PixelBlock block =
                        media::blockOf(component, {.x = sx, .y = sy}, image);
                    REQUIRE(block.width > 0);
                    REQUIRE(block.height > 0);
                    REQUIRE(block.x + block.width <= image.width);
                    REQUIRE(block.y + block.height <= image.height);
                    for (std::uint32_t y = block.y; y < block.y + block.height; ++y) {
                        for (std::uint32_t x = block.x; x < block.x + block.width; ++x) {
                            ++covered[(std::size_t{y} * image.width) + x];
                        }
                    }
                }
            }
            for (std::uint32_t y = 0; y < image.height; ++y) {
                for (std::uint32_t x = 0; x < image.width; ++x) {
                    CAPTURE(x);
                    CAPTURE(y);
                    // The blocks tile the image with no overlap.
                    REQUIRE(covered[(std::size_t{y} * image.width) + x] == 1);
                    const media::SamplePosition sample = media::sampleOf(component, {x, y});
                    const media::PixelBlock block = media::blockOf(component, sample, image);
                    // Every pixel lies in the block of its sample.
                    REQUIRE(x >= block.x);
                    REQUIRE(x < block.x + block.width);
                    REQUIRE(y >= block.y);
                    REQUIRE(y < block.y + block.height);
                }
            }
            // Empty outside the component's grid.
            CHECK(media::blockOf(component, {.x = sample_width, .y = 0}, image) ==
                  media::PixelBlock{});
            CHECK(media::blockOf(component, {.x = 0, .y = sample_height}, image) ==
                  media::PixelBlock{});
        }
    }
}

TEST_CASE("sample grid: a 4:2:0 chroma block at the odd last column is one pixel wide") {
    const media::ComponentDesc chroma{
        .kind = media::ComponentKind::U, .bits = 8, .log2SubX = 1, .log2SubY = 1};
    const dvv::platform::PixelSize image{.width = FRAME_WIDTH, .height = FRAME_HEIGHT};
    const media::SamplePosition sample = media::sampleOf(chroma, {36, 20});
    CHECK(sample == media::SamplePosition{.x = 18, .y = 10});
    CHECK(media::blockOf(chroma, sample, image) ==
          media::PixelBlock{.x = 36, .y = 20, .width = 1, .height = 2});
    // The last row of an odd height: one pixel high.
    CHECK(media::blockOf(chroma, {.x = 0, .y = 11}, image) ==
          media::PixelBlock{.x = 0, .y = 22, .width = 2, .height = 1});
}

TEST_CASE("stored words: every catalogue format reads back unpack()'s codes") {
    for (const media::FormatId format : media::catalogue()) {
        checkReadBack(format);
    }
}

TEST_CASE("stored words: padding masks") {
    CHECK(paddingMaskOf("p010le", 0) == 0x003FU);
    CHECK(paddingMaskOf("p010le", 1) == 0x003FU);
    CHECK(paddingMaskOf("y210le", 0) == 0x003FU);
    for (const std::string_view name : {"yuv420p10le", "yuv420p10be", "gray10le"}) {
        CAPTURE(name);
        CHECK(paddingMaskOf(name, 0) == 0xFC00U);
    }
    CHECK(paddingMaskOf("gray12le", 0) == 0xF000U);
    CHECK(paddingMaskOf("v210", 0) == 0xC0000000U);
    CHECK(paddingMaskOf("x2rgb10le", 0) == 0xC0000000U);
    for (const std::string_view name :
         {"p016le", "yuv420p", "nv12", "yuyv422", "rgb24", "rgba64le", "gbrp16le"}) {
        CAPTURE(name);
        CHECK(paddingMaskOf(name, 0) == 0U);
    }
    // v210 component 0: its word holds three samples (Cb0 Y0 Cr0).
    const media::StoredWord y0 = wordOf(idOf("v210"), 12, 1, 0, {});
    CHECK(y0.sharedCount == 3);
    CHECK(y0.sharedShifts[0] == 0);
    CHECK(y0.sharedShifts[1] == 10);
    CHECK(y0.sharedShifts[2] == 20);
    CHECK(y0.shift == 10);
    CHECK(y0.wordBytes == 4);
    CHECK(y0.bits == 10);
}

TEST_CASE("stored words: a custom 4:1:1 format") {
    const auto component = [](media::ComponentKind kind, std::uint8_t log2_sub_x) {
        return media::ComponentDesc{.kind = kind, .bits = 8, .log2SubX = log2_sub_x};
    };
    const auto plane = [](std::uint8_t component_index, std::uint8_t group_pixels) {
        media::PlaneDesc result{.groupPixels = group_pixels, .slotCount = 1};
        result.slots[0] = {.component = component_index};
        return result;
    };
    media::PixelFormatDesc format{.name = "test-yuv411p",
                                  .model = media::ColourModel::Yuv,
                                  .componentCount = 3,
                                  .planeCount = 3};
    format.components = {component(media::ComponentKind::Y, 0),
                         component(media::ComponentKind::U, 2),
                         component(media::ComponentKind::V, 2)};
    format.planes = {plane(0, 1), plane(1, 4), plane(2, 4)};
    const dvv::Result<void> valid = media::validate(format);
    REQUIRE_MESSAGE(valid.has_value(),
                    (valid.has_value() ? std::string{} : valid.error().describe()));
    const dvv::Result<media::FormatId> id = media::detail::internCustomFormat(format);
    REQUIRE_MESSAGE(id.has_value(), (id.has_value() ? std::string{} : id.error().describe()));
    checkReadBack(*id);
}

TEST_CASE("stored words: readStoredWord reads a window and is 0 outside it") {
    const std::vector<std::byte> bytes{std::byte{0x12}, std::byte{0x34}, std::byte{0x56},
                                       std::byte{0x78}};
    media::StoredWord word{.byteOffset = 101, .wordBytes = 2};
    CHECK(media::readStoredWord(bytes, 100, word) == 0x5634U);
    word.order = media::ByteOrder::Big;
    CHECK(media::readStoredWord(bytes, 100, word) == 0x3456U);
    word.byteOffset = 103;  // the word's second byte is past the window
    CHECK(media::readStoredWord(bytes, 100, word) == 0U);
    word.byteOffset = 99;  // before the window
    CHECK(media::readStoredWord(bytes, 100, word) == 0U);
    word = {.byteOffset = 0, .wordBytes = 4};
    CHECK(media::readStoredWord(bytes, 0, word) == 0x78563412U);
    CHECK(media::codeOf(0x78563412U, 4, 8) == 0x41U);
}

TEST_CASE("stored words: unknown formats, components and samples are Internal") {
    const auto unknown = media::storedWord(media::FormatId{0xFFFF}, 4, 2, 0, {});
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == dvv::Errc::Internal);
    const auto component = media::storedWord(idOf("yuv420p"), 4, 2, 3, {});
    REQUIRE_FALSE(component.has_value());
    CHECK(component.error().code == dvv::Errc::Internal);
    const auto row = media::storedWord(idOf("yuv420p"), 4, 2, 1, {.x = 0, .y = 1});
    REQUIRE_FALSE(row.has_value());
    CHECK(row.error().code == dvv::Errc::Internal);
}
