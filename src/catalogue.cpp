// The built-in pixel formats (M2 design D3), in the fixed order that gives the catalogue ids 0..36.
// Each slot is written as slot(component, index, word, shift); planes are in memory order.

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <utility>

#include "dvv/media/pixel_format.h"
#include "format_table.h"

namespace dvv::media::detail {

namespace {

// Component indices in the canonical order of each model (D5).
constexpr std::uint8_t Y = 0, U = 1, V = 2, R = 0, G = 1, B = 2, A = 3;

constexpr ByteOrder LE = ByteOrder::Little;
constexpr ByteOrder BE = ByteOrder::Big;

ComponentDesc component(ComponentKind kind, std::uint8_t bits, std::uint8_t log2_sub_x = 0,
                        std::uint8_t log2_sub_y = 0) {
    return {.kind = kind, .bits = bits, .log2SubX = log2_sub_x, .log2SubY = log2_sub_y};
}

SampleSlot slot(std::uint8_t component, std::uint8_t index, std::uint8_t word,
                std::uint8_t shift = 0) {
    return {.component = component, .index = index, .word = word, .shift = shift};
}

struct PlaneLayout {
    std::uint8_t groupPixels = 1;
    std::uint8_t groupBytes = 1;
    std::uint8_t wordBytes = 1;
    ByteOrder order = LE;
    std::uint8_t log2SubY = 0;
    std::uint16_t rowAlign = 1;
};

PlaneDesc plane(const PlaneLayout& layout, std::initializer_list<SampleSlot> slots) {
    PlaneDesc result;
    result.groupPixels = layout.groupPixels;
    result.groupBytes = layout.groupBytes;
    result.wordBytes = layout.wordBytes;
    result.order = layout.order;
    result.rowAlign = layout.rowAlign;
    result.log2SubY = layout.log2SubY;
    for (const SampleSlot& sample : slots) {
        result.slots[result.slotCount++] = sample;
    }
    return result;
}

PixelFormatDescriptor format(std::string name, ColourModel model,
                             std::initializer_list<ComponentDesc> components,
                             std::initializer_list<PlaneDesc> planes,
                             std::uint8_t width_multiple = 1) {
    PixelFormatDescriptor result;
    result.name = std::move(name);
    result.model = model;
    for (const ComponentDesc& c : components) {
        result.components[result.componentCount++] = c;
    }
    for (const PlaneDesc& p : planes) {
        result.planes[result.planeCount++] = p;
    }
    result.widthMultiple = width_multiple;
    return result;
}

std::uint8_t wordBytesFor(std::uint8_t bits) {
    return static_cast<std::uint8_t>(bits <= 8 ? 1 : 2);
}

// One plane per component, one sample per group: groupPixels = 2^log2SubX of the component.
PlaneDesc planarPlane(std::uint8_t component, std::uint8_t word_bytes, std::uint8_t log2_sub_x,
                      std::uint8_t log2_sub_y, ByteOrder order) {
    return plane({.groupPixels = static_cast<std::uint8_t>(1U << log2_sub_x),
                  .groupBytes = word_bytes,
                  .wordBytes = word_bytes,
                  .order = order,
                  .log2SubY = log2_sub_y},
                 {slot(component, 0, 0)});
}

PixelFormatDescriptor gray(std::string name, std::uint8_t bits, ByteOrder order = LE) {
    return format(std::move(name), ColourModel::Gray, {component(ComponentKind::Y, bits)},
                  {planarPlane(Y, wordBytesFor(bits), 0, 0, order)});
}

// Y, U, V planes (yuv4xxp*).
PixelFormatDescriptor yuvPlanar(std::string name, std::uint8_t bits, std::uint8_t log2_sub_x,
                                std::uint8_t log2_sub_y, ByteOrder order = LE) {
    const std::uint8_t word_bytes = wordBytesFor(bits);
    return format(std::move(name), ColourModel::Yuv,
                  {component(ComponentKind::Y, bits),
                   component(ComponentKind::U, bits, log2_sub_x, log2_sub_y),
                   component(ComponentKind::V, bits, log2_sub_x, log2_sub_y)},
                  {planarPlane(Y, word_bytes, 0, 0, order),
                   planarPlane(U, word_bytes, log2_sub_x, log2_sub_y, order),
                   planarPlane(V, word_bytes, log2_sub_x, log2_sub_y, order)});
}

// G, B, R planes (gbrp*).
PixelFormatDescriptor gbrPlanar(std::string name, std::uint8_t bits) {
    const std::uint8_t word_bytes = wordBytesFor(bits);
    return format(std::move(name), ColourModel::Rgb,
                  {component(ComponentKind::R, bits), component(ComponentKind::G, bits),
                   component(ComponentKind::B, bits)},
                  {planarPlane(G, word_bytes, 0, 0, LE), planarPlane(B, word_bytes, 0, 0, LE),
                   planarPlane(R, word_bytes, 0, 0, LE)});
}

// Y, U, V components with half-width chroma (4:2:0 or 4:2:2), for the semi-planar and packed
// formats.
std::array<ComponentDesc, 3> yuv(std::uint8_t bits, std::uint8_t log2_sub_y) {
    return {component(ComponentKind::Y, bits), component(ComponentKind::U, bits, 1, log2_sub_y),
            component(ComponentKind::V, bits, 1, log2_sub_y)};
}

PixelFormatDescriptor yuvFormat(std::string name, std::uint8_t bits, std::uint8_t log2_sub_y,
                                std::initializer_list<PlaneDesc> planes,
                                std::uint8_t width_multiple = 1) {
    const std::array<ComponentDesc, 3> c = yuv(bits, log2_sub_y);
    return format(std::move(name), ColourModel::Yuv, {c[0], c[1], c[2]}, planes, width_multiple);
}

PixelFormatDescriptor rgb(std::string name, std::uint8_t bits, const PlaneLayout& layout,
                          std::initializer_list<SampleSlot> slots) {
    return format(std::move(name), ColourModel::Rgb,
                  {component(ComponentKind::R, bits), component(ComponentKind::G, bits),
                   component(ComponentKind::B, bits)},
                  {plane(layout, slots)});
}

PixelFormatDescriptor rgba(std::string name, std::uint8_t bits, const PlaneLayout& layout,
                           std::initializer_list<SampleSlot> slots) {
    return format(std::move(name), ColourModel::Rgb,
                  {component(ComponentKind::R, bits), component(ComponentKind::G, bits),
                   component(ComponentKind::B, bits), component(ComponentKind::A, bits)},
                  {plane(layout, slots)});
}

std::array<PixelFormatDescriptor, CATALOGUE_SIZE> makeBuiltInFormats() {
    return {
        gray("gray", 8),                         // 0
        gray("gray10le", 10),                    // 1
        gray("gray12le", 12),                    // 2
        gray("gray16le", 16),                    // 3
        gray("gray16be", 16, BE),                // 4
        yuvPlanar("yuv420p", 8, 1, 1),           // 5
        yuvPlanar("yuv422p", 8, 1, 0),           // 6
        yuvPlanar("yuv444p", 8, 0, 0),           // 7
        yuvPlanar("yuv420p10le", 10, 1, 1),      // 8
        yuvPlanar("yuv422p10le", 10, 1, 0),      // 9
        yuvPlanar("yuv444p10le", 10, 0, 0),      // 10
        yuvPlanar("yuv420p12le", 12, 1, 1),      // 11
        yuvPlanar("yuv422p12le", 12, 1, 0),      // 12
        yuvPlanar("yuv444p12le", 12, 0, 0),      // 13
        yuvPlanar("yuv420p16le", 16, 1, 1),      // 14
        yuvPlanar("yuv444p16le", 16, 0, 0),      // 15
        yuvPlanar("yuv420p10be", 10, 1, 1, BE),  // 16
        // 17 nv12, 18 nv21: interleaved chroma plane.
        yuvFormat("nv12", 8, 1,
                  {plane({}, {slot(Y, 0, 0)}),
                   plane({.groupPixels = 2, .groupBytes = 2, .wordBytes = 1, .log2SubY = 1},
                         {slot(U, 0, 0), slot(V, 0, 1)})}),
        yuvFormat("nv21", 8, 1,
                  {plane({}, {slot(Y, 0, 0)}),
                   plane({.groupPixels = 2, .groupBytes = 2, .wordBytes = 1, .log2SubY = 1},
                         {slot(V, 0, 0), slot(U, 0, 1)})}),
        // 19 p010le: samples in the high bits of 16-bit words; 20 p016le.
        yuvFormat("p010le", 10, 1,
                  {plane({.groupPixels = 1, .groupBytes = 2, .wordBytes = 2}, {slot(Y, 0, 0, 6)}),
                   plane({.groupPixels = 2, .groupBytes = 4, .wordBytes = 2, .log2SubY = 1},
                         {slot(U, 0, 0, 6), slot(V, 0, 1, 6)})}),
        yuvFormat("p016le", 16, 1,
                  {plane({.groupPixels = 1, .groupBytes = 2, .wordBytes = 2}, {slot(Y, 0, 0)}),
                   plane({.groupPixels = 2, .groupBytes = 4, .wordBytes = 2, .log2SubY = 1},
                         {slot(U, 0, 0), slot(V, 0, 1)})}),
        // 21 yuyv422, 22 uyvy422, 23 y210le: packed 4:2:2.
        yuvFormat("yuyv422", 8, 0,
                  {plane({.groupPixels = 2, .groupBytes = 4, .wordBytes = 1},
                         {slot(Y, 0, 0), slot(U, 0, 1), slot(Y, 1, 2), slot(V, 0, 3)})}),
        yuvFormat("uyvy422", 8, 0,
                  {plane({.groupPixels = 2, .groupBytes = 4, .wordBytes = 1},
                         {slot(U, 0, 0), slot(Y, 0, 1), slot(V, 0, 2), slot(Y, 1, 3)})}),
        yuvFormat(
            "y210le", 10, 0,
            {plane({.groupPixels = 2, .groupBytes = 8, .wordBytes = 2},
                   {slot(Y, 0, 0, 6), slot(U, 0, 1, 6), slot(Y, 1, 2, 6), slot(V, 0, 3, 6)})}),
        // 24 v210: 6 pixels in four little-endian 32-bit words of three 10-bit samples each,
        // rows aligned to 128 bytes.
        yuvFormat("v210", 10, 0,
                  {plane({.groupPixels = 6, .groupBytes = 16, .wordBytes = 4, .rowAlign = 128},
                         {slot(U, 0, 0), slot(Y, 0, 0, 10), slot(V, 0, 0, 20),  //
                          slot(Y, 1, 1), slot(U, 1, 1, 10), slot(Y, 2, 1, 20),  //
                          slot(V, 1, 2), slot(Y, 3, 2, 10), slot(U, 2, 2, 20),  //
                          slot(Y, 4, 3), slot(V, 2, 3, 10), slot(Y, 5, 3, 20)})},
                  2),
        // 25..33: packed RGB.
        rgb("rgb24", 8, {.groupBytes = 3}, {slot(R, 0, 0), slot(G, 0, 1), slot(B, 0, 2)}),
        rgb("bgr24", 8, {.groupBytes = 3}, {slot(B, 0, 0), slot(G, 0, 1), slot(R, 0, 2)}),
        rgba("rgba", 8, {.groupBytes = 4},
             {slot(R, 0, 0), slot(G, 0, 1), slot(B, 0, 2), slot(A, 0, 3)}),
        rgba("bgra", 8, {.groupBytes = 4},
             {slot(B, 0, 0), slot(G, 0, 1), slot(R, 0, 2), slot(A, 0, 3)}),
        rgba("argb", 8, {.groupBytes = 4},
             {slot(A, 0, 0), slot(R, 0, 1), slot(G, 0, 2), slot(B, 0, 3)}),
        rgb("rgb48le", 16, {.groupBytes = 6, .wordBytes = 2},
            {slot(R, 0, 0), slot(G, 0, 1), slot(B, 0, 2)}),
        rgb("rgb48be", 16, {.groupBytes = 6, .wordBytes = 2, .order = BE},
            {slot(R, 0, 0), slot(G, 0, 1), slot(B, 0, 2)}),
        rgba("rgba64le", 16, {.groupBytes = 8, .wordBytes = 2},
             {slot(R, 0, 0), slot(G, 0, 1), slot(B, 0, 2), slot(A, 0, 3)}),
        rgb("x2rgb10le", 10, {.groupBytes = 4, .wordBytes = 4},
            {slot(R, 0, 0, 20), slot(G, 0, 0, 10), slot(B, 0, 0)}),
        // 34..36: planar G, B, R.
        gbrPlanar("gbrp", 8),
        gbrPlanar("gbrp10le", 10),
        gbrPlanar("gbrp16le", 16),
    };
}

}  // namespace

std::span<const PixelFormatDescriptor> builtInFormats() {
    static const std::array<PixelFormatDescriptor, CATALOGUE_SIZE> FORMATS = makeBuiltInFormats();
    return FORMATS;
}

}  // namespace dvv::media::detail
