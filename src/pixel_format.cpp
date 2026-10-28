#include "dvv/media/pixel_format.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "format_table.h"
#include "math_util.h"

namespace dvv::media {

namespace {

constexpr std::size_t MAX_NAME_BYTES = 64;
constexpr std::string_view UNPACKED_SUFFIX = "@planes";
constexpr std::string_view DISPLAY_SUFFIX = "@display";
constexpr std::uint16_t MAX_ROW_ALIGN = 4096;
constexpr std::uint64_t MAX_PLANE_BYTES = 0xFFFFFFFFU;

// One interned format. The descriptor and `unpacked` never change after interning; `unpackedId` is
// guarded by the table mutex.
struct Entry {
    PixelFormatDescriptor descriptor;
    bool unpacked = false;
    std::optional<FormatId> unpackedId{};
};

// The process-wide format table (spec §6: process infrastructure, never destroyed). Entries are
// never moved or removed, so references to their descriptors stay valid.
struct FormatTable {
    std::shared_mutex mutex;
    std::vector<std::unique_ptr<Entry>> entries;
    const PixelFormatDescriptor empty{};

    FormatTable() {
        const std::span<const PixelFormatDescriptor> built_in = detail::builtInFormats();
        entries.reserve(built_in.size());
        for (const PixelFormatDescriptor& descriptor : built_in) {
            entries.push_back(std::make_unique<Entry>(Entry{.descriptor = descriptor}));
        }
    }

