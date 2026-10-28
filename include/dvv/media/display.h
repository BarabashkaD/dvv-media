#pragma once

// The display view and its colour model (spec §6.3, §8; design M3 D3-D6, D5R §1-§2). ToDisplay
// turns code values into linear-light RGB; the primaries are not converted (the screen's are
// assumed). The CPU reference here and
// shaders/to_display.slang use the same float32 formulas.

#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "dvv/media/colour_preset.h"
#include "dvv/media/types.h"

namespace dvv::media {

// The display format: RGBA, four 16-bit words holding IEEE half floats, linear light. Interned on
// first use as "rgba16f@display" (model Rgb, components R16 G16 B16 A16, one plane of groups of one
// pixel in 8 bytes, little-endian), so geometry() works. Never an original: acquireOriginal refuses
// an origin of this format (Internal).
[[nodiscard]] FormatId displayFormat();
// True for the display format (compares the name; never interns).
[[nodiscard]] bool isDisplay(FormatId format) noexcept;
// {Rgb, Full, Linear, Bt709}.
[[nodiscard]] ColourId displayColour();
// {displayFormat(), displayColour(), LayoutPolicy::Any, Api::VkImage, device_domain, 0}.
[[nodiscard]] ViewSpec displayView(const Domain& device_domain);

// Which fields resolveColour() filled in because the frame left them Unspecified.
enum AssumedColour : std::uint8_t {
    AssumedMatrix = 1,
    AssumedRange = 2,
    AssumedTransfer = 4,
    AssumedPrimaries = 8,
};
struct ResolvedColour {
    ColourParams params;       // no Unspecified field left
    std::uint8_t assumed = 0;  // AssumedColour bits
    // standardOf() of the given fields; nullopt: "no standard" (the given fields kept as they are).
    // Bt709 with every AssumedColour bit of matrix (YUV), transfer and primaries set: "colour not
    // signalled".
    std::optional<ColourPreset> standard;
    bool operator==(const ResolvedColour&) const = default;
};
// The colour of `desc` (D5R §1.3, M4-D5R-NSPIX2): missing fields (left Unspecified by the frame)
// come from the standard the given fields point to (standardOf with `hint`), else BT.709. A
// nonStandard hint gives no standard (BT.709 fills); a BT.601 hint picks the PAL or NTSC primaries.
// Callers that show the values pass the default hint, as the renderer does. The matrix of an RGB
// or gray format is Rgb. The range never comes from a standard: the frame's range, else
// defaultRange(model). The primaries are information only (no gamut conversion).
[[nodiscard]] ResolvedColour resolveColour(const FrameDesc& desc, FileHint hint = {});

// rgb' = matrix * (c0, c1, c2) + offset over the format's component code values (canonical
// component order: Y U V or R G B; gray uses c0). Row-major 3x3. Folds range normalisation, the
// YUV -> R'G'B' matrix, component order and gray.
struct DisplayAffine {
    std::array<float, 9> matrix{};
    std::array<float, 3> offset{};
};
[[nodiscard]] DisplayAffine displayAffine(FormatId format, const ColourParams& resolved);

// CPU reference of ToDisplay for one pixel. `code_values` in component order (alpha ignored;
// missing entries count as 0). Returns linear-light RGBA (primaries not converted), each in
// [0, 1], alpha 1.
[[nodiscard]] std::array<float, 4> toDisplayReference(
    FormatId format, const ColourParams& resolved,
    std::span<const std::uint16_t> code_values) noexcept;

// IEC 61966-2-1 OETF and its inverse, input clamped to [0, 1].
[[nodiscard]] float srgbEncode(float linear) noexcept;
[[nodiscard]] float srgbDecode(float encoded) noexcept;
[[nodiscard]] std::uint8_t encodeSrgb8(float linear) noexcept;  // lround(255 * srgbEncode(linear))
[[nodiscard]] std::uint16_t floatToHalf(float value) noexcept;  // round to nearest even
[[nodiscard]] float halfToFloat(std::uint16_t half) noexcept;   // exact

}  // namespace dvv::media
