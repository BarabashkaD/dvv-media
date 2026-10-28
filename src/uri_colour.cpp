#include "dvv/media/uri_colour.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dvv/media/colour_preset.h"
#include "dvv/media/types.h"

namespace dvv::media {

namespace {

// The canonical query spelling of an enum value.
template <class E>
struct Name {
    std::string_view text;
    E value;
};
constexpr std::array<Name<ColourRange>, 2> RANGES{{
    {"full", ColourRange::Full},
    {"limited", ColourRange::Limited},
}};
constexpr std::array<Name<ColourMatrix>, 5> MATRICES{{
    {"bt601", ColourMatrix::Bt601},
    {"bt709", ColourMatrix::Bt709},
    {"bt2020", ColourMatrix::Bt2020Ncl},
    {"rgb", ColourMatrix::Rgb},
    {"custom", ColourMatrix::Custom},
}};
constexpr std::array<Name<TransferFn>, 5> TRANSFERS{{
    {"srgb", TransferFn::Srgb},
    {"bt1886", TransferFn::Bt1886},
    {"linear", TransferFn::Linear},
    {"pq", TransferFn::Pq},
    {"hlg", TransferFn::Hlg},
}};
constexpr std::array<Name<Primaries>, 4> PRIMARIES{{
    {"bt601-525", Primaries::Bt601_525},
    {"bt601-625", Primaries::Bt601_625},
    {"bt709", Primaries::Bt709},
    {"bt2020", Primaries::Bt2020},
}};

// The value named `text`, or Errc::Format "bad <parameter> '<text>' (<every name>)".
template <class E, std::size_t N>
Result<E> parseName(std::string_view parameter, std::string_view text,
                    const std::array<Name<E>, N>& names) {
    std::string list;
    for (const Name<E>& name : names) {
        if (name.text == text) {
            return name.value;
        }
        list += list.empty() ? "" : ", ";
        list += name.text;
    }
    return makeError(Errc::Format, std::format("bad {} '{}' ({})", parameter, text, list));
}

// The name of a value that is in `names`.
template <class E, std::size_t N>
std::string nameOf(E value, const std::array<Name<E>, N>& names) {
    const auto found = std::ranges::find(names, value, &Name<E>::value);
    return found == names.end() ? std::string() : std::string(found->text);
}

// The colour keys that D5R removed (§3.1).
constexpr std::array<std::string_view, 5> REMOVED_KEYS{"matrix", "kr", "kb", "transfer",
                                                       "primaries"};

// "bt601-ntsc, bt601-pal, bt709, bt2020, bt2100-pq, bt2100-hlg".
std::string standardList() {
    std::string list;
    for (const ColourPresetInfo& info : colourPresets()) {
        list += list.empty() ? "" : ", ";
        list += info.uriName;
    }
    return list;
}

// The standard named `text`, or Errc::Format "bad colour '<text>' (<every token>)".
Result<ColourPreset> parsePreset(std::string_view text) {
    if (const auto preset = findColourPreset(text)) {
        return *preset;
    }
    return makeError(Errc::Format, std::format("bad colour '{}' ({})", text, standardList()));
}

}  // namespace

std::string unknownParameterText(std::string_view name) {
    if (std::ranges::find(REMOVED_KEYS, name) != REMOVED_KEYS.end()) {
        return std::format("unknown parameter '{}' (set the colour with colour=<standard>: {})",
                           name, standardList());
    }
    return std::format("unknown parameter '{}'", name);
}

std::string_view colourName(ColourRange range) noexcept {
    const auto found = std::ranges::find(RANGES, range, &Name<ColourRange>::value);
    return found == RANGES.end() ? std::string_view() : found->text;
}

std::string_view colourName(ColourMatrix matrix) noexcept {
    const auto found = std::ranges::find(MATRICES, matrix, &Name<ColourMatrix>::value);
    return found == MATRICES.end() ? std::string_view() : found->text;
}

std::string_view colourName(TransferFn transfer) noexcept {
    const auto found = std::ranges::find(TRANSFERS, transfer, &Name<TransferFn>::value);
    return found == TRANSFERS.end() ? std::string_view() : found->text;
}

std::string_view colourName(Primaries primaries) noexcept {
    const auto found = std::ranges::find(PRIMARIES, primaries, &Name<Primaries>::value);
    return found == PRIMARIES.end() ? std::string_view() : found->text;
}

Result<ColourRange> parseColourRange(std::string_view text) {
    return parseName("range", text, RANGES);
}

Result<ColourMatrix> parseColourMatrix(std::string_view text) {
    return parseName("matrix", text, MATRICES);
}

Result<TransferFn> parseTransferFn(std::string_view text) {
    return parseName("transfer", text, TRANSFERS);
}

Result<Primaries> parsePrimaries(std::string_view text) {
    return parseName("primaries", text, PRIMARIES);
}

ColourParams merged(const ColourSpec& spec, ColourModel model) noexcept {
    ColourParams colour{};
    if (spec.preset) {
        const ColourParams& standard = colourPresetInfo(*spec.preset).colour;
        // RGB and gray: the matrix is not used (resolveColour gives Rgb).
        if (model == ColourModel::Yuv) {
            colour.matrix = standard.matrix;
            colour.kr = standard.kr;
            colour.kb = standard.kb;
        }
        colour.transfer = standard.transfer;
        colour.primaries = standard.primaries;
    }
    // Only range= sets the range, never the standard (D5R §1.3): a file's range wins over it.
    colour.range = spec.range;
    return colour;
}

ColourParams merged(const ColourSpec& spec) noexcept {
    return merged(spec, ColourModel::Yuv);
}

Result<ColourParams> colourFromQuery(const Uri& uri) {
    DVV_TRY(const ColourSpec spec, colourSpecFromQuery(uri));
    return merged(spec);
}

Result<ColourSpec> colourSpecFromQuery(const Uri& uri) {
    ColourSpec spec;
    if (const auto preset = uri.param("colour")) {
        DVV_TRY(spec.preset, parsePreset(*preset));
    }
    if (const auto range = uri.param("range")) {
        DVV_TRY(spec.range, parseName("range", *range, RANGES));
    }
    return spec;
}

std::vector<std::pair<std::string, std::string>> colourParameters(const ColourParams& colour) {
    std::vector<std::pair<std::string, std::string>> result;
    if (colour.range != ColourRange::Unspecified) {
        result.emplace_back("range", nameOf(colour.range, RANGES));
    }
    if (colour.matrix != ColourMatrix::Unspecified) {
        result.emplace_back("matrix", nameOf(colour.matrix, MATRICES));
    }
    if (colour.matrix == ColourMatrix::Custom) {
        result.emplace_back("kr", std::format("{}", colour.kr));
        result.emplace_back("kb", std::format("{}", colour.kb));
    }
    if (colour.transfer != TransferFn::Unspecified) {
        result.emplace_back("transfer", nameOf(colour.transfer, TRANSFERS));
    }
    if (colour.primaries != Primaries::Unspecified) {
        result.emplace_back("primaries", nameOf(colour.primaries, PRIMARIES));
    }
    return result;
}

std::vector<std::pair<std::string, std::string>> colourParameters(const ColourSpec& spec) {
    std::vector<std::pair<std::string, std::string>> result;
    if (spec.preset) {
        result.emplace_back("colour", colourPresetInfo(*spec.preset).uriName);
    }
    if (spec.range != ColourRange::Unspecified) {
        result.emplace_back("range", nameOf(spec.range, RANGES));
    }
    return result;
}

}  // namespace dvv::media
