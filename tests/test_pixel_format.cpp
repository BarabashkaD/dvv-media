#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dvv/media/pixel_format.h"
#include "format_table.h"

namespace media = dvv::media;

namespace {

constexpr std::size_t CATALOGUE_SIZE = 37;
constexpr media::FormatId UNKNOWN_ID = 100000;

// The D3 order (ids 0..36).
constexpr std::array<std::string_view, CATALOGUE_SIZE> NAMES = {
    "gray",        "gray10le",    "gray12le",    "gray16le",    "gray16be",    "yuv420p",
    "yuv422p",     "yuv444p",     "yuv420p10le", "yuv422p10le", "yuv444p10le", "yuv420p12le",
    "yuv422p12le", "yuv444p12le", "yuv420p16le", "yuv444p16le", "yuv420p10be", "nv12",
    "nv21",        "p010le",      "p016le",      "yuyv422",     "uyvy422",     "y210le",
    "v210",        "rgb24",       "bgr24",       "rgba",        "bgra",        "argb",
    "rgb48le",     "rgb48be",     "rgba64le",    "x2rgb10le",   "gbrp",        "gbrp10le",
    "gbrp16le"};

media::FormatId idOf(std::string_view name) {
    const auto id = media::findFormat(name);
    REQUIRE(id.has_value());
    return id.value_or(media::FormatId{0});  // REQUIRE above stops the test case first
}

void checkRejects(const media::PixelFormatDescriptor& descriptor, std::string_view text) {
    const auto result = media::validate(descriptor);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == dvv::Errc::Format);
    CHECK_MESSAGE(result.error().message.find(text) != std::string::npos, result.error().message);
}

struct PlaneSize {
    std::uint32_t stride = 0;
    std::uint32_t rows = 0;
};
struct SizeExpectation {
    std::vector<PlaneSize> planes;
    std::uint64_t bytes = 0;
};
// The geometry at 1x1, 33x17 and 49x3, derived from FFmpeg's av_image_fill_linesizes with
// alignment 1 (not from the descriptor model).
struct GeometryRow {
    std::vector<std::string_view> names;
    std::array<SizeExpectation, 3> sizes;
};

std::vector<PlaneSize> times3(PlaneSize plane) {
    return {plane, plane, plane};
}

std::vector<GeometryRow> geometryTable() {
    return {
        {{"gray"}, {{{{{1, 1}}, 1}, {{{33, 17}}, 561}, {{{49, 3}}, 147}}}},
        {{"gray10le", "gray12le", "gray16le", "gray16be"},
         {{{{{2, 1}}, 2}, {{{66, 17}}, 1122}, {{{98, 3}}, 294}}}},
        {{"yuv420p"},
         {{{times3({1, 1}), 3},
           {{{33, 17}, {17, 9}, {17, 9}}, 867},
           {{{49, 3}, {25, 2}, {25, 2}}, 247}}}},
        {{"yuv422p"},
         {{{times3({1, 1}), 3},
           {{{33, 17}, {17, 17}, {17, 17}}, 1139},
           {{{49, 3}, {25, 3}, {25, 3}}, 297}}}},
        {{"yuv444p"}, {{{times3({1, 1}), 3}, {times3({33, 17}), 1683}, {times3({49, 3}), 441}}}},
        {{"yuv420p10le", "yuv420p12le", "yuv420p16le", "yuv420p10be"},
         {{{times3({2, 1}), 6},
           {{{66, 17}, {34, 9}, {34, 9}}, 1734},
           {{{98, 3}, {50, 2}, {50, 2}}, 494}}}},
        {{"yuv422p10le", "yuv422p12le"},
         {{{times3({2, 1}), 6},
           {{{66, 17}, {34, 17}, {34, 17}}, 2278},
           {{{98, 3}, {50, 3}, {50, 3}}, 594}}}},
        {{"yuv444p10le", "yuv444p12le", "yuv444p16le"},
         {{{times3({2, 1}), 6}, {times3({66, 17}), 3366}, {times3({98, 3}), 882}}}},
        {{"nv12", "nv21"},
         {{{{{1, 1}, {2, 1}}, 3}, {{{33, 17}, {34, 9}}, 867}, {{{49, 3}, {50, 2}}, 247}}}},
        {{"p010le", "p016le"},
         {{{{{2, 1}, {4, 1}}, 6}, {{{66, 17}, {68, 9}}, 1734}, {{{98, 3}, {100, 2}}, 494}}}},
        {{"yuyv422", "uyvy422"}, {{{{{4, 1}}, 4}, {{{68, 17}}, 1156}, {{{100, 3}}, 300}}}},
        {{"y210le"}, {{{{{8, 1}}, 8}, {{{136, 17}}, 2312}, {{{200, 3}}, 600}}}},
        {{"rgb24", "bgr24"}, {{{{{3, 1}}, 3}, {{{99, 17}}, 1683}, {{{147, 3}}, 441}}}},
        {{"rgba", "bgra", "argb", "x2rgb10le"},
         {{{{{4, 1}}, 4}, {{{132, 17}}, 2244}, {{{196, 3}}, 588}}}},
        {{"rgb48le", "rgb48be"}, {{{{{6, 1}}, 6}, {{{198, 17}}, 3366}, {{{294, 3}}, 882}}}},
        {{"rgba64le"}, {{{{{8, 1}}, 8}, {{{264, 17}}, 4488}, {{{392, 3}}, 1176}}}},
        {{"gbrp"}, {{{times3({1, 1}), 3}, {times3({33, 17}), 1683}, {times3({49, 3}), 441}}}},
        {{"gbrp10le", "gbrp16le"},
         {{{times3({2, 1}), 6}, {times3({66, 17}), 3366}, {times3({98, 3}), 882}}}},
        // v210 at 2x2, 34x17 and 50x3: the stride is ceil(w / 48) * 128.
        {{"v210"}, {{{{{128, 2}}, 256}, {{{128, 17}}, 2176}, {{{256, 3}}, 768}}}},
    };
}

}  // namespace

