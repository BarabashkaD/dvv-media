#pragma once

// The colour query parameters of source URIs (spec §6.3, §7.2; D5R design §3.1): colour (a
// standard, colour_preset.h) and range. Every provider that takes colour overrides parses them
// here (raw, synthetic and Y4M in dvv-media, the ffmpeg provider in dvv-components), and resolved
// URIs write back the query's own form with colourParameters(ColourSpec). The keys matrix, kr, kb,
// transfer and primaries are gone; unknownParameterText() names the replacement.

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/uri.h"
#include "dvv/media/colour_preset.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"

namespace dvv::media {

// The colour query parameters every provider accepts.
inline constexpr std::array<std::string_view, 2> COLOUR_PARAMETERS{"colour", "range"};

// The colour of a query (or a clip description) as the user wrote it: the standard (colour=) and
// the range (range=), each optional.
struct ColourSpec {
    std::optional<ColourPreset> preset{};
    ColourRange range{};  // Unspecified when range= is not given
    bool operator==(const ColourSpec&) const = default;
};

// The text for a query parameter no provider knows. The removed colour keys matrix, kr, kb,
// transfer and primaries give "unknown parameter 'matrix' (set the colour with colour=<standard>:
// bt601-ntsc, bt601-pal, bt709, bt2020, bt2100-pq, bt2100-hlg)"; any other name gives "unknown
// parameter 'x'". Used by every provider's parameter check (dvv-media and dvv-components).
[[nodiscard]] std::string unknownParameterText(std::string_view name);

// colour=bt601-ntsc|bt601-pal|bt709|bt2020|bt2100-pq|bt2100-hlg; range=full|limited (allowed with
// every standard and every format, U-D5-9). Errc::Format: "bad colour 'x' (bt601-ntsc, bt601-pal,
// bt709, bt2020, bt2100-pq, bt2100-hlg)" or "bad range 'x' (full, limited)".
[[nodiscard]] Result<ColourSpec> colourSpecFromQuery(const Uri& uri);
// The colour to render for a format of colour model `model` (D5R §1.3): the standard sets the
// matrix (YUV only; RGB and gray leave it Unspecified, resolveColour() gives Rgb), the transfer
// and the primaries; the range comes only from spec.range, never from the standard (so a range the
// file gives wins, else resolveColour()'s default: limited for YUV, full for RGB and gray). No
// standard: matrix, transfer and primaries Unspecified. Providers call it once the format is
// known; the resolved URI keeps the query's form (colourParameters(ColourSpec)).
[[nodiscard]] ColourParams merged(const ColourSpec& spec, ColourModel model) noexcept;
// merged(spec, ColourModel::Yuv).
[[nodiscard]] ColourParams merged(const ColourSpec& spec) noexcept;
// merged(colourSpecFromQuery(uri)), with the same errors.
[[nodiscard]] Result<ColourParams> colourFromQuery(const Uri& uri);

// The four fields of a colour (CLI output, tests): the fields that are not Unspecified, in the
// order range, matrix, kr, kb, transfer, primaries, with the names below; kr and kb (Custom only)
// as std::format("{}", value). These are not URI keys (only colour= and range= are).
[[nodiscard]] std::vector<std::pair<std::string, std::string>> colourParameters(
    const ColourParams& colour);
// The query's own form: colour=<token> (when a standard is named), then range=<range> (when
// given). Pass a typed argument: colourParameters({}) is ambiguous between the two overloads.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> colourParameters(
    const ColourSpec& spec);

// The spellings of the field values (range: full, limited; matrix: bt601, bt709, bt2020, rgb,
// custom; transfer: srgb, bt1886, linear, pq, hlg; primaries: bt601-525, bt601-625, bt709,
// bt2020); "" for Unspecified.
[[nodiscard]] std::string_view colourName(ColourRange range) noexcept;
[[nodiscard]] std::string_view colourName(ColourMatrix matrix) noexcept;
[[nodiscard]] std::string_view colourName(TransferFn transfer) noexcept;
[[nodiscard]] std::string_view colourName(Primaries primaries) noexcept;
// The value of a URI spelling. Errc::Format "bad range|matrix|transfer|primaries 'x' (<names>)" as
// above.
[[nodiscard]] Result<ColourRange> parseColourRange(std::string_view text);
[[nodiscard]] Result<ColourMatrix> parseColourMatrix(std::string_view text);
[[nodiscard]] Result<TransferFn> parseTransferFn(std::string_view text);
[[nodiscard]] Result<Primaries> parsePrimaries(std::string_view text);

}  // namespace dvv::media
