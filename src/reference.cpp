#include "dvv/media/reference.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <utility>
#include <vector>

#include "math_util.h"

namespace dvv::media {

namespace {

// The word of `plane.wordBytes` bytes at `at`, in the plane's byte order.
[[nodiscard]] std::uint32_t readWord(std::span<const std::byte> bytes, std::size_t at,
                                     const PlaneDesc& plane) noexcept {
    std::uint32_t word = 0;
    for (unsigned k = 0; k < plane.wordBytes; ++k) {
        const unsigned shift =
            8U * (plane.order == ByteOrder::Little ? k : plane.wordBytes - 1U - k);
        word |= std::to_integer<std::uint32_t>(bytes[at + k]) << shift;
    }
    return word;
}

void writeWord(std::span<std::byte> bytes, std::size_t at, const PlaneDesc& plane,
               std::uint32_t word) noexcept {
    for (unsigned k = 0; k < plane.wordBytes; ++k) {
        const unsigned shift =
            8U * (plane.order == ByteOrder::Little ? k : plane.wordBytes - 1U - k);
        bytes[at + k] = static_cast<std::byte>((word >> shift) & 0xFFU);
    }
}

// The component grid widths (componentSize) of a format; 0 beyond componentCount.
[[nodiscard]] std::array<std::uint32_t, MAX_COMPONENTS> gridWidths(FormatId format,
                                                                   const PixelFormatDescriptor& d,
                                                                   std::uint32_t width,
                                                                   std::uint32_t height) {
    std::array<std::uint32_t, MAX_COMPONENTS> widths{};
    for (std::size_t c = 0; c < d.componentCount; ++c) {
        widths[c] = componentSize(format, c, width, height).first;
    }
    return widths;
}

// Visits every in-width sample of the frame once: visit(component, index of the value in the
// component's row-major grid, byte offset of its word, plane, shift). Samples beyond the width,
// padding bits and row padding are skipped. A plane's rows are its components' rows (validate()
// ties the plane's log2SubY to them).
template <typename Visit>
void forEachSample(const PixelFormatDescriptor& d, const FrameGeometry& frame, std::uint32_t width,
                   const std::array<std::uint32_t, MAX_COMPONENTS>& widths, const Visit& visit) {
    for (std::size_t p = 0; p < d.planeCount; ++p) {
        const PlaneDesc& plane = d.planes[p];
        const PlaneGeometry& geometry = frame.planes[p];
        const std::uint64_t groups = detail::ceilDiv(width, plane.groupPixels);
        for (std::uint64_t row = 0; row < geometry.rows; ++row) {
            for (std::uint64_t g = 0; g < groups; ++g) {
                const std::uint64_t group_offset =
                    geometry.offset + (row * geometry.stride) + (g * plane.groupBytes);
                for (std::size_t s = 0; s < plane.slotCount; ++s) {
                    const SampleSlot& slot = plane.slots[s];
                    const ComponentDesc& component = d.components[slot.component];
                    const std::uint64_t grid_width = widths[slot.component];
                    const std::uint64_t x =
                        (g * (plane.groupPixels >> component.log2SubX)) + slot.index;
                    if (x >= grid_width) {
                        continue;
                    }
                    visit(std::size_t{slot.component},
                          static_cast<std::size_t>((row * grid_width) + x),
                          static_cast<std::size_t>(group_offset +
                                                   (std::uint64_t{slot.word} * plane.wordBytes)),
                          plane, unsigned{slot.shift});
                }
            }
        }
    }
}

}  // namespace

Result<std::vector<std::byte>> pack(const CodeValues& values) {
    auto frame = geometry(values.format, values.width, values.height);
    if (!frame) {
        return std::unexpected(std::move(frame.error()));
    }
    const PixelFormatDescriptor& d = descriptor(values.format);
    const std::array<std::uint32_t, MAX_COMPONENTS> widths =
        gridWidths(values.format, d, values.width, values.height);
    for (std::size_t c = 0; c < MAX_COMPONENTS; ++c) {
        std::size_t expected = 0;
        if (c < d.componentCount) {
            const auto [cw, ch] = componentSize(values.format, c, values.width, values.height);
            expected = std::size_t{cw} * ch;
        }
        const std::size_t count = values.components[c].size();
        if (count != expected) {
            return makeError(Errc::Internal, std::format("component {} has {} values, expected {}",
                                                         c, count, expected));
        }
    }
    for (std::size_t c = 0; c < d.componentCount; ++c) {
        const unsigned bits = d.components[c].bits;
        const std::uint32_t limit = std::uint32_t{1} << bits;
        const std::vector<std::uint16_t>& component = values.components[c];
        const auto bad = std::ranges::find_if(
            component, [limit](std::uint16_t value) { return std::uint32_t{value} >= limit; });
        if (bad != component.end()) {
            const auto i = static_cast<std::size_t>(bad - component.begin());
            return makeError(
                Errc::Internal,
                std::format("component {} value {} at ({}, {}) does not fit in {} bits", c, *bad,
                            i % widths[c], i / widths[c], bits));
        }
    }
    std::vector<std::byte> bytes(frame->bytes);
    forEachSample(d, *frame, values.width, widths,
                  [&bytes, &values](std::size_t c, std::size_t i, std::size_t at,
                                    const PlaneDesc& plane, unsigned shift) {
                      const std::uint32_t word = readWord(bytes, at, plane) |
                                                 (std::uint32_t{values.components[c][i]} << shift);
                      writeWord(bytes, at, plane, word);
                  });
    return bytes;
}

Result<CodeValues> unpack(FormatId format, std::uint32_t width, std::uint32_t height,
                          std::span<const std::byte> original) {
    auto frame = geometry(format, width, height);
    if (!frame) {
        return std::unexpected(std::move(frame.error()));
    }
    if (original.size() < frame->bytes) {
        return makeError(Errc::Internal, std::format("original has {} bytes, expected at least {}",
                                                     original.size(), frame->bytes));
    }
    const PixelFormatDescriptor& d = descriptor(format);
    const std::array<std::uint32_t, MAX_COMPONENTS> widths = gridWidths(format, d, width, height);
    CodeValues result{.format = format, .width = width, .height = height};
    for (std::size_t c = 0; c < d.componentCount; ++c) {
        const auto [cw, ch] = componentSize(format, c, width, height);
        result.components[c].resize(std::size_t{cw} * ch);
    }
    forEachSample(d, *frame, width, widths,
                  [&result, &d, original](std::size_t c, std::size_t i, std::size_t at,
                                          const PlaneDesc& plane, unsigned shift) {
                      const std::uint32_t mask = (std::uint32_t{1} << d.components[c].bits) - 1U;
                      result.components[c][i] = static_cast<std::uint16_t>(
                          (readWord(original, at, plane) >> shift) & mask);
                  });
    return result;
}

Result<void> unpackToPlanes(FormatId format, std::uint32_t width, std::uint32_t height,
                            std::span<const std::byte> original, std::span<std::byte> planes) {
    // The source format first, so that an odd v210 width is reported as Format.
    if (auto frame = geometry(format, width, height); !frame) {
        return std::unexpected(std::move(frame.error()));
    }
    const FormatId unpacked = unpackedFormat(format);
    auto target = geometry(unpacked, width, height);
    if (!target) {
        return std::unexpected(std::move(target.error()));
    }
    if (planes.size() != target->bytes) {
        return makeError(Errc::Internal, std::format("planes need exactly {} bytes, got {}",
                                                     target->bytes, planes.size()));
    }
    auto values = unpack(format, width, height, original);
    if (!values) {
        return std::unexpected(std::move(values.error()));
    }
    values->format = unpacked;
    auto bytes = pack(*values);
    if (!bytes) {
        return std::unexpected(std::move(bytes.error()));
    }
    std::ranges::copy(*bytes, planes.begin());
    return {};
}

}  // namespace dvv::media