TEST_CASE("the catalogue lists 37 formats in D3 order") {
    CHECK(media::detail::builtInFormats().size() == CATALOGUE_SIZE);
    const auto catalogue = media::catalogue();
    REQUIRE(catalogue.size() == CATALOGUE_SIZE);
    for (std::size_t i = 0; i < CATALOGUE_SIZE; ++i) {
        CAPTURE(i);
        CHECK(catalogue[i] == i);
        CHECK(media::descriptor(catalogue[i]).name == NAMES[i]);
    }
}

TEST_CASE("every catalogue entry validates") {
    for (const media::FormatId id : media::catalogue()) {
        const auto result = media::validate(media::descriptor(id));
        CHECK_MESSAGE(result.has_value(), (result ? std::string() : result.error().message));
    }
    for (const auto& descriptor : media::detail::builtInFormats()) {
        CHECK(media::validate(descriptor).has_value());
    }
}

TEST_CASE("findFormat") {
    CHECK(media::findFormat("v210") == media::FormatId{24});
    CHECK(media::findFormat("gbrp16le") == media::FormatId{36});
    CHECK_FALSE(media::findFormat("V210").has_value());
    CHECK_FALSE(media::findFormat("").has_value());
    CHECK_FALSE(media::findFormat("nope").has_value());
}

TEST_CASE("formatAlias maps FFmpeg's full-range names") {
    CHECK(media::formatAlias("yuvj420p") ==
          media::FormatAlias{idOf("yuv420p"), media::ColourRange::Full});
    CHECK(media::formatAlias("yuvj422p") ==
          media::FormatAlias{idOf("yuv422p"), media::ColourRange::Full});
    CHECK(media::formatAlias("yuvj444p") ==
          media::FormatAlias{idOf("yuv444p"), media::ColourRange::Full});
    for (const std::string_view name : {"yuv420p", "", "YUVJ420P", "yuvj411p", "nope"}) {
        CAPTURE(name);
        CHECK_FALSE(media::formatAlias(name).has_value());
    }
    CHECK_FALSE(media::findFormat("yuvj420p").has_value());  // unchanged
}

