#pragma once

// The colour standards (D5R design §1): each names a standard and fills the four colour fields
// (matrix, range, transfer, primaries). The URI key `colour=<uriName>` (uri_colour.h), the clip
// description JSON and the format dialog use them. A standard's range is its default only: a range
// the file gives always wins (D5R §1.3), and `range=` may pick the other one (U-D5-9).

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"

namespace dvv::media {

enum class ColourPreset : std::uint8_t { Bt601Ntsc, Bt601Pal, Bt709, Bt2020, Bt2100Pq, Bt2100Hlg };

struct ColourPresetInfo {
    ColourPreset id;
    std::string_view uriName;  // the token (URI, JSON, file name), e.g. "bt2100-pq"
    std::string_view label;    // e.g. "BT.601 (PAL)", "BT.2100 PQ"
    ColourParams colour;       // all four fields set (range: the YUV default, limited)
};

// Every standard, in table order: bt601-ntsc, bt601-pal, bt709, bt2020, bt2100-pq, bt2100-hlg.
[[nodiscard]] std::span<const ColourPresetInfo> colourPresets() noexcept;
// The standard spelled `uri_name` (exact, lower case), or nullopt.
[[nodiscard]] std::optional<ColourPreset> findColourPreset(std::string_view uri_name) noexcept;
// The table row of `preset`; a value outside the enum gives the bt601-ntsc row.
[[nodiscard]] const ColourPresetInfo& colourPresetInfo(ColourPreset preset) noexcept;
// The standard whose four fields all equal those of `colour` (kr and kb are not compared), or
// nullopt.
[[nodiscard]] std::optional<ColourPreset> matchColourPreset(const ColourParams& colour) noexcept;
// The standard luma weights {Kr, Kb} of a matrix; nullopt for Rgb, Custom and Unspecified.
[[nodiscard]] std::optional<std::array<float, 2>> standardWeights(ColourMatrix matrix) noexcept;

// What a container says beyond the four fields (filled by the ffmpeg/lavfi providers; raw files,
// Y4M and synthetic sources give none).
enum class Bt601Hint : std::uint8_t {
    None,
    Pal,   // matrix code 5 (BT.470BG)
    Ntsc,  // matrix code 6 (SMPTE 170M)
};
struct FileHint {
    // Which BT.601 the matrix code names; used only when the primaries are not given.
    Bt601Hint bt601 = Bt601Hint::None;
    // The file signalled a primaries, transfer or matrix code that no field value here stands for
    // (e.g. SMPTE 240M); the field was left Unspecified, and the colour fits no standard.
    bool nonStandard = false;
    bool operator==(const FileHint&) const = default;
};

// The standard that the given fields point to (D5R §3.3 as aligned by Task 0); the range is
// ignored. Order: transfer PQ or HLG gives BT.2100; else the matrix (YUV formats only, not the GBR
// matrix) names BT.2020, BT.709 or BT.601; else the primaries name it; nothing given gives BT.709
// ("colour not signalled"). BT.601 is NTSC or PAL by the primaries, else by `hint.bt601`, else PAL.
// The result must agree with every given field (the transfer BT.1886 is the SDR curve of every
// non-BT.2100 standard); otherwise, or with a Custom matrix, the sRGB or linear curve or
// `hint.nonStandard`, nullopt ("no standard": the given fields are used as they are).
[[nodiscard]] std::optional<ColourPreset> standardOf(const ColourParams& given, ColourModel model,
                                                     FileHint hint) noexcept;

// The range when neither the URI nor the file gives one: limited for YUV, full for RGB and gray.
[[nodiscard]] ColourRange defaultRange(ColourModel model) noexcept;
// True when `range` is not the range of `preset` on `model` (U-D5-9 "(not standard)" note): full
// range with BT.601, BT.709 or BT.2020 on a YUV format.
[[nodiscard]] bool rangeNotStandard(ColourPreset preset, ColourModel model,
                                    ColourRange range) noexcept;

// U-D5-8 "Auto detect": the usual standard of an untagged picture `height` lines high (<= 480
// lines: BT.601 (NTSC); <= 576: BT.601 (PAL); else BT.709).
[[nodiscard]] ColourPreset guessStandard(std::uint32_t height) noexcept;
// U-D5-8 mismatch hint: "SD size: BT.601 is common for SD" for BT.709, BT.2020 or BT.2100 at 576
// lines or less; "HD size: BT.709 is common" for BT.601 above 576 lines; else "".
[[nodiscard]] std::string_view sizeHint(ColourPreset preset, std::uint32_t height) noexcept;

}  // namespace dvv::media
