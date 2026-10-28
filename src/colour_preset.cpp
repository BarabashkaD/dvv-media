#include "dvv/media/colour_preset.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"

namespace dvv::media {

namespace {

constexpr ColourParams preset(ColourMatrix matrix, TransferFn transfer, Primaries primaries) {
    return {.matrix = matrix,
            .range = ColourRange::Limited,
            .transfer = transfer,
            .primaries = primaries,
            .kr = 0,
            .kb = 0};
}

// D5R design §1.1; the order is the enum's order.
constexpr std::array<ColourPresetInfo, 6> PRESETS{{
    {ColourPreset::Bt601Ntsc, "bt601-ntsc", "BT.601 (NTSC)",
     preset(ColourMatrix::Bt601, TransferFn::Bt1886, Primaries::Bt601_525)},
    {ColourPreset::Bt601Pal, "bt601-pal", "BT.601 (PAL)",
     preset(ColourMatrix::Bt601, TransferFn::Bt1886, Primaries::Bt601_625)},
    {ColourPreset::Bt709, "bt709", "BT.709",
     preset(ColourMatrix::Bt709, TransferFn::Bt1886, Primaries::Bt709)},
    {ColourPreset::Bt2020, "bt2020", "BT.2020",
     preset(ColourMatrix::Bt2020Ncl, TransferFn::Bt1886, Primaries::Bt2020)},
    {ColourPreset::Bt2100Pq, "bt2100-pq", "BT.2100 PQ",
     preset(ColourMatrix::Bt2020Ncl, TransferFn::Pq, Primaries::Bt2020)},
    {ColourPreset::Bt2100Hlg, "bt2100-hlg", "BT.2100 HLG",
     preset(ColourMatrix::Bt2020Ncl, TransferFn::Hlg, Primaries::Bt2020)},
}};

constexpr std::uint32_t SD_MAX_LINES = 576;
constexpr std::uint32_t NTSC_MAX_LINES = 480;

bool isBt601(ColourPreset preset) noexcept {
    return preset == ColourPreset::Bt601Ntsc || preset == ColourPreset::Bt601Pal;
}

// The BT.601 standard of the given primaries, else of the hint, else PAL.
ColourPreset bt601Of(Primaries primaries, Bt601Hint hint) noexcept {
    if (primaries == Primaries::Bt601_525) {
        return ColourPreset::Bt601Ntsc;
    }
    if (primaries == Primaries::Bt601_625) {
        return ColourPreset::Bt601Pal;
    }
    if (primaries == Primaries::Unspecified && hint == Bt601Hint::Ntsc) {
        return ColourPreset::Bt601Ntsc;
    }
    return ColourPreset::Bt601Pal;
}

// The candidate named by the first given field in the order curve (PQ/HLG), matrix, primaries.
ColourPreset candidateOf(const ColourParams& given, bool compare_matrix, Bt601Hint hint) noexcept {
    if (given.transfer == TransferFn::Pq) {
        return ColourPreset::Bt2100Pq;
    }
    if (given.transfer == TransferFn::Hlg) {
        return ColourPreset::Bt2100Hlg;
    }
    if (compare_matrix) {
        switch (given.matrix) {
            case ColourMatrix::Bt2020Ncl: return ColourPreset::Bt2020;
            case ColourMatrix::Bt709: return ColourPreset::Bt709;
            case ColourMatrix::Bt601: return bt601Of(given.primaries, hint);
            case ColourMatrix::Unspecified:
            case ColourMatrix::Rgb:
            case ColourMatrix::Custom: break;
        }
    }
    switch (given.primaries) {
        case Primaries::Bt2020: return ColourPreset::Bt2020;
        case Primaries::Bt601_525: return ColourPreset::Bt601Ntsc;
        case Primaries::Bt601_625: return ColourPreset::Bt601Pal;
        case Primaries::Bt709:
        case Primaries::Unspecified: break;
    }
    return ColourPreset::Bt709;
}

}  // namespace

std::span<const ColourPresetInfo> colourPresets() noexcept {
    return PRESETS;
}

std::optional<ColourPreset> findColourPreset(std::string_view uri_name) noexcept {
    const auto found = std::ranges::find(PRESETS, uri_name, &ColourPresetInfo::uriName);
    return found == PRESETS.end() ? std::nullopt : std::optional(found->id);
}

const ColourPresetInfo& colourPresetInfo(ColourPreset preset) noexcept {
    const auto index = static_cast<std::size_t>(preset);
    return index < PRESETS.size() ? PRESETS[index] : PRESETS.front();
}

std::optional<ColourPreset> matchColourPreset(const ColourParams& colour) noexcept {
    const auto found = std::ranges::find_if(PRESETS, [&](const ColourPresetInfo& info) {
        return info.colour.matrix == colour.matrix && info.colour.range == colour.range &&
               info.colour.transfer == colour.transfer && info.colour.primaries == colour.primaries;
    });
    return found == PRESETS.end() ? std::nullopt : std::optional(found->id);
}

std::optional<std::array<float, 2>> standardWeights(ColourMatrix matrix) noexcept {
    switch (matrix) {
        case ColourMatrix::Bt601: return std::array{0.299f, 0.114f};
        case ColourMatrix::Bt709: return std::array{0.2126f, 0.0722f};
        case ColourMatrix::Bt2020Ncl: return std::array{0.2627f, 0.0593f};
        case ColourMatrix::Unspecified:
        case ColourMatrix::Rgb:
        case ColourMatrix::Custom: break;
    }
    return std::nullopt;
}

std::optional<ColourPreset> standardOf(const ColourParams& given, ColourModel model,
                                       FileHint hint) noexcept {
    // The GBR matrix on a YUV format counts as an RGB format: the matrix is not compared.
    const bool compare_matrix = model == ColourModel::Yuv && given.matrix != ColourMatrix::Rgb;
    if (hint.nonStandard || (compare_matrix && given.matrix == ColourMatrix::Custom)) {
        return std::nullopt;
    }
    const ColourPreset candidate = candidateOf(given, compare_matrix, hint.bt601);
    const ColourParams& standard = colourPresetInfo(candidate).colour;
    const bool fits =
        (!compare_matrix || given.matrix == ColourMatrix::Unspecified ||
         given.matrix == standard.matrix) &&
        (given.transfer == TransferFn::Unspecified || given.transfer == standard.transfer) &&
        (given.primaries == Primaries::Unspecified || given.primaries == standard.primaries);
    return fits ? std::optional(candidate) : std::nullopt;
}

ColourRange defaultRange(ColourModel model) noexcept {
    return model == ColourModel::Yuv ? ColourRange::Limited : ColourRange::Full;
}

bool rangeNotStandard(ColourPreset preset, ColourModel model, ColourRange range) noexcept {
    const bool sdr = preset != ColourPreset::Bt2100Pq && preset != ColourPreset::Bt2100Hlg;
    return sdr && model == ColourModel::Yuv && range == ColourRange::Full;
}

ColourPreset guessStandard(std::uint32_t height) noexcept {
    if (height <= NTSC_MAX_LINES) {
        return ColourPreset::Bt601Ntsc;
    }
    return height <= SD_MAX_LINES ? ColourPreset::Bt601Pal : ColourPreset::Bt709;
}

std::string_view sizeHint(ColourPreset preset, std::uint32_t height) noexcept {
    if (isBt601(preset)) {
        return height > SD_MAX_LINES ? "HD size: BT.709 is common" : "";
    }
    return height <= SD_MAX_LINES ? "SD size: BT.601 is common for SD" : "";
}

}  // namespace dvv::media