TEST_CASE("representative descriptor fields") {
    const auto& p010 = media::descriptor(idOf("p010le"));
    CHECK(p010.planeCount == 2);
    const media::PlaneDesc& chroma = p010.planes[1];
    CHECK(chroma.groupPixels == 2);
    CHECK(chroma.groupBytes == 4);
    CHECK(chroma.wordBytes == 2);
    CHECK(chroma.order == media::ByteOrder::Little);
    CHECK(chroma.log2SubY == 1);
    CHECK(chroma.slotCount == 2);
    CHECK(chroma.slots[0] == media::SampleSlot{1, 0, 0, 6});
    CHECK(chroma.slots[1] == media::SampleSlot{2, 0, 1, 6});

    const auto& v210 = media::descriptor(idOf("v210"));
    CHECK(v210.widthMultiple == 2);
    CHECK(v210.planes[0].rowAlign == 128);
    CHECK(v210.planes[0].slotCount == 12);
    CHECK(v210.planes[0].slots[11] == media::SampleSlot{0, 5, 3, 20});

    CHECK(media::descriptor(idOf("gbrp")).planes[0].slots[0].component == 1);
    CHECK(media::descriptor(idOf("rgb48be")).planes[0].order == media::ByteOrder::Big);
}

TEST_CASE("geometry matches hand-computed sizes") {
    const std::array<std::pair<std::uint32_t, std::uint32_t>, 3> sizes = {
        {{1, 1}, {33, 17}, {49, 3}}};
    const std::array<std::pair<std::uint32_t, std::uint32_t>, 3> v210_sizes = {
        {{2, 2}, {34, 17}, {50, 3}}};
    const std::array<std::uint32_t, 3> v210_row_bytes = {16, 96, 144};

    std::size_t covered = 0;
    for (const GeometryRow& row : geometryTable()) {
        for (const std::string_view name : row.names) {
            ++covered;
            const media::FormatId id = idOf(name);
            const bool is_v210 = name == "v210";
            for (std::size_t s = 0; s < 3; ++s) {
                const auto [width, height] = is_v210 ? v210_sizes[s] : sizes[s];
                CAPTURE(name);
                CAPTURE(width);
                CAPTURE(height);
                const auto geometry = media::geometry(id, width, height);
                REQUIRE(geometry.has_value());
                const SizeExpectation& expected = row.sizes[s];
                REQUIRE(geometry->planeCount == expected.planes.size());
                std::uint64_t offset = 0;
                for (std::size_t p = 0; p < expected.planes.size(); ++p) {
                    CAPTURE(p);
                    const media::PlaneGeometry& plane = geometry->planes[p];
                    CHECK(plane.stride == expected.planes[p].stride);
                    CHECK(plane.rows == expected.planes[p].rows);
                    CHECK(plane.rowBytes == (is_v210 ? v210_row_bytes[s] : plane.stride));
                    CHECK(plane.offset == offset);
                    offset += std::uint64_t{plane.stride} * plane.rows;
                }
                CHECK(offset == expected.bytes);
                CHECK(geometry->bytes == expected.bytes);
            }
        }
    }
    CHECK(covered == CATALOGUE_SIZE);
}

TEST_CASE("v210 stride and width rules") {
    const media::FormatId v210 = idOf("v210");
    const std::array<std::pair<std::uint32_t, std::uint32_t>, 5> strides = {
        {{2, 128}, {48, 128}, {50, 256}, {96, 256}, {98, 384}}};
    for (const auto& [width, stride] : strides) {
        CAPTURE(width);
        const auto geometry = media::geometry(v210, width, 1);
        REQUIRE(geometry.has_value());
        CHECK(geometry->planes[0].stride == stride);
    }
    for (const auto& [width, height] : {std::pair{33U, 17U}, std::pair{1U, 1U}}) {
        CAPTURE(width);
        const auto geometry = media::geometry(v210, width, height);
        REQUIRE_FALSE(geometry.has_value());
        CHECK(geometry.error().code == dvv::Errc::Format);
        CHECK(geometry.error().message == "v210 needs a width that is a multiple of 2");
    }
}

TEST_CASE("geometry rejects bad sizes") {
    const auto expect_format = [](const dvv::Result<media::FrameGeometry>& geometry,
                                  std::string_view text) {
        REQUIRE_FALSE(geometry.has_value());
        CHECK(geometry.error().code == dvv::Errc::Format);
        CHECK_MESSAGE(geometry.error().message.find(text) != std::string::npos,
                      geometry.error().message);
    };
    expect_format(media::geometry(idOf("gray"), 0, 1), "is empty");
    expect_format(media::geometry(idOf("gray"), 1, 0), "is empty");
    expect_format(media::geometry(idOf("gray16le"), 65536, 65536), "exceeds 4 GiB");
    expect_format(media::geometry(idOf("rgba64le"), 0xFFFFFFFFU, 1), "exceeds 4 GiB");

    const auto unknown = media::geometry(UNKNOWN_ID, 1, 1);
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == dvv::Errc::Internal);
}

