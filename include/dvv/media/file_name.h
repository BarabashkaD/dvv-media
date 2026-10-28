#pragma once

// Frame size, format, fps and colour standard from a raw file's name (M2 design §4.5, D5R design
// §3.4): <name>_WxH[_fmt][_fps][_colour]. Public so that a GUI can say what the name gave.

#include <cstdint>
#include <optional>
#include <string_view>

#include "dvv/media/colour_preset.h"
#include "dvv/media/types.h"

namespace dvv::media {

struct FileNameInfo {
    std::uint32_t width = 0, height = 0;
    std::optional<FormatId> format{};
    std::optional<Rational> fps{};
    std::optional<ColourPreset> colour{};  // the last token is a colour token (colour_preset.h)
};

// Parses the stem of a UTF-8 file name (directories and extension are ignored). The name is split
// at '_' and read from the right: an optional colour token (bt601-ntsc, bt601-pal, bt709, bt2020,
// bt2100-pq, bt2100-hlg; exact, lower case), an optional fps token (\d+(\.\d+)? with an optional
// "fps" suffix), an optional catalogue format name, then a required "WxH" (1..65535 each). Anything
// else gives nullopt, so a colour token alone ("clip_bt709.yuv") gives nothing. No format name or
// fps token equals a colour token. Directories end at the last '/' or '\'. The whole name is tried
// first and then the name without its extension, so "clip_1920x1080_29.97fps" keeps its fps.
[[nodiscard]] std::optional<FileNameInfo> parseFileName(std::string_view utf8_path);

}  // namespace dvv::media