    // The entry of an id, or nullptr. The entry itself stays valid forever.
    [[nodiscard]] const Entry* find(FormatId format) {
        const std::shared_lock lock(mutex);
        return format < entries.size() ? entries[format].get() : nullptr;
    }
};

FormatTable& formatTable() {
    // Intentionally leaked; out of memory at start-up is fatal anyway.
    static auto* table = new FormatTable();  // NOLINT(bugprone-unhandled-exception-at-new)
    return *table;
}

[[nodiscard]] std::unexpected<Error> invalid(const PixelFormatDescriptor& descriptor,
                                             std::string_view reason) {
    return makeError(Errc::Format, std::format("pixel format '{}': {}", descriptor.name, reason));
}

[[nodiscard]] bool isNameChar(char ch) noexcept {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
           ch == '_' || ch == '.' || ch == '-';
}

// 1..64 bytes of [A-Za-z0-9_.-], optionally followed by exactly "@planes" or "@display".
[[nodiscard]] bool isValidName(std::string_view name) noexcept {
    if (name.ends_with(UNPACKED_SUFFIX)) {
        name.remove_suffix(UNPACKED_SUFFIX.size());
    } else if (name.ends_with(DISPLAY_SUFFIX)) {
        name.remove_suffix(DISPLAY_SUFFIX.size());
    }
    return !name.empty() && name.size() <= MAX_NAME_BYTES && std::ranges::all_of(name, isNameChar);
}

// Gray: Y[, A]; Yuv: Y, U, V[, A]; Rgb: R, G, B[, A] (D5).
[[nodiscard]] bool componentsMatchModel(const PixelFormatDescriptor& descriptor) noexcept {
    std::array<ComponentKind, MAX_COMPONENTS> canonical{};
    std::size_t colour_count = 0;
    switch (descriptor.model) {
        case ColourModel::Gray:
            canonical = {ComponentKind::Y, ComponentKind::A};
            colour_count = 1;
            break;
        case ColourModel::Yuv:
            canonical = {ComponentKind::Y, ComponentKind::U, ComponentKind::V, ComponentKind::A};
            colour_count = 3;
            break;
        case ColourModel::Rgb:
            canonical = {ComponentKind::R, ComponentKind::G, ComponentKind::B, ComponentKind::A};
            colour_count = 3;
            break;
        default: return false;
    }
    const std::size_t count = descriptor.componentCount;
    if (count != colour_count && count != colour_count + 1) {
        return false;
    }
    for (std::size_t c = 0; c < count; ++c) {
        if (descriptor.components[c].kind != canonical[c]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isValidSubsampling(const PixelFormatDescriptor& descriptor,
                                      std::size_t c) noexcept {
    const ComponentDesc& component = descriptor.components[c];
    switch (component.kind) {
        case ComponentKind::U:
        case ComponentKind::V: {
            if (component.log2SubX > 2 || component.log2SubY > 2) {
                return false;
            }
            if (component.kind == ComponentKind::V) {
                // The canonical order puts U right before V.
                const ComponentDesc& u = descriptor.components[c - 1];
                return u.log2SubX == component.log2SubX && u.log2SubY == component.log2SubY;
            }
            return true;
        }
        default: return component.log2SubX == 0 && component.log2SubY == 0;
    }
}

[[nodiscard]] bool planeHolds(const PlaneDesc& plane, std::size_t c) noexcept {
    return std::any_of(plane.slots.begin(), plane.slots.begin() + plane.slotCount,
                       [c](const SampleSlot& slot) { return slot.component == c; });
}

// Rule 10 for component c in plane p: its samples are exactly indices 0..n-1 once each, with
// n = groupPixels >> log2SubX.
[[nodiscard]] bool samplesMatchGroup(const PlaneDesc& plane, const ComponentDesc& component,
                                     std::size_t c) noexcept {
    const unsigned sub_x = 1U << component.log2SubX;
    if (plane.groupPixels % sub_x != 0) {
        return false;
    }
    const unsigned n = plane.groupPixels >> component.log2SubX;
    unsigned count = 0;
    std::uint32_t seen = 0;  // n <= MAX_SLOTS when the counts match
    for (std::size_t s = 0; s < plane.slotCount; ++s) {
        const SampleSlot& slot = plane.slots[s];
        if (slot.component != c) {
            continue;
        }
        ++count;
        if (slot.index >= n || slot.index >= MAX_SLOTS) {
            return false;
        }
        const std::uint32_t bit = 1U << slot.index;
        if ((seen & bit) != 0) {
            return false;
        }
        seen |= bit;
    }
    return count == n;
}

[[nodiscard]] Result<void> validatePlanes(const PixelFormatDescriptor& d) {
    // Rule 6: the plane layout.
    for (std::size_t p = 0; p < d.planeCount; ++p) {
        const PlaneDesc& plane = d.planes[p];
        if (plane.wordBytes != 1 && plane.wordBytes != 2 && plane.wordBytes != 4) {
            return invalid(d, std::format("plane {}: wordBytes must be 1, 2 or 4", p));
        }
        if (plane.groupBytes == 0 || plane.groupBytes % plane.wordBytes != 0) {
            return invalid(
                d, std::format("plane {}: groupBytes must be a non-zero multiple of wordBytes", p));
        }
        if (plane.groupPixels == 0 || plane.groupPixels % d.widthMultiple != 0) {
            return invalid(
                d, std::format("plane {}: groupPixels must be a non-zero multiple of widthMultiple",
                               p));
        }
        if (!std::has_single_bit(plane.rowAlign) || plane.rowAlign > MAX_ROW_ALIGN) {
            return invalid(d,
                           std::format("plane {}: rowAlign must be a power of two up to 4096", p));
        }
        if (plane.slotCount < 1 || plane.slotCount > MAX_SLOTS) {
            return invalid(d, std::format("plane {}: needs 1 to 12 slots", p));
        }
    }
    // Rule 7: every slot inside its word.
    for (std::size_t p = 0; p < d.planeCount; ++p) {
        const PlaneDesc& plane = d.planes[p];
        const unsigned words = plane.groupBytes / plane.wordBytes;
        for (std::size_t s = 0; s < plane.slotCount; ++s) {
            const SampleSlot& slot = plane.slots[s];
            if (slot.component >= d.componentCount) {
                return invalid(d, std::format("plane {} slot {}: component out of range", p, s));
            }
            const unsigned bits = d.components[slot.component].bits;
            if (slot.word >= words || slot.shift + bits > 8U * plane.wordBytes) {
                return invalid(d, std::format("plane {} slot {}: outside its word", p, s));
            }
        }
    }
    // Rule 8: no shared bits.
    for (std::size_t p = 0; p < d.planeCount; ++p) {
        const PlaneDesc& plane = d.planes[p];
        for (std::size_t s = 0; s < plane.slotCount; ++s) {
            const SampleSlot& a = plane.slots[s];
            const unsigned a_end = a.shift + d.components[a.component].bits;
            for (std::size_t t = s + 1; t < plane.slotCount; ++t) {
                const SampleSlot& b = plane.slots[t];
                const unsigned b_end = b.shift + d.components[b.component].bits;
                if (a.word == b.word && a.shift < b_end && b.shift < a_end) {
                    return invalid(d, std::format("plane {}: slots {} and {} overlap", p, s, t));
                }
            }
        }
    }
    // Rule 9: each component in exactly one plane.
    for (std::size_t c = 0; c < d.componentCount; ++c) {
        std::size_t planes = 0;
        for (std::size_t p = 0; p < d.planeCount; ++p) {
            planes += planeHolds(d.planes[p], c) ? 1 : 0;
        }
        if (planes > 1) {
            return invalid(d, std::format("component {} is in more than one plane", c));
        }
        if (planes == 0) {
            return invalid(d, std::format("component {} is in no plane", c));
        }
    }
    // Rule 10: the components of a plane fill its groups.
    for (std::size_t p = 0; p < d.planeCount; ++p) {
        const PlaneDesc& plane = d.planes[p];
        for (std::size_t c = 0; c < d.componentCount; ++c) {
            if (!planeHolds(plane, c)) {
                continue;
            }
            const ComponentDesc& component = d.components[c];
            if (plane.log2SubY != component.log2SubY) {
                return invalid(d,
                               std::format("plane {}: log2SubY differs from component {}", p, c));
            }
            if (!samplesMatchGroup(plane, component, c)) {
                return invalid(
                    d,
                    std::format("plane {}: samples of component {} do not match the group", p, c));
            }
            if ((plane.groupPixels >> component.log2SubX) > MAX_COMPONENT_SAMPLES) {
                const std::string reason =
                    std::format("plane {}: more than 6 samples of component {} per group", p, c);
                return invalid(d, reason + " (GPU limit)");
            }
        }
    }
    return {};
}

// The lossless planar form of a format (D5): one plane per component, one sample per group.
[[nodiscard]] PixelFormatDescriptor makeUnpacked(const PixelFormatDescriptor& source) {
    PixelFormatDescriptor result;
    result.name = source.name + std::string(UNPACKED_SUFFIX);
    result.model = source.model;
    result.componentCount = source.componentCount;
    result.components = source.components;
    result.widthMultiple = 1;
    result.planeCount = source.componentCount;
    for (std::uint8_t i = 0; i < source.componentCount; ++i) {
        const ComponentDesc& component = source.components[i];
        const auto word_bytes = static_cast<std::uint8_t>(component.bits <= 8 ? 1 : 2);
        PlaneDesc& plane = result.planes[i];
        plane.groupPixels = static_cast<std::uint8_t>(1U << component.log2SubX);
        plane.groupBytes = word_bytes;
        plane.wordBytes = word_bytes;
        plane.order = ByteOrder::Little;
        plane.rowAlign = 1;
        plane.log2SubY = component.log2SubY;
        plane.slotCount = 1;
        plane.slots[0] = SampleSlot{.component = i, .index = 0, .word = 0, .shift = 0};
    }
    return result;
}

}  // namespace

Result<void> validate(const PixelFormatDescriptor& descriptor) {
    const PixelFormatDescriptor& d = descriptor;
    // Rule 1.
    if (!isValidName(d.name)) {
        return invalid(d, "invalid name");
    }
    // Rule 2.
    if (d.componentCount < 1 || d.componentCount > MAX_COMPONENTS) {
        return invalid(d, "needs 1 to 4 components");
    }
    if (d.planeCount < 1 || d.planeCount > MAX_PLANES) {
        return invalid(d, "needs 1 to 4 planes");
    }
    // Rule 3.
    if (!componentsMatchModel(d)) {
        return invalid(d, "components do not match the colour model");
    }
    // Rule 4.
    for (std::size_t c = 0; c < d.componentCount; ++c) {
        const ComponentDesc& component = d.components[c];
        if (component.bits < 1 || component.bits > 16) {
            return invalid(d, std::format("component {}: bits must be 1 to 16", c));
        }
        if (!isValidSubsampling(d, c)) {
            return invalid(d, std::format("component {}: invalid subsampling", c));
        }
    }
    // Rule 5.
    if (d.widthMultiple < 1) {
        return invalid(d, "widthMultiple must be at least 1");
    }
    // Rules 6 to 10.
    return validatePlanes(d);
}

std::span<const FormatId> catalogue() {
    static constexpr std::array<FormatId, detail::CATALOGUE_SIZE> IDS = [] {
        std::array<FormatId, detail::CATALOGUE_SIZE> ids{};
        for (std::size_t i = 0; i < ids.size(); ++i) {
            ids[i] = static_cast<FormatId>(i);
        }
        return ids;
    }();
    return IDS;
}

const PixelFormatDescriptor& descriptor(FormatId format) noexcept {
    FormatTable& table = formatTable();
    const Entry* entry = table.find(format);
    return entry != nullptr ? entry->descriptor : table.empty;
}

std::optional<FormatId> findFormat(std::string_view name) noexcept {
    FormatTable& table = formatTable();
    const std::shared_lock lock(table.mutex);
    for (std::size_t id = 0; id < table.entries.size(); ++id) {
        if (table.entries[id]->descriptor.name == name) {
            return static_cast<FormatId>(id);
        }
    }
    return std::nullopt;
}

std::optional<FormatAlias> formatAlias(std::string_view name) noexcept {
    struct Alias {
        std::string_view alias;
        std::string_view format;
    };
    static constexpr std::array<Alias, 3> ALIASES{{
        {"yuvj420p", "yuv420p"},
        {"yuvj422p", "yuv422p"},
        {"yuvj444p", "yuv444p"},
    }};
    const auto found = std::ranges::find(ALIASES, name, &Alias::alias);
    if (found == ALIASES.end()) {
        return std::nullopt;
    }
    const std::optional<FormatId> format = findFormat(found->format);  // a catalogue name
    if (!format) {
        return std::nullopt;
    }
    return FormatAlias{.format = *format, .range = ColourRange::Full};
}

FormatId unpackedFormat(FormatId format) {
    FormatTable& table = formatTable();
    const Entry* source = nullptr;
    {
        const std::shared_lock lock(table.mutex);
        if (format >= table.entries.size()) {
            return format;
        }
        source = table.entries[format].get();
        // The display format (named "<name>@display") is its own unpacked form.
        if (source->unpacked || source->descriptor.name.ends_with(DISPLAY_SUFFIX)) {
            return format;
        }
        if (source->unpackedId) {
            return *source->unpackedId;
        }
    }
    // The source descriptor never changes, so it is read without the lock.
    auto entry = std::make_unique<Entry>(
        Entry{.descriptor = makeUnpacked(source->descriptor), .unpacked = true});
    const std::unique_lock lock(table.mutex);
    Entry& owner = *table.entries[format];
    if (owner.unpackedId) {
        return *owner.unpackedId;  // another thread was first
    }
    const auto id = static_cast<FormatId>(table.entries.size());
    table.entries.push_back(std::move(entry));
    owner.unpackedId = id;
    return id;
}

bool isUnpacked(FormatId format) noexcept {
    const Entry* entry = formatTable().find(format);
    return entry != nullptr && entry->unpacked;
}

Result<FormatId> detail::internCustomFormat(const PixelFormatDescriptor& descriptor) {
    FormatTable& table = formatTable();
    auto entry = std::make_unique<Entry>(Entry{.descriptor = descriptor});
    const std::unique_lock lock(table.mutex);
    for (std::size_t id = 0; id < table.entries.size(); ++id) {
        const PixelFormatDescriptor& existing = table.entries[id]->descriptor;
        if (existing.name != descriptor.name) {
            continue;
        }
        if (id < detail::CATALOGUE_SIZE) {
            return makeError(Errc::Format,
                             std::format("'{}' is a built-in format", descriptor.name));
        }
        if (existing == descriptor) {
            return static_cast<FormatId>(id);
        }
        return makeError(
            Errc::Format,
            std::format("a different format named '{}' is already loaded", descriptor.name));
    }
    const auto id = static_cast<FormatId>(table.entries.size());
    table.entries.push_back(std::move(entry));
    return id;
}

Result<FrameGeometry> geometry(FormatId format, std::uint32_t width, std::uint32_t height) {
    const Entry* entry = formatTable().find(format);
    if (entry == nullptr) {
        return makeError(Errc::Internal, std::format("unknown format id {}", format));
    }
    const PixelFormatDescriptor& d = entry->descriptor;
    if (width == 0 || height == 0) {
        return makeError(Errc::Format,
                         std::format("{}: frame size {}x{} is empty", d.name, width, height));
    }
    if (width % d.widthMultiple != 0) {
        return makeError(Errc::Format, std::format("{} needs a width that is a multiple of {}",
                                                   d.name, unsigned{d.widthMultiple}));
    }
    FrameGeometry result;
    result.planeCount = d.planeCount;
    std::uint64_t offset = 0;
    for (std::size_t p = 0; p < d.planeCount; ++p) {
        const PlaneDesc& plane = d.planes[p];
        const std::uint64_t row_bytes =
            detail::ceilDiv(width, plane.groupPixels) * plane.groupBytes;
        const std::uint64_t stride = detail::alignUp(row_bytes, plane.rowAlign);
        const std::uint64_t rows = detail::ceilDiv(height, std::uint64_t{1} << plane.log2SubY);
        if (stride > MAX_PLANE_BYTES || stride * rows > MAX_PLANE_BYTES) {
            return makeError(Errc::Format, std::format("{} at {}x{}: plane {} exceeds 4 GiB",
                                                       d.name, width, height, p));
        }
        result.planes[p] = PlaneGeometry{.offset = offset,
                                         .stride = static_cast<std::uint32_t>(stride),
                                         .rows = static_cast<std::uint32_t>(rows),
                                         .rowBytes = static_cast<std::uint32_t>(row_bytes)};
        offset += stride * rows;
    }
    result.bytes = offset;
    return result;
}

std::pair<std::uint32_t, std::uint32_t> componentSize(FormatId format, std::size_t component,
                                                      std::uint32_t width, std::uint32_t height) {
    const Entry* entry = formatTable().find(format);
    if (entry == nullptr || component >= entry->descriptor.componentCount) {
        return {0, 0};
    }
    const ComponentDesc& c = entry->descriptor.components[component];
    return {static_cast<std::uint32_t>(detail::ceilDiv(width, std::uint64_t{1} << c.log2SubX)),
            static_cast<std::uint32_t>(detail::ceilDiv(height, std::uint64_t{1} << c.log2SubY))};
}

}  // namespace dvv::media
