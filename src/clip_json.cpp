#include "dvv/media/clip_json.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "dvv/base/json.h"
#include "dvv/media/colour_preset.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"
#include "dvv/platform/file_reader.h"

namespace dvv::media {

namespace {

constexpr std::array<std::string_view, 5> CLIP_KEYS = {"rawreelClip", "format", "size", "frameRate",
                                                       "colour"};
constexpr std::array<std::string_view, 2> SIZE_KEYS = {"width", "height"};
constexpr std::array<std::string_view, 2> COLOUR_KEYS = {"standard", "range"};

// The readSmallFile() limit: a clip or pixel-format description is a few hundred bytes.
constexpr std::uint64_t MAX_SMALL_FILE = std::uint64_t{1} << 20U;
constexpr std::string_view FPS_FORM = R"(write N or N/D, e.g. "30000/1001")";

// Builds the errors of one clip file: "clip description '<file>': [<path>: ]<reason>".
class Errors {
public:
    explicit Errors(std::string_view file) : file_(file) {}

    [[nodiscard]] std::unexpected<Error> at(std::string_view path, std::string_view reason) const {
        if (path.empty()) {
            return makeError(Errc::Format, std::format("clip description '{}': {}", file_, reason));
        }
        return makeError(Errc::Format,
                         std::format("clip description '{}': {}: {}", file_, path, reason));
    }

private:
    std::string_view file_;
};

[[nodiscard]] std::string member(std::string_view path, std::string_view key) {
    return path.empty() ? std::string(key) : std::format("{}.{}", path, key);
}

// An object whose keys all come from `keys`.
[[nodiscard]] Result<void> checkObject(const Errors& errors, const json::Value& value,
                                       std::string_view path,
                                       std::span<const std::string_view> keys) {
    if (!value.is_object()) {
        return errors.at(path, "not an object");
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (std::ranges::find(keys, std::string_view(it.key())) == keys.end()) {
            return errors.at(path, std::format("unknown key '{}'", it.key()));
        }
    }
    return {};
}

// object[key], which must be there.
[[nodiscard]] Result<const json::Value*> required(const Errors& errors, const json::Value& object,
                                                  std::string_view path, std::string_view key) {
    const auto found = object.find(key);
    if (found == object.end()) {
        return errors.at(path, std::format("missing key '{}'", key));
    }
    return &*found;
}

[[nodiscard]] Result<std::string> readString(const Errors& errors, const json::Value& value,
                                             std::string_view path) {
    if (!value.is_string()) {
        return errors.at(path, "expected a string");
    }
    return value.get<std::string>();
}

// A colour= name, with the URI's message for a bad one.
[[nodiscard]] Result<ColourPreset> readPreset(const Errors& errors, const json::Value& value,
                                              std::string_view path) {
    DVV_TRY(const std::string name, readString(errors, value, path));
    if (const std::optional<ColourPreset> preset = findColourPreset(name)) {
        return *preset;
    }
    std::string names;
    for (const ColourPresetInfo& info : colourPresets()) {
        names += names.empty() ? "" : ", ";
        names += info.uriName;
    }
    return errors.at(path, std::format("bad colour '{}' ({})", name, names));
}

// "@<path>" with a relative path joined to the clip file's folder; '\' is read as '/'.
[[nodiscard]] std::string anchored(std::string_view at_path, std::string_view clip_path) {
    std::string path(at_path);
    std::ranges::replace(path, '\\', '/');
    const bool drive = path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) != 0 &&
                       path[1] == ':' && path[2] == '/';
    if (path.starts_with('/') || drive) {
        return "@" + path;
    }
    std::string folder(clip_path);
    std::ranges::replace(folder, '\\', '/');
    const std::size_t slash = folder.rfind('/');
    if (slash == std::string::npos) {
        return "@" + path;
    }
    folder.resize(slash + 1);
    return "@" + folder + path;
}

[[nodiscard]] Result<std::string> parseFormat(const Errors& errors, const json::Value& value,
                                              std::string_view clip_path) {
    DVV_TRY(std::string text, readString(errors, value, "format"));
    if (text.starts_with('@')) {
        if (text.size() == 1) {
            return errors.at("format", "empty path after '@'");
        }
        return anchored(std::string_view(text).substr(1), clip_path);
    }
    // Catalogue names only (as fmt= in the raw provider).
    const std::optional<FormatId> format = findFormat(text);
    if (!format || std::ranges::find(catalogue(), *format) == catalogue().end()) {
        return errors.at("format", std::format("unknown pixel format '{}'", text));
    }
    return text;
}

[[nodiscard]] Result<std::uint32_t> readDimension(const Errors& errors, const json::Value& value,
                                                  std::string_view key) {
    auto number = json::get<std::uint32_t>(value, key);
    if (!number || *number < 1 || *number > 65535) {
        return errors.at(member("size", key), "expected an integer 1..65535");
    }
    return *number;
}

[[nodiscard]] Result<std::pair<std::uint32_t, std::uint32_t>> parseSize(const Errors& errors,
                                                                        const json::Value& value) {
    DVV_TRY_VOID(checkObject(errors, value, "size", SIZE_KEYS));
    for (const std::string_view key : SIZE_KEYS) {
        DVV_TRY_VOID(required(errors, value, "size", key));
    }
    DVV_TRY(const std::uint32_t width, readDimension(errors, value, "width"));
    DVV_TRY(const std::uint32_t height, readDimension(errors, value, "height"));
    return std::pair{width, height};
}

// A decimal integer > 0 (digits only).
[[nodiscard]] std::optional<std::int64_t> positive(std::string_view text) {
    if (text.empty() || !std::ranges::all_of(text, [](char c) {
            return std::isdigit(static_cast<unsigned char>(c)) != 0;
        })) {
        return std::nullopt;
    }
    std::int64_t number = 0;
    const char* const first = std::to_address(text.begin());
    const char* const last = std::to_address(text.end());
    const auto [end, error] = std::from_chars(first, last, number);
    if (error != std::errc{} || end != last || number <= 0) {
        return std::nullopt;
    }
    return number;
}

// An integer > 0, "N" or "N/D"; decimals are refused (no 29.97 trap).
[[nodiscard]] Result<Rational> parseFrameRate(const Errors& errors, const json::Value& value) {
    std::optional<std::int64_t> num;
    std::optional<std::int64_t> den = 1;
    if (value.is_number_integer()) {
        if (value.is_number_unsigned()) {
            const auto number = value.get<std::uint64_t>();
            if (std::in_range<std::int64_t>(number) && number > 0) {
                num = static_cast<std::int64_t>(number);
            }
        } else if (const auto number = value.get<std::int64_t>(); number > 0) {
            num = number;
        }
    } else if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        const std::size_t slash = text.find('/');
        num = positive(std::string_view(text).substr(0, slash));
        if (slash != std::string::npos) {
            den = positive(std::string_view(text).substr(slash + 1));
        }
    }
    if (!num || !den) {
        return errors.at("frameRate", FPS_FORM);
    }
    const std::int64_t divisor = std::gcd(*num, *den);
    return Rational{.num = *num / divisor, .den = *den / divisor};
}