TEST_CASE("componentSize") {
    using Size = std::pair<std::uint32_t, std::uint32_t>;
    const media::FormatId yuv420p = idOf("yuv420p");
    CHECK(media::componentSize(yuv420p, 0, 33, 17) == Size{33, 17});
    CHECK(media::componentSize(yuv420p, 1, 33, 17) == Size{17, 9});
    CHECK(media::componentSize(idOf("v210"), 2, 34, 17) == Size{17, 17});
    CHECK(media::componentSize(idOf("nv12"), 2, 1, 1) == Size{1, 1});
    CHECK(media::componentSize(yuv420p, 3, 33, 17) == Size{0, 0});
    CHECK(media::componentSize(UNKNOWN_ID, 0, 33, 17) == Size{0, 0});
}

TEST_CASE("unpacked formats") {
    for (const media::FormatId format : media::catalogue()) {
        const media::PixelFormatDescriptor& source = media::descriptor(format);
        CAPTURE(source.name);
        const media::FormatId unpacked = media::unpackedFormat(format);
        CHECK(unpacked != format);
        CHECK(media::isUnpacked(unpacked));
        CHECK_FALSE(media::isUnpacked(format));
        CHECK(media::unpackedFormat(unpacked) == unpacked);
        CHECK(media::unpackedFormat(format) == unpacked);

        const media::PixelFormatDescriptor& derived = media::descriptor(unpacked);
        CHECK(derived.name == source.name + "@planes");
        const auto valid = media::validate(derived);
        CHECK_MESSAGE(valid.has_value(), (valid ? std::string() : valid.error().message));
        CHECK(derived.planeCount == derived.componentCount);
        CHECK(derived.componentCount == source.componentCount);
        for (std::uint8_t i = 0; i < derived.planeCount; ++i) {
            CAPTURE(i);
            const media::PlaneDesc& plane = derived.planes[i];
            CHECK(plane.slotCount == 1);
            CHECK(plane.slots[0] == media::SampleSlot{i, 0, 0, 0});
            CHECK(plane.wordBytes == (derived.components[i].bits <= 8 ? 1 : 2));
        }
    }

    const media::FormatId v210 = idOf("v210");
    CHECK(media::findFormat("v210@planes") == media::unpackedFormat(v210));
    const auto from_v210 = media::geometry(media::unpackedFormat(v210), 34, 17);
    const auto from_yuv = media::geometry(idOf("yuv422p10le"), 34, 17);
    REQUIRE(from_v210.has_value());
    REQUIRE(from_yuv.has_value());
    CHECK(*from_v210 == *from_yuv);
    CHECK(media::descriptor(media::unpackedFormat(idOf("gbrp"))).planes[0].slots[0].component == 0);
}

TEST_CASE("unknown ids") {
    CHECK(media::descriptor(UNKNOWN_ID).name.empty());
    CHECK(media::descriptor(UNKNOWN_ID).planeCount == 0);
    CHECK_FALSE(media::isUnpacked(UNKNOWN_ID));
    CHECK(media::unpackedFormat(UNKNOWN_ID) == UNKNOWN_ID);
}

