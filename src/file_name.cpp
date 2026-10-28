#include "dvv/media/file_name.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "dvv/media/colour_preset.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"
#include "sources/uri_params.h"

namespace dvv::media {

namespace {

bool isDigits(std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
}

// \d+(\.\d+)? with an optional "fps" suffix.
std::optional<Rational> fpsToken(std::string_view token) {
    if (token.ends_with("fps")) {
        token.remove_suffix(3);
    }
    const std::size_t dot = token.find('.');
    const bool shaped = dot == std::string_view::npos
                            ? isDigits(token)
                            : isDigits(token.substr(0, dot)) && isDigits(token.substr(dot + 1));
    if (!shaped) {
        return std::nullopt;
    }
    const Result<Rational> fps = detail::parseFps(token);
    if (!fps) {
        return std::nullopt;
    }
    return *fps;
}

// A catalogue format only: unpacked and custom formats depend on the process's history.
std::optional<FormatId> formatToken(std::string_view token) {
    const std::optional<FormatId> format = findFormat(token);
    if (!format || std::ranges::find(catalogue(), *format) == catalogue().end()) {
        return std::nullopt;
    }
    return format;
}

// \d+x\d+, both 1..65535.
std::optional<std::pair<std::uint32_t, std::uint32_t>> sizeToken(std::string_view token) {
    const std::size_t x = token.find('x');
    if (x == std::string_view::npos || !isDigits(token.substr(0, x)) ||
        !isDigits(token.substr(x + 1))) {
        return std::nullopt;
    }
    const auto size = detail::parseSize(token);
    if (!size) {
        return std::nullopt;
    }
    return *size;
}

std::optional<FileNameInfo> parseName(std::string_view name) {
    std::vector<std::string_view> tokens;
    for (std::size_t start = 0;;) {
        const std::size_t end = name.find('_', start);
        tokens.push_back(name.substr(start, end - start));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }

    FileNameInfo info;
    std::size_t count = tokens.size();
    if (const auto colour = findColourPreset(tokens[count - 1])) {
        info.colour = colour;
        --count;
    }
    if (count == 0) {
        return std::nullopt;
    }
    if (const auto fps = fpsToken(tokens[count - 1])) {
        info.fps = fps;
        --count;
    }
    if (count > 0) {
        if (const auto format = formatToken(tokens[count - 1])) {
            info.format = format;
            --count;
        }
    }
    if (count == 0) {
        return std::nullopt;
    }
    const auto size = sizeToken(tokens[count - 1]);
    if (!size) {
        return std::nullopt;
    }
    info.width = size->first;
    info.height = size->second;
    return info;
}

}  // namespace

std::optional<FileNameInfo> parseFileName(std::string_view utf8_path) {
    const std::size_t separator = utf8_path.find_last_of("/\\");
    const std::string_view name =
        separator == std::string_view::npos ? utf8_path : utf8_path.substr(separator + 1);
    if (auto info = parseName(name)) {
        return info;
    }
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
        return std::nullopt;
    }
    return parseName(name.substr(0, dot));
}

}  // namespace dvv::media