// "colour": {"standard": "<token>", "range": "full"|"limited"} (D5R §3.2); range is optional.
[[nodiscard]] Result<ColourSpec> parseColour(const Errors& errors, const json::Value& value) {
    constexpr std::string_view PATH = "colour";
    if (!value.is_object()) {
        return errors.at(PATH, "not an object");
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (std::ranges::find(COLOUR_KEYS, std::string_view(it.key())) == COLOUR_KEYS.end()) {
            return errors.at(PATH, std::format("unknown key '{}' (standard, range)", it.key()));
        }
    }
    DVV_TRY(const json::Value* standard, required(errors, value, PATH, "standard"));
    ColourSpec spec{.preset = std::nullopt, .range = ColourRange::Unspecified};
    DVV_TRY(spec.preset, readPreset(errors, *standard, "colour.standard"));
    if (const auto range = value.find("range"); range != value.end()) {
        DVV_TRY(const std::string text, readString(errors, *range, "colour.range"));
        auto parsed = parseColourRange(text);
        if (!parsed) {
            return errors.at("colour.range", parsed.error().message);
        }
        spec.range = *parsed;
    }
    return spec;
}

}  // namespace

Result<ClipDescription> parseClipJson(std::string_view text, std::string_view clip_utf8_path) {
    const Errors errors(clip_utf8_path);
    auto parsed = json::parse(text);
    if (!parsed) {
        return errors.at("", parsed.error().message);
    }
    const json::Value& root = *parsed;
    if (!root.is_object()) {
        return errors.at("", "not an object");
    }
    // The version first: a later version may have keys this one does not know.
    DVV_TRY(const json::Value* version, required(errors, root, "", "rawreelClip"));
    // Any number but the integer 1 is a version this rawreel does not read (2.0 and 1.0 too).
    if (!version->is_number()) {
        return errors.at("rawreelClip", "expected the integer 1");
    }
    if (!version->is_number_integer() ||
        (version->is_number_unsigned() ? version->get<std::uint64_t>() != 1
                                       : version->get<std::int64_t>() != 1)) {
        return errors.at("rawreelClip", std::format("unsupported version {} (this rawreel reads 1)",
                                                    json::dump(*version)));
    }
    DVV_TRY_VOID(checkObject(errors, root, "", CLIP_KEYS));
    if (root.size() == 1) {
        return errors.at("", "give at least one of 'format', 'size', 'frameRate' or 'colour'");
    }

    ClipDescription clip{};
    if (const auto found = root.find("format"); found != root.end()) {
        DVV_TRY(clip.format, parseFormat(errors, *found, clip_utf8_path));
    }
    if (const auto found = root.find("size"); found != root.end()) {
        DVV_TRY(clip.size, parseSize(errors, *found));
    }
    if (const auto found = root.find("frameRate"); found != root.end()) {
        DVV_TRY(clip.fps, parseFrameRate(errors, *found));
    }
    if (const auto found = root.find("colour"); found != root.end()) {
        DVV_TRY(clip.colour, parseColour(errors, *found));
    }
    return clip;
}

Result<std::string> readSmallFile(std::string_view utf8_path) {
    DVV_TRY(const platform::FileReader reader, platform::FileReader::open(pathFromUtf8(utf8_path)));
    if (reader.size() > MAX_SMALL_FILE) {
        return makeError(Errc::Format, std::format("'{}' is too large", utf8_path));
    }
    std::string text(static_cast<std::size_t>(reader.size()), '\0');
    if (!text.empty()) {
        DVV_TRY(const std::size_t read, reader.readAt(0, std::as_writable_bytes(std::span(text))));
        text.resize(read);
    }
    return text;
}

Result<ClipDescription> loadClipJson(std::string_view utf8_path) {
    DVV_TRY(const std::string text, readSmallFile(utf8_path));
    return parseClipJson(text, utf8_path);
}

}  // namespace dvv::media
