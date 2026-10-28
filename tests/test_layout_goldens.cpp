// Hand-derived layout goldens (M2 design D4, R8): every byte below was written from FFmpeg's
// component descriptors (libavutil/pixdesc.c) and the v210 encoder (libavcodec/v210enc.c,
// v210_template.c, v210dec.c), never from the catalogue or by calling pack(). A wrong catalogue
// entry therefore cannot hide behind a pack and an unpack that agree with each other.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "dvv/media/pixel_format.h"
#include "dvv/media/reference.h"

namespace media = dvv::media;

namespace {

// Distinct code values: the top 4 bits name the component (c + 1), the low 4 bits the sample
// (n + 1), and the bits between alternate 1010... so byte-order and shift mistakes show.
std::uint16_t goldenValue(std::size_t c, std::size_t n, unsigned bits) {
    const unsigned pattern = 0xAAAAU & ((1U << (bits - 8U)) - 1U);  // 0 for 8 bits
    return static_cast<std::uint16_t>(((c + 1U) << (bits - 4U)) | (pattern << 4U) | (n + 1U));
}

struct Golden {
    std::string_view format;
    std::uint32_t width, height;
    std::vector<std::uint8_t> bytes;
};

// 2x2 frames (v210: 6x1, 2x1 and 4x1, as a prefix of the 128-byte row). Component order is the
// canonical one: Y, U, V or R, G, B, A. Values: 8 bit 0x11.. (Y/R), 0x21.. (U/G), 0x31.. (V/B),
// 0x41.. (A); 10 bit 0x061.., 0x0A1.., 0x0E1..; 12 bit 0x1A1.., 0x2A1.., 0x3A1..; 16 bit 0x1AA1..,
// 0x2AA1.., 0x3AA1.., 0x4AA1... In a 2x2 frame a 4:2:0 chroma grid is 1x1 and a 4:2:2 chroma grid
// is 1x2 (row 1 uses sample n = 1).
// The table is laid out by hand (one golden per initializer, one plane or row per line), so
// clang-format leaves it alone.
// clang-format off
std::vector<Golden> goldens() {
    return {
        // gray: Y{0,1,0,0,8}.
        {"gray", 2, 2, {0x11, 0x12, 0x13, 0x14}},
        // gray10le, gray12le, gray16le: Y{0,2,0,0,d}, LE; gray16be: BE.
        {"gray10le", 2, 2, {0x61, 0x00, 0x62, 0x00, 0x63, 0x00, 0x64, 0x00}},
        {"gray12le", 2, 2, {0xA1, 0x01, 0xA2, 0x01, 0xA3, 0x01, 0xA4, 0x01}},
        {"gray16le", 2, 2, {0xA1, 0x1A, 0xA2, 0x1A, 0xA3, 0x1A, 0xA4, 0x1A}},
        {"gray16be", 2, 2, {0x1A, 0xA1, 0x1A, 0xA2, 0x1A, 0xA3, 0x1A, 0xA4}},
        // yuv4xxp: Y{0,1,0,0,8} U{1,1,0,0,8} V{2,1,0,0,8}.
        {"yuv420p", 2, 2,
         {0x11, 0x12, 0x13, 0x14,     // Y
          0x21,                       // U
          0x31}},                     // V
        {"yuv422p", 2, 2,
         {0x11, 0x12, 0x13, 0x14,     // Y
          0x21, 0x22,                 // U
          0x31, 0x32}},               // V
        {"yuv444p", 2, 2,
         {0x11, 0x12, 0x13, 0x14,     // Y
          0x21, 0x22, 0x23, 0x24,     // U
          0x31, 0x32, 0x33, 0x34}},   // V
        // yuv4xxpNNle: Y{0,2,0,0,d} U{1,2,0,0,d} V{2,2,0,0,d}, LE.
        {"yuv420p10le", 2, 2,
         {0x61, 0x00, 0x62, 0x00, 0x63, 0x00, 0x64, 0x00,     // Y
          0xA1, 0x00,                                         // U
          0xE1, 0x00}},                                       // V
        {"yuv422p10le", 2, 2,
         {0x61, 0x00, 0x62, 0x00, 0x63, 0x00, 0x64, 0x00,     // Y
          0xA1, 0x00, 0xA2, 0x00,                             // U
          0xE1, 0x00, 0xE2, 0x00}},                           // V
        {"yuv444p10le", 2, 2,
         {0x61, 0x00, 0x62, 0x00, 0x63, 0x00, 0x64, 0x00,     // Y
          0xA1, 0x00, 0xA2, 0x00, 0xA3, 0x00, 0xA4, 0x00,     // U
          0xE1, 0x00, 0xE2, 0x00, 0xE3, 0x00, 0xE4, 0x00}},   // V
        {"yuv420p12le", 2, 2,
         {0xA1, 0x01, 0xA2, 0x01, 0xA3, 0x01, 0xA4, 0x01,     // Y
          0xA1, 0x02,                                         // U
          0xA1, 0x03}},                                       // V
        {"yuv422p12le", 2, 2,
         {0xA1, 0x01, 0xA2, 0x01, 0xA3, 0x01, 0xA4, 0x01,     // Y
          0xA1, 0x02, 0xA2, 0x02,                             // U
          0xA1, 0x03, 0xA2, 0x03}},                           // V
        {"yuv444p12le", 2, 2,
         {0xA1, 0x01, 0xA2, 0x01, 0xA3, 0x01, 0xA4, 0x01,     // Y
          0xA1, 0x02, 0xA2, 0x02, 0xA3, 0x02, 0xA4, 0x02,     // U
          0xA1, 0x03, 0xA2, 0x03, 0xA3, 0x03, 0xA4, 0x03}},   // V
        {"yuv420p16le", 2, 2,
         {0xA1, 0x1A, 0xA2, 0x1A, 0xA3, 0x1A, 0xA4, 0x1A,     // Y
          0xA1, 0x2A,                                         // U
          0xA1, 0x3A}},                                       // V
        {"yuv444p16le", 2, 2,
         {0xA1, 0x1A, 0xA2, 0x1A, 0xA3, 0x1A, 0xA4, 0x1A,     // Y
          0xA1, 0x2A, 0xA2, 0x2A, 0xA3, 0x2A, 0xA4, 0x2A,     // U
          0xA1, 0x3A, 0xA2, 0x3A, 0xA3, 0x3A, 0xA4, 0x3A}},   // V
        // yuv420p10be: the same planes, BE 16-bit words.
        {"yuv420p10be", 2, 2,
         {0x00, 0x61, 0x00, 0x62, 0x00, 0x63, 0x00, 0x64,     // Y
          0x00, 0xA1,                                         // U
          0x00, 0xE1}},                                       // V
        // nv12: Y{0,1,0,0,8} U{1,2,0,0,8} V{1,2,1,0,8}; nv21: U{1,2,1} V{1,2,0}.
        {"nv12", 2, 2,
         {0x11, 0x12, 0x13, 0x14,     // Y
          0x21, 0x31}},               // U V
        {"nv21", 2, 2,
         {0x11, 0x12, 0x13, 0x14,     // Y
          0x31, 0x21}},               // V U
        // p010le: Y{0,2,0,6,10} U{1,4,0,6,10} V{1,4,2,6,10}: value << 6 in LE 16-bit words.
        {"p010le", 2, 2,
         {0x40, 0x18, 0x80, 0x18, 0xC0, 0x18, 0x00, 0x19,     // Y
          0x40, 0x28, 0x40, 0x38}},                           // U V
        // p016le: Y{0,2,0,0,16} U{1,4,0,0,16} V{1,4,2,0,16}.
        {"p016le", 2, 2,
         {0xA1, 0x1A, 0xA2, 0x1A, 0xA3, 0x1A, 0xA4, 0x1A,     // Y
          0xA1, 0x2A, 0xA1, 0x3A}},                           // U V
        // yuyv422: Y{0,2,0,0,8} U{0,4,1,0,8} V{0,4,3,0,8}.
        {"yuyv422", 2, 2,
         {0x11, 0x21, 0x12, 0x31,     // row 0: Y0 U0 Y1 V0
          0x13, 0x22, 0x14, 0x32}},   // row 1: Y2 U1 Y3 V1
        // uyvy422: Y{0,2,1,0,8} U{0,4,0,0,8} V{0,4,2,0,8}.
        {"uyvy422", 2, 2,
         {0x21, 0x11, 0x31, 0x12,     // row 0: U0 Y0 V0 Y1
          0x22, 0x13, 0x32, 0x14}},   // row 1: U1 Y2 V1 Y3
        // y210le: Y{0,4,0,6,10} U{0,8,2,6,10} V{0,8,6,6,10}: value << 6 in LE 16-bit words.
        {"y210le", 2, 2,
         {0x40, 0x18, 0x40, 0x28, 0x80, 0x18, 0x40, 0x38,     // row 0: Y0 U0 Y1 V0
          0xC0, 0x18, 0x80, 0x28, 0x00, 0x19, 0x80, 0x38}},   // row 1: Y2 U1 Y3 V1
        // v210: four LE 32-bit words a | b << 10 | c << 20 per 6 pixels, (a, b, c) = (Cb0, Y0,
        // Cr0), (Y1, Cb1, Y2), (Cr1, Y3, Cb2), (Y4, Cr2, Y5); the rest of the 128-byte row is 0.
        {"v210", 6, 1,
         {0xA1, 0x84, 0x11, 0x0E, 0x62, 0x88, 0x32, 0x06,     // Cb0 Y0 Cr0 | Y1 Cb1 Y2
          0xE2, 0x90, 0x31, 0x0A, 0x65, 0x8C, 0x63, 0x06}},   // Cr1 Y3 Cb2 | Y4 Cr2 Y5
        // A last group of 2 pixels: word 0, and word 1 = Y1 only.
        {"v210", 2, 1,
         {0xA1, 0x84, 0x11, 0x0E, 0x62, 0x00, 0x00, 0x00}},   // Cb0 Y0 Cr0 | Y1
        // A last group of 4 pixels: words 0 and 1, and word 2 = Cr1 | Y3 << 10.
        {"v210", 4, 1,
         {0xA1, 0x84, 0x11, 0x0E, 0x62, 0x88, 0x32, 0x06,     // Cb0 Y0 Cr0 | Y1 Cb1 Y2
          0xE2, 0x90, 0x01, 0x00}},                           // Cr1 Y3
        // rgb24: R{0,3,0} G{0,3,1} B{0,3,2}; bgr24: R{0,3,2} G{0,3,1} B{0,3,0}.
        {"rgb24", 2, 2,
         {0x11, 0x21, 0x31, 0x12, 0x22, 0x32,     // row 0
          0x13, 0x23, 0x33, 0x14, 0x24, 0x34}},   // row 1
        {"bgr24", 2, 2,
         {0x31, 0x21, 0x11, 0x32, 0x22, 0x12,     // row 0
          0x33, 0x23, 0x13, 0x34, 0x24, 0x14}},   // row 1
        // rgba: R{0,4,0} G{0,4,1} B{0,4,2} A{0,4,3}.
        {"rgba", 2, 2,
         {0x11, 0x21, 0x31, 0x41, 0x12, 0x22, 0x32, 0x42,     // row 0
          0x13, 0x23, 0x33, 0x43, 0x14, 0x24, 0x34, 0x44}},   // row 1
        // bgra: R{0,4,2} G{0,4,1} B{0,4,0} A{0,4,3}.
        {"bgra", 2, 2,
         {0x31, 0x21, 0x11, 0x41, 0x32, 0x22, 0x12, 0x42,     // row 0
          0x33, 0x23, 0x13, 0x43, 0x34, 0x24, 0x14, 0x44}},   // row 1
        // argb: R{0,4,1} G{0,4,2} B{0,4,3} A{0,4,0}.
        {"argb", 2, 2,
         {0x41, 0x11, 0x21, 0x31, 0x42, 0x12, 0x22, 0x32,     // row 0
          0x43, 0x13, 0x23, 0x33, 0x44, 0x14, 0x24, 0x34}},   // row 1
        // rgb48le / rgb48be: R{0,6,0,0,16} G{0,6,2,0,16} B{0,6,4,0,16}.
        {"rgb48le", 2, 2,
         {0xA1, 0x1A, 0xA1, 0x2A, 0xA1, 0x3A, 0xA2, 0x1A, 0xA2, 0x2A, 0xA2, 0x3A,     // row 0
          0xA3, 0x1A, 0xA3, 0x2A, 0xA3, 0x3A, 0xA4, 0x1A, 0xA4, 0x2A, 0xA4, 0x3A}},   // row 1
        {"rgb48be", 2, 2,
         {0x1A, 0xA1, 0x2A, 0xA1, 0x3A, 0xA1, 0x1A, 0xA2, 0x2A, 0xA2, 0x3A, 0xA2,     // row 0
          0x1A, 0xA3, 0x2A, 0xA3, 0x3A, 0xA3, 0x1A, 0xA4, 0x2A, 0xA4, 0x3A, 0xA4}},   // row 1
        // rgba64le: R{0,8,0} G{0,8,2} B{0,8,4} A{0,8,6}, depth 16.
        {"rgba64le", 2, 2,
         {0xA1, 0x1A, 0xA1, 0x2A, 0xA1, 0x3A, 0xA1, 0x4A,     // pixel 0
          0xA2, 0x1A, 0xA2, 0x2A, 0xA2, 0x3A, 0xA2, 0x4A,     // pixel 1
          0xA3, 0x1A, 0xA3, 0x2A, 0xA3, 0x3A, 0xA3, 0x4A,     // pixel 2
          0xA4, 0x1A, 0xA4, 0x2A, 0xA4, 0x3A, 0xA4, 0x4A}},   // pixel 3
        // x2rgb10le: R at bit 20, G at bit 10, B at bit 0 of the LE 32-bit pixel.
        {"x2rgb10le", 2, 2,
         {0xE1, 0x84, 0x12, 0x06, 0xE2, 0x88, 0x22, 0x06,     // row 0
          0xE3, 0x8C, 0x32, 0x06, 0xE4, 0x90, 0x42, 0x06}},   // row 1
        // gbrp*: G plane, B plane, R plane (R{2,s,0,0,d} G{0,s,0,0,d} B{1,s,0,0,d}).
        {"gbrp", 2, 2,
         {0x21, 0x22, 0x23, 0x24,     // G
          0x31, 0x32, 0x33, 0x34,     // B
          0x11, 0x12, 0x13, 0x14}},   // R
        {"gbrp10le", 2, 2,
         {0xA1, 0x00, 0xA2, 0x00, 0xA3, 0x00, 0xA4, 0x00,     // G
          0xE1, 0x00, 0xE2, 0x00, 0xE3, 0x00, 0xE4, 0x00,     // B
          0x61, 0x00, 0x62, 0x00, 0x63, 0x00, 0x64, 0x00}},   // R
        {"gbrp16le", 2, 2,
         {0xA1, 0x2A, 0xA2, 0x2A, 0xA3, 0x2A, 0xA4, 0x2A,     // G
          0xA1, 0x3A, 0xA2, 0x3A, 0xA3, 0x3A, 0xA4, 0x3A,     // B
          0xA1, 0x1A, 0xA2, 0x1A, 0xA3, 0x1A, 0xA4, 0x1A}},   // R
    };
}
// clang-format on

media::FormatId idOf(std::string_view name) {
    const auto id = media::findFormat(name);
    REQUIRE_MESSAGE(id.has_value(), name);
    return id.value_or(media::FormatId{0});  // REQUIRE above stops the test case first
}

// The golden's code values: goldenValue on each component's own grid.
media::CodeValues goldenValues(const Golden& golden) {
    media::CodeValues values{
        .format = idOf(golden.format), .width = golden.width, .height = golden.height};
    const media::PixelFormatDescriptor& d = media::descriptor(values.format);
    for (std::size_t c = 0; c < d.componentCount; ++c) {
        const auto [cw, ch] = media::componentSize(values.format, c, golden.width, golden.height);
        std::vector<std::uint16_t>& component = values.components[c];
        for (std::size_t n = 0; n < std::size_t{cw} * ch; ++n) {
            component.push_back(goldenValue(c, n, d.components[c].bits));
        }
    }
    return values;
}

// The golden bytes, zero-extended to the frame size (only v210 lists a prefix).
std::vector<std::byte> goldenFrame(const Golden& golden) {
    const auto frame = media::geometry(idOf(golden.format), golden.width, golden.height);
    REQUIRE(frame.has_value());
    REQUIRE(golden.bytes.size() <= frame->bytes);
    std::vector<std::byte> bytes(frame->bytes);
    std::ranges::transform(golden.bytes, bytes.begin(),
                           [](std::uint8_t b) { return static_cast<std::byte>(b); });
    return bytes;
}

std::string label(const Golden& golden) {
    return std::format("{} {}x{}", golden.format, golden.width, golden.height);
}

}  // namespace

