#include "dvv/media/sample_grid.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>

#include "dvv/base/error.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"
#include "dvv/platform/pixel_size.h"

namespace dvv::media {

namespace {

// The plane that holds a component's samples, and those samples sorted by index.
struct ComponentSlots {
    std::size_t plane = 0;
    std::array<SampleSlot, MAX_SLOTS> slots{};
    std::size_t count = 0;
};

Result<ComponentSlots> componentSlots(const PixelFormatDesc& d, std::size_t component) {
    if (component >= d.componentCount) {
        return makeError(Errc::Internal, std::format("stored words: component {} of {} has no "
                                                     "samples",
                                                     component, d.name));
    }
    ComponentSlots result;
    bool found = false;
    for (std::size_t p = 0; p < d.planeCount; ++p) {
        const PlaneDesc& candidate = d.planes[p];
        for (std::size_t s = 0; s < candidate.slotCount; ++s) {
            if (candidate.slots[s].component != component) {
                continue;
            }
            if (found && result.plane != p) {
                return makeError(Errc::Internal,
                                 std::format("stored words: component {} of {} spans planes {} "
                                             "and {}",
                                             component, d.name, result.plane, p));
            }
            found = true;
            result.plane = p;
            result.slots[result.count++] = candidate.slots[s];
        }
    }
    if (!found) {
        return makeError(Errc::Internal, std::format("stored words: component {} of {} has no "
                                                     "samples",
                                                     component, d.name));
    }
    std::ranges::sort(std::span(result.slots).first(result.count), {}, &SampleSlot::index);
    const std::size_t n = result.count;
    bool indexed = true;
    for (std::size_t k = 0; k < n; ++k) {
        indexed = indexed && result.slots[k].index == k;
    }
    const PlaneDesc& plane = d.planes[result.plane];
    if (n > MAX_COMPONENT_SAMPLES || !indexed ||
        n != static_cast<std::size_t>(plane.groupPixels >> d.components[component].log2SubX)) {
        return makeError(Errc::Internal,
                         std::format("stored words: component {} of {} has {} samples per group, "
                                     "which the stored-word rule cannot describe (at most {}, "
                                     "indices 0..n-1, groupPixels >> log2SubX)",
                                     component, d.name, n, MAX_COMPONENT_SAMPLES));
    }
    return result;
}

// The bits of a word that no slot of the plane's group covers.
std::uint32_t paddingMaskOf(const PixelFormatDesc& d, const PlaneDesc& plane, std::uint8_t word) {
    std::uint32_t covered = 0;
    for (std::size_t s = 0; s < plane.slotCount; ++s) {
        const SampleSlot& slot = plane.slots[s];
        if (slot.word == word) {
            covered |= ((1U << d.components[slot.component].bits) - 1U) << slot.shift;
        }
    }
    const std::uint32_t word_bits =
        plane.wordBytes >= 4 ? 0xFFFFFFFFU : (1U << (plane.wordBytes * 8U)) - 1U;
    return word_bits & ~covered;
}

// shaders/sample_grid.slang storedWordAddress(), statement by statement.
std::uint64_t storedWordAddress(const StoredWordLayout& layout, SamplePosition sample,
                                std::uint32_t& shift, std::uint32_t& padding_mask) noexcept {
    const std::uint32_t group = sample.x / layout.samplesPerGroup;
    const std::uint32_t k = sample.x - (group * layout.samplesPerGroup);
    const std::uint32_t slot = (layout.slots[k >> 1U] >> ((k & 1U) * 16U)) & 0xFFFFU;
    const std::uint32_t word = slot >> 8U;
    shift = slot & 0xFFU;
    padding_mask = layout.paddingMasks[k];
    return layout.planeOffset + (std::uint64_t{sample.y} * std::uint64_t{layout.stride}) +
           (std::uint64_t{group} * std::uint64_t{layout.groupBytes}) +
           (std::uint64_t{word} * std::uint64_t{layout.wordBytes});
}

}  // namespace

PixelBlock blockOf(const ComponentDesc& component, SamplePosition sample,
                   dvv::platform::PixelSize image) noexcept {
    const std::uint32_t x0 = sample.x << component.log2SubX;
    const std::uint32_t y0 = sample.y << component.log2SubY;
    if (x0 >= image.width || y0 >= image.height) {
        return {};
    }
    const std::uint32_t x1 = std::min((sample.x + 1U) << component.log2SubX, image.width);
    const std::uint32_t y1 = std::min((sample.y + 1U) << component.log2SubY, image.height);
    return {.x = x0, .y = y0, .width = x1 - x0, .height = y1 - y0};
}

Result<StoredWordLayout> storedWordLayout(FormatId format, std::uint32_t width,
                                          std::uint32_t height, std::size_t component) {
    const PixelFormatDesc& d = formatDesc(format);
    DVV_TRY(const ComponentSlots slots, componentSlots(d, component));
    DVV_TRY(const FrameGeometry frame, geometry(format, width, height));
    const PlaneDesc& plane = d.planes[slots.plane];
    StoredWordLayout layout;
    layout.planeOffset = frame.planes[slots.plane].offset;
    layout.stride = frame.planes[slots.plane].stride;
    layout.groupBytes = plane.groupBytes;
    layout.wordBytes = plane.wordBytes;
    layout.bigEndian = plane.order == ByteOrder::Big ? 1U : 0U;
    layout.bits = d.components[component].bits;
    layout.samplesPerGroup = static_cast<std::uint32_t>(slots.count);
    for (std::size_t k = 0; k < slots.count; ++k) {
        const SampleSlot& slot = slots.slots[k];
        layout.slots[k / 2] |= ((std::uint32_t{slot.word} << 8U) | std::uint32_t{slot.shift})
                               << (16U * (k % 2));
        layout.paddingMasks[k] = paddingMaskOf(d, plane, slot.word);
    }
    return layout;
}

Result<StoredWord> storedWord(FormatId format, std::uint32_t width, std::uint32_t height,
                              std::size_t component, SamplePosition sample) {
    const PixelFormatDesc& d = formatDesc(format);
    DVV_TRY(const ComponentSlots slots, componentSlots(d, component));
    DVV_TRY(const StoredWordLayout layout, storedWordLayout(format, width, height, component));
    const auto [grid_width, grid_height] = componentSize(format, component, width, height);
    if (sample.x >= grid_width || sample.y >= grid_height) {
        return makeError(
            Errc::Internal,
            std::format("stored words: sample ({}, {}) lies outside the {}x{} grid "
                        "of component {} of {}",
                        sample.x, sample.y, grid_width, grid_height, component, d.name));
    }
    std::uint32_t shift = 0;
    std::uint32_t padding_mask = 0;
    const std::uint64_t address = storedWordAddress(layout, sample, shift, padding_mask);
    const PlaneDesc& plane = d.planes[slots.plane];
    const std::uint8_t word = slots.slots[sample.x % slots.count].word;

    StoredWord result;
    result.plane = static_cast<std::uint8_t>(slots.plane);
    result.byteOffset = address;
    result.wordBytes = plane.wordBytes;
    result.order = plane.order;
    result.shift = static_cast<std::uint8_t>(shift);
    result.bits = d.components[component].bits;
    result.paddingMask = padding_mask;
    for (std::size_t s = 0; s < plane.slotCount; ++s) {
        if (plane.slots[s].word == word) {
            result.sharedShifts[result.sharedCount++] = plane.slots[s].shift;
        }
    }
    std::ranges::sort(std::span(result.sharedShifts).first(result.sharedCount));
    return result;
}

std::uint32_t readStoredWord(std::span<const std::byte> bytes, std::uint64_t first_byte,
                             const StoredWord& word) noexcept {
    if (word.byteOffset < first_byte || word.wordBytes == 0 || word.wordBytes > 4) {
        return 0;
    }
    const std::uint64_t at = word.byteOffset - first_byte;
    if (at > bytes.size() || bytes.size() - at < word.wordBytes) {
        return 0;
    }
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < word.wordBytes; ++i) {
        const auto byte = std::to_integer<std::uint32_t>(bytes[at + i]);
        value = word.order == ByteOrder::Little ? value | (byte << (8U * i)) : (value << 8U) | byte;
    }
    return value;
}

}  // namespace dvv::media
