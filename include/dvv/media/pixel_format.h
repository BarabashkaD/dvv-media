#pragma once

// Pixel-format descriptors (spec §7.3, M2 design D2): a plane is a row of groups; a group spans
// groupPixels full-resolution pixels in groupBytes bytes made of words of wordBytes with one byte
// order; each sample of a group is a slot {component, index in the component's own grid, word,
// shift}, value = (word >> shift) & ((1 << bits) - 1).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "dvv/base/error.h"
#include "dvv/media/types.h"

namespace dvv::media {

enum class ColourModel : std::uint8_t { Gray, Yuv, Rgb };
enum class ComponentKind : std::uint8_t { Y, U, V, R, G, B, A };
enum class ByteOrder : std::uint8_t { Little, Big };

inline constexpr std::size_t MAX_COMPONENTS = 4, MAX_PLANES = 4, MAX_SLOTS = 12;
// GPU unpack limit (design §5.4): samples of one component per group.
inline constexpr std::size_t MAX_COMPONENT_SAMPLES = 6;

struct ComponentDesc {
    ComponentKind kind = ComponentKind::Y;
    std::uint8_t bits = 0;      // significant bits, 1..16
    std::uint8_t log2SubX = 0;  // horizontal subsampling (1 = half width, rounded up)
    std::uint8_t log2SubY = 0;
    bool operator==(const ComponentDesc&) const = default;
};
struct SampleSlot {
    std::uint8_t component = 0;  // index into components
    std::uint8_t index = 0;      // sample index inside the group, in the component's own grid
    std::uint8_t word = 0;       // word index inside the group
    std::uint8_t shift = 0;      // bit position of the sample's LSB inside the word
    bool operator==(const SampleSlot&) const = default;
};
struct PlaneDesc {
    std::uint8_t groupPixels = 1;  // full-resolution pixels per group (1, 2, 6, ...)
    std::uint8_t groupBytes = 1;   // bytes per group
    std::uint8_t wordBytes = 1;    // 1, 2 or 4; groupBytes % wordBytes == 0
    ByteOrder order = ByteOrder::Little;
    std::uint16_t rowAlign = 1;  // row stride alignment in bytes (v210: 128)
    std::uint8_t log2SubY = 0;   // rows = ceil(height / 2^log2SubY); equals its components'
    std::uint8_t slotCount = 0;
    std::array<SampleSlot, MAX_SLOTS> slots{};  // every sample of one group; other bits are padding
    bool operator==(const PlaneDesc&) const = default;
};
struct PixelFormatDescriptor {
    std::string name;  // FFmpeg pix_fmt name ("v210": codec name)
    ColourModel model = ColourModel::Gray;
    std::uint8_t componentCount = 0;
    std::array<ComponentDesc, MAX_COMPONENTS> components{};  // order = unpacked plane order
    std::uint8_t planeCount = 0;
    std::array<PlaneDesc, MAX_PLANES> planes{};
    std::uint8_t widthMultiple = 1;  // the width must be a multiple of this (v210: 2)
    bool operator==(const PixelFormatDescriptor&) const = default;
};

// Checks a descriptor, including the GPU limits. Errc::Format with the reason (rules in the plan).
[[nodiscard]] Result<void> validate(const PixelFormatDescriptor& descriptor);

// Process-wide table: entries are heap-allocated and never moved or removed, so descriptor()
// returns stable references while other threads intern formats. The catalogue (D3) is interned at
// first use, in the order of D3, as ids 0..36.
[[nodiscard]] std::span<const FormatId> catalogue();
// Unknown id: a reference to a static empty descriptor (name "", no components, no planes).
[[nodiscard]] const PixelFormatDescriptor& descriptor(FormatId format) noexcept;
// Exact, case-sensitive name match over every interned format (catalogue, unpacked, custom).
[[nodiscard]] std::optional<FormatId> findFormat(std::string_view name) noexcept;
// FFmpeg's deprecated full-range names (M4 design D4): what a decoder output named yuvj420p,
// yuvj422p or yuvj444p is in the catalogue, with the range its name implies.
struct FormatAlias {
    FormatId format = 0;
    ColourRange range = ColourRange::Unspecified;
    bool operator==(const FormatAlias&) const = default;
};
// yuvj420p -> {yuv420p, Full}, yuvj422p -> {yuv422p, Full}, yuvj444p -> {yuv444p, Full}; nullopt
// for every other name, catalogue names included. Exact, case-sensitive. findFormat() does not know
// these names. Used by the FFmpeg providers.
[[nodiscard]] std::optional<FormatAlias> formatAlias(std::string_view name) noexcept;
// The lossless planar form (D5), named "<name>@planes"; interned on first use; thread-safe;
// idempotent for unpacked formats; an unknown id is returned unchanged.
[[nodiscard]] FormatId unpackedFormat(FormatId format);
[[nodiscard]] bool isUnpacked(FormatId format) noexcept;

struct PlaneGeometry {
    std::uint64_t offset = 0;  // from the frame start
    std::uint32_t stride = 0;  // rowBytes rounded up to the plane's rowAlign
    std::uint32_t rows = 0;
    std::uint32_t rowBytes = 0;  // ceil(width / groupPixels) * groupBytes
    bool operator==(const PlaneGeometry&) const = default;
};
struct FrameGeometry {
    std::array<PlaneGeometry, MAX_PLANES> planes{};
    std::uint8_t planeCount = 0;
    std::uint64_t bytes = 0;  // tight layout, planes back to back
    bool operator==(const FrameGeometry&) const = default;
};
// Errc::Format for a 0 width or height, a width that is not a multiple of widthMultiple ("v210
// needs a width that is a multiple of 2"), or a plane whose stride or stride x rows exceeds
// 0xFFFFFFFF. Errc::Internal for an unknown id.
[[nodiscard]] Result<FrameGeometry> geometry(FormatId format, std::uint32_t width,
                                             std::uint32_t height);
// ceil(width / 2^log2SubX) x ceil(height / 2^log2SubY); {0, 0} for an unknown id or component.
[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> componentSize(FormatId format,
                                                                    std::size_t component,
                                                                    std::uint32_t width,
                                                                    std::uint32_t height);

}  // namespace dvv::media