TEST_CASE("validate rejects broken descriptors") {
    const auto copy_of = [](std::string_view name) { return media::descriptor(idOf(name)); };

    SUBCASE("name") {
        for (const char* name : {"", "a b", "a@b", "gray@displays"}) {
            CAPTURE(name);
            auto d = copy_of("gray");
            d.name = name;
            checkRejects(d, "invalid name");
        }
        auto d = copy_of("gray");
        d.name = "gray@planes";
        CHECK(media::validate(d).has_value());
        d.name = "gray@display";
        CHECK(media::validate(d).has_value());
    }
    SUBCASE("component count") {
        auto d = copy_of("gray");
        d.componentCount = 0;
        checkRejects(d, "needs 1 to 4 components");
    }
    SUBCASE("colour model") {
        auto d = copy_of("yuv420p");
        d.components[1].kind = media::ComponentKind::Y;
        checkRejects(d, "components do not match the colour model");
    }
    SUBCASE("bits") {
        for (const std::uint8_t bits : {std::uint8_t{17}, std::uint8_t{0}}) {
            CAPTURE(bits);
            auto d = copy_of("gray10le");
            d.components[0].bits = bits;
            checkRejects(d, "bits must be 1 to 16");
        }
    }
    SUBCASE("subsampling") {
        auto luma = copy_of("yuv420p");
        luma.components[0].log2SubX = 1;
        checkRejects(luma, "invalid subsampling");
        auto wide = copy_of("yuv420p");
        wide.components[1].log2SubX = 3;
        wide.components[2].log2SubX = 3;
        checkRejects(wide, "invalid subsampling");
        auto mismatch = copy_of("yuv420p");
        mismatch.components[2].log2SubY = 0;
        checkRejects(mismatch, "invalid subsampling");
    }
    SUBCASE("widthMultiple") {
        auto d = copy_of("gray");
        d.widthMultiple = 0;
        checkRejects(d, "widthMultiple must be at least 1");
    }
    SUBCASE("wordBytes") {
        auto d = copy_of("gray10le");
        d.planes[0].wordBytes = 3;
        checkRejects(d, "wordBytes must be 1, 2 or 4");
    }
    SUBCASE("groupBytes") {
        auto d = copy_of("rgb48le");
        d.planes[0].groupBytes = 5;
        checkRejects(d, "groupBytes must be a non-zero multiple of wordBytes");
    }
    SUBCASE("groupPixels") {
        auto d = copy_of("v210");
        d.widthMultiple = 4;
        checkRejects(d, "groupPixels must be a non-zero multiple of widthMultiple");
    }
    SUBCASE("rowAlign") {
        for (const std::uint16_t align : {std::uint16_t{3}, std::uint16_t{8192}}) {
            CAPTURE(align);
            auto d = copy_of("gray");
            d.planes[0].rowAlign = align;
            checkRejects(d, "rowAlign must be a power of two up to 4096");
        }
    }
    SUBCASE("slot count") {
        auto d = copy_of("gray");
        d.planes[0].slotCount = 0;
        checkRejects(d, "needs 1 to 12 slots");
    }
    SUBCASE("slot component") {
        auto d = copy_of("gray");
        d.planes[0].slots[0].component = 5;
        checkRejects(d, "component out of range");
    }
    SUBCASE("slot outside its word") {
        auto shifted = copy_of("p010le");
        shifted.planes[0].slots[0].shift = 7;
        checkRejects(shifted, "outside its word");
        auto beyond = copy_of("yuyv422");
        beyond.planes[0].slots[0].word = 4;
        checkRejects(beyond, "outside its word");
    }
    SUBCASE("overlapping slots") {
        auto d = copy_of("x2rgb10le");
        d.planes[0].slots[1].shift = 15;
        checkRejects(d, "slots 0 and 1 overlap");
    }
    SUBCASE("component in two planes") {
        auto d = copy_of("gbrp");
        d.planes[1].slots[0].component = 1;
        checkRejects(d, "component 1 is in more than one plane");
    }
    SUBCASE("component in no plane") {
        auto d = copy_of("yuv420p");
        d.planeCount = 2;
        checkRejects(d, "component 2 is in no plane");
    }
    SUBCASE("plane log2SubY") {
        auto d = copy_of("yuv420p");
        d.planes[1].log2SubY = 0;
        checkRejects(d, "log2SubY differs from component 1");
    }
    SUBCASE("samples do not match the group") {
        auto d = copy_of("yuyv422");
        d.planes[0].slots[2].index = 0;
        checkRejects(d, "samples of component 0 do not match the group");
    }
    SUBCASE("GPU sample limit") {
        media::PixelFormatDescriptor d;
        d.name = "wide";
        d.model = media::ColourModel::Gray;
        d.componentCount = 1;
        d.components[0] = {.kind = media::ComponentKind::Y, .bits = 8};
        d.planeCount = 1;
        media::PlaneDesc& plane = d.planes[0];
        plane.groupPixels = 7;
        plane.groupBytes = 7;
        plane.wordBytes = 1;
        plane.slotCount = 7;
        for (std::uint8_t k = 0; k < 7; ++k) {
            plane.slots[k] = {.component = 0, .index = k, .word = k, .shift = 0};
        }
        checkRejects(d, "more than 6 samples of component 0 per group (GPU limit)");
    }
}
