#pragma once

// The clip description JSON (D5 design §3): `{"rawreelClip": 1, ...}` names a raw clip's format,
// size, frame rate and colour. Every key but rawreelClip is optional; at least one group is given.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "dvv/base/error.h"
#include "dvv/media/types.h"
#include "dvv/media/uri_colour.h"

namespace dvv::media {

struct ClipDescription {
    // A catalogue name, or "@<path>" to a pixel-format JSON: a relative path is joined to the
    // clip file's folder (so it is absolute when the clip path is), with '/' separators.
    std::optional<std::string> format{};
    std::optional<std::pair<std::uint32_t, std::uint32_t>> size{};  // width, height: 1..65535
    std::optional<Rational> fps{};                                  // reduced, num and den > 0
    // "colour": {"standard": "<token>", "range": "full"|"limited"} (D5R §3.2): the preset is
    // always set; the range is Unspecified when not given. The raw provider uses it as it uses
    // colour= and range= (the query wins per key).
    std::optional<ColourSpec> colour{};
    bool operator==(const ClipDescription&) const = default;
};

// Parses `text` strictly (an unknown key at any level is an error). `clip_utf8_path` names the
// file in the errors and anchors relative "@" paths; the file is not read here (loadClipJson()
// reads it). Errc::Format "clip description '<file>': <json path>: <reason>" (no "<json path>: "
// at the top level), e.g. "rawreelClip: unsupported version 2 (this rawreel reads 1)" (any number
// but the integer 1, so 2.0 too; a non-number is "rawreelClip: expected the integer 1"),
// "size: unknown key 'w'", "format: unknown pixel format 'yuv421p'", "frameRate: write N or N/D,
// e.g. \"30000/1001\"", "colour: missing key 'standard'", "colour: unknown key 'custom'
// (standard, range)", "colour.standard: bad colour 'x' (bt601-ntsc, bt601-pal, bt709, bt2020,
// bt2100-pq, bt2100-hlg)", "colour.range: bad range 'x' (full, limited)".
//
// Known limits of the "@" test (both separators work, so '\' counts as '/' in both paths): a
// path starting with '/' or "<letter>:/" is absolute. So a Windows drive-relative "@X:x.json" is
// joined to the clip folder as written, a POSIX relative "@X:/b.json" is taken as absolute, and a
// POSIX clip path with a literal '\' in a name is split there.
[[nodiscard]] Result<ClipDescription> parseClipJson(std::string_view text,
                                                    std::string_view clip_utf8_path);

// Reads a small whole file (blocking; <= 1 MiB), as the raw provider reads fmt=@<json>, clip=@
// and the sidecar: Errc::Format "'<file>' is too large" above 1 MiB, Errc::Io when the file
// cannot be opened or read. For a dialog's Load JSON, which detects the kind of JSON itself.
[[nodiscard]] Result<std::string> readSmallFile(std::string_view utf8_path);

// readSmallFile(utf8_path), then parseClipJson(text, utf8_path), with their errors.
[[nodiscard]] Result<ClipDescription> loadClipJson(std::string_view utf8_path);

}  // namespace dvv::media