TEST_CASE("every format has a layout golden") {
    const std::vector<Golden> all = goldens();
    CHECK(all.size() == 39);

    std::set<std::string_view> golden_names;
    std::set<std::pair<std::uint32_t, std::uint32_t>> v210_sizes;
    for (const Golden& golden : all) {
        golden_names.insert(golden.format);
        if (golden.format == "v210") {
            v210_sizes.insert({golden.width, golden.height});
        } else {
            CHECK_MESSAGE(golden.width == 2, golden.format);
            CHECK_MESSAGE(golden.height == 2, golden.format);
        }
    }
    std::set<std::string_view> catalogue_names;
    for (const media::FormatId id : media::catalogue()) {
        catalogue_names.insert(media::descriptor(id).name);
    }
    CHECK(catalogue_names.size() == 37);
    CHECK(golden_names == catalogue_names);
    CHECK(v210_sizes == std::set<std::pair<std::uint32_t, std::uint32_t>>{{6, 1}, {2, 1}, {4, 1}});
}

TEST_CASE("pack produces the golden bytes") {
    for (const Golden& golden : goldens()) {
        INFO(label(golden));
        const std::vector<std::byte> expected = goldenFrame(golden);
        if (golden.format != "v210") {
            CHECK(golden.bytes.size() == expected.size());  // the whole frame is listed
        }
        const auto packed = media::pack(goldenValues(golden));
        REQUIRE(packed.has_value());
        REQUIRE(packed->size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            if ((*packed)[i] != expected[i]) {
                FAIL_CHECK(std::format("{}: byte {} expected 0x{:02X}, got 0x{:02X}", label(golden),
                                       i, std::to_integer<unsigned>(expected[i]),
                                       std::to_integer<unsigned>((*packed)[i])));
            }
        }
    }
}

TEST_CASE("unpack reads the golden values") {
    for (const Golden& golden : goldens()) {
        INFO(label(golden));
        const auto values =
            media::unpack(idOf(golden.format), golden.width, golden.height, goldenFrame(golden));
        REQUIRE(values.has_value());
        CHECK(*values == goldenValues(golden));
    }
}
