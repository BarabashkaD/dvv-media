#include "dvv/media/format_json.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "dvv/media/pixel_format.h"
#include "format_table.h"

namespace dvv::media {

namespace {

// JSON spellings, indexed by the enum values.
constexpr std::array<std::string_view, 3> MODELS = {"gray", "yuv", "rgb"};
constexpr std::array<std::string_view, 7> KINDS = {"Y", "U", "V", "R", "G", "B", "A"};
constexpr std::array<std::string_view, 2> ORDERS = {"little", "big"};

constexpr std::array<std::string_view, 5> FORMAT_KEYS = {"name", "model", "widthMultiple",
                                                         "components", "planes"};
constexpr std::array<std::string_view, 4> COMPONENT_KEYS = {"kind", "bits", "log2SubX", "log2SubY"};
constexpr std::array<std::string_view, 7> PLANE_KEYS = {
    "groupPixels", "groupBytes", "wordBytes", "byteOrder", "rowAlign", "log2SubY", "slots"};
constexpr std::array<std::string_view, 4> SLOT_KEYS = {"component", "index", "word", "shift"};

constexpr std::string_view UNPACKED_SUFFIX = "@planes";
constexpr std::string_view DISPLAY_SUFFIX = "@display";

// "custom pixel format: <path>: <reason>"; the top-level object has an empty path.
[[nodiscard]] std::unexpected<Error> malformed(std::string_view path, std::string_view reason) {
    if (path.empty()) {
        return makeError(Errc::Format, std::format("custom pixel format: {}", reason));
    }
    return makeError(Errc::Format, std::format("custom pixel format: {}: {}", path, reason));
}

[[nodiscard]] std::string member(std::string_view path, std::string_view key) {
    return path.empty() ? std::string(key) : std::format("{}.{}", path, key);
}

// An object whose keys all come from `keys`.
[[nodiscard]] Result<void> checkObject(const json::Value& value, std::string_view path,
                                       std::span<const std::string_view> keys) {
    if (!value.is_object()) {
        return malformed(path, "not an object");
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (std::ranges::find(keys, std::string_view(it.key())) == keys.end()) {
            return malformed(path, std::format("unknown key '{}'", it.key()));
        }
    }
    return {};
}

// An integer key; a missing key gives `fallback` when there is one.
template <class T>
[[nodiscard]] Result<T> readInteger(const json::Value& object, std::string_view path,
                                    std::string_view key,
                                    std::optional<T> fallback = std::nullopt) {
    if (fallback && !object.contains(key)) {
        return *fallback;
    }
    auto value = json::get<T>(object, key);
    if (!value) {
        return malformed(member(path, key), value.error().message);
    }
    return *value;
}

// A string key with one of `names`; the result is the index into `names`.
[[nodiscard]] Result<std::size_t> readName(const json::Value& object, std::string_view path,
                                           std::string_view key,
                                           std::span<const std::string_view> names,
                                           std::optional<std::size_t> fallback = std::nullopt) {
    if (fallback && !object.contains(key)) {
        return *fallback;
    }
    auto text = json::get<std::string>(object, key);
    if (!text) {
        return malformed(member(path, key), text.error().message);
    }
    const auto found = std::ranges::find(names, std::string_view(*text));
    if (found == names.end()) {
        return malformed(member(path, key), std::format("unknown value '{}'", *text));
    }
    return static_cast<std::size_t>(found - names.begin());
}

// A required array key with 1..max entries.
[[nodiscard]] Result<const json::Value*> readArray(const json::Value& object, std::string_view path,
                                                   std::string_view key, std::size_t max) {
    const auto found = object.find(key);
    if (found == object.end()) {
        return malformed(member(path, key), std::format("missing JSON key '{}'", key));
    }
    if (!found->is_array()) {
        return malformed(member(path, key), "expected an array");
    }
    if (found->empty() || found->size() > max) {
        return malformed(member(path, key), std::format("expected 1 to {} entries", max));
    }
    return &*found;
}

[[nodiscard]] Result<ComponentDesc> parseComponent(const json::Value& value,
                                                   const std::string& path) {
    DVV_TRY_VOID(checkObject(value, path, COMPONENT_KEYS));
    ComponentDesc component;
    DVV_TRY(const std::size_t kind, readName(value, path, "kind", KINDS));
    component.kind = static_cast<ComponentKind>(kind);
    DVV_TRY(component.bits, readInteger<std::uint8_t>(value, path, "bits"));
    DVV_TRY(component.log2SubX,
            readInteger<std::uint8_t>(value, path, "log2SubX", std::uint8_t{0}));
    DVV_TRY(component.log2SubY,
            readInteger<std::uint8_t>(value, path, "log2SubY", std::uint8_t{0}));
    return component;
}

[[nodiscard]] Result<SampleSlot> parseSlot(const json::Value& value, const std::string& path) {
    DVV_TRY_VOID(checkObject(value, path, SLOT_KEYS));
    SampleSlot slot;
    DVV_TRY(slot.component, readInteger<std::uint8_t>(value, path, "component"));
    DVV_TRY(slot.index, readInteger<std::uint8_t>(value, path, "index"));
    DVV_TRY(slot.word, readInteger<std::uint8_t>(value, path, "word"));
    DVV_TRY(slot.shift, readInteger<std::uint8_t>(value, path, "shift"));
    return slot;
}

[[nodiscard]] Result<PlaneDesc> parsePlane(const json::Value& value, const std::string& path) {
    DVV_TRY_VOID(checkObject(value, path, PLANE_KEYS));
    PlaneDesc plane;
    DVV_TRY(plane.groupPixels, readInteger<std::uint8_t>(value, path, "groupPixels"));
    DVV_TRY(plane.groupBytes, readInteger<std::uint8_t>(value, path, "groupBytes"));
    DVV_TRY(plane.wordBytes, readInteger<std::uint8_t>(value, path, "wordBytes"));
    DVV_TRY(const std::size_t order, readName(value, path, "byteOrder", ORDERS, std::size_t{0}));
    plane.order = static_cast<ByteOrder>(order);
    DVV_TRY(plane.rowAlign, readInteger<std::uint16_t>(value, path, "rowAlign", std::uint16_t{1}));
    DVV_TRY(plane.log2SubY, readInteger<std::uint8_t>(value, path, "log2SubY", std::uint8_t{0}));
    DVV_TRY(const json::Value* slots, readArray(value, path, "slots", MAX_SLOTS));
    plane.slotCount = static_cast<std::uint8_t>(slots->size());
    for (std::size_t s = 0; s < slots->size(); ++s) {
        DVV_TRY(plane.slots[s], parseSlot((*slots)[s], std::format("{}.slots[{}]", path, s)));
    }
    return plane;
}

// Step 1 of formatFromJson: the structure only; validate() checks the values.
[[nodiscard]] Result<PixelFormatDescriptor> parseDescriptor(const json::Value& value) {
    const std::string path;
    DVV_TRY_VOID(checkObject(value, path, FORMAT_KEYS));
    PixelFormatDescriptor d;
    auto name = json::get<std::string>(value, "name");
    if (!name) {
        return malformed("name", name.error().message);
    }
    d.name = std::move(*name);
    DVV_TRY(const std::size_t model, readName(value, path, "model", MODELS));
    d.model = static_cast<ColourModel>(model);
    DVV_TRY(d.widthMultiple,
            readInteger<std::uint8_t>(value, path, "widthMultiple", std::uint8_t{1}));
    DVV_TRY(const json::Value* components, readArray(value, path, "components", MAX_COMPONENTS));
    d.componentCount = static_cast<std::uint8_t>(components->size());
    for (std::size_t c = 0; c < components->size(); ++c) {
        DVV_TRY(d.components[c],
                parseComponent((*components)[c], std::format("components[{}]", c)));
    }
    DVV_TRY(const json::Value* planes, readArray(value, path, "planes", MAX_PLANES));
    d.planeCount = static_cast<std::uint8_t>(planes->size());
    for (std::size_t p = 0; p < planes->size(); ++p) {
        DVV_TRY(d.planes[p], parsePlane((*planes)[p], std::format("planes[{}]", p)));
    }
    return d;
}

}  // namespace

json::Value formatToJson(FormatId format) {
    const PixelFormatDescriptor& d = descriptor(format);
    if (d.name.empty()) {
        return {};  // unknown id: every interned format has a non-empty name
    }
    json::Value components = json::Value::array();
    for (std::size_t c = 0; c < d.componentCount; ++c) {
        const ComponentDesc& component = d.components[c];
        json::Value item = json::Value::object();
        item["kind"] = KINDS.at(std::to_underlying(component.kind));
        item["bits"] = unsigned{component.bits};
        item["log2SubX"] = unsigned{component.log2SubX};
        item["log2SubY"] = unsigned{component.log2SubY};
        components.push_back(std::move(item));
    }
    json::Value planes = json::Value::array();
    for (std::size_t p = 0; p < d.planeCount; ++p) {
        const PlaneDesc& plane = d.planes[p];
        json::Value slots = json::Value::array();
        for (std::size_t s = 0; s < plane.slotCount; ++s) {
            const SampleSlot& slot = plane.slots[s];
            json::Value item = json::Value::object();
            item["component"] = unsigned{slot.component};
            item["index"] = unsigned{slot.index};
            item["word"] = unsigned{slot.word};
            item["shift"] = unsigned{slot.shift};
            slots.push_back(std::move(item));
        }
        json::Value item = json::Value::object();
        item["groupPixels"] = unsigned{plane.groupPixels};
        item["groupBytes"] = unsigned{plane.groupBytes};
        item["wordBytes"] = unsigned{plane.wordBytes};
        item["byteOrder"] = ORDERS.at(std::to_underlying(plane.order));
        item["rowAlign"] = unsigned{plane.rowAlign};
        item["log2SubY"] = unsigned{plane.log2SubY};
        item["slots"] = std::move(slots);
        planes.push_back(std::move(item));
    }
    json::Value result = json::Value::object();
    result["name"] = d.name;
    result["model"] = MODELS.at(std::to_underlying(d.model));
    result["widthMultiple"] = unsigned{d.widthMultiple};
    result["components"] = std::move(components);
    result["planes"] = std::move(planes);
    return result;
}

Result<FormatId> formatFromJson(const json::Value& value) {
    DVV_TRY(const PixelFormatDescriptor descriptor, parseDescriptor(value));
    for (const std::string_view suffix : {UNPACKED_SUFFIX, DISPLAY_SUFFIX}) {
        if (descriptor.name.ends_with(suffix)) {
            return makeError(Errc::Format,
                             std::format("custom pixel format '{}': the suffix {} is reserved",
                                         descriptor.name, suffix));
        }
    }
    DVV_TRY_VOID(validate(descriptor));
    return detail::internCustomFormat(descriptor);
}

}  // namespace dvv::media
