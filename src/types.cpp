#include "dvv/media/types.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace dvv::media {

std::size_t FrameKeyHash::operator()(const FrameKey& key) const noexcept {
    // SplitMix64 finaliser over both fields: equal keys hash equally, nearby keys spread.
    std::uint64_t z = (key.origin * 0x9E3779B97F4A7C15ULL) ^ static_cast<std::uint64_t>(key.index);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return static_cast<std::size_t>(z ^ (z >> 31U));
}

namespace {

// The process-wide colour table (spec §6: process infrastructure, never destroyed). Entries are
// never moved or removed, so references returned by colourParams() stay valid. Lookups compare the
// whole ColourParams (kr and kb by value) in a linear scan: a few dozen entries in practice (custom
// matrices add entries).
struct ColourTable {
    std::shared_mutex mutex;
    std::vector<std::unique_ptr<ColourParams>> entries;

    ColourTable() { entries.push_back(std::make_unique<ColourParams>()); }

    // The id of params, or entries.size() if it is not in the table. Needs the lock.
    [[nodiscard]] std::size_t find(const ColourParams& params) const noexcept {
        std::size_t id = 0;
        while (id < entries.size() && *entries[id] != params) {
            ++id;
        }
        return id;
    }
};

ColourTable& colourTable() {
    // Intentionally leaked; out of memory at start-up is fatal anyway.
    static auto* table = new ColourTable();  // NOLINT(bugprone-unhandled-exception-at-new)
    return *table;
}

// The length of the well-formed UTF-8 sequence starting at text[i] (Unicode Table 3-7), or 0 if
// the byte there does not start one.
std::size_t wellFormedLength(std::string_view text, std::size_t i) noexcept {
    const auto byte = [&text](std::size_t at) { return static_cast<std::uint8_t>(text[at]); };
    const std::uint8_t lead = byte(i);
    if (lead <= 0x7FU) {
        return 1;
    }
    std::size_t length = 0;
    // The allowed range of the second byte; every later byte is 80..BF.
    std::uint8_t low = 0x80U;
    std::uint8_t high = 0xBFU;
    if (lead >= 0xC2U && lead <= 0xDFU) {
        length = 2;
    } else if (lead == 0xE0U) {
        length = 3;
        low = 0xA0U;
    } else if ((lead >= 0xE1U && lead <= 0xECU) || lead == 0xEEU || lead == 0xEFU) {
        length = 3;
    } else if (lead == 0xEDU) {
        length = 3;
        high = 0x9FU;  // no surrogates
    } else if (lead == 0xF0U) {
        length = 4;
        low = 0x90U;
    } else if (lead >= 0xF1U && lead <= 0xF3U) {
        length = 4;
    } else if (lead == 0xF4U) {
        length = 4;
        high = 0x8FU;  // nothing above U+10FFFF
    } else {
        return 0;  // 80..C1, F5..FF
    }
    if (text.size() - i < length) {
        return 0;  // truncated
    }
    if (byte(i + 1) < low || byte(i + 1) > high) {
        return 0;
    }
    for (std::size_t k = 2; k < length; ++k) {
        if (byte(i + k) < 0x80U || byte(i + k) > 0xBFU) {
            return 0;
        }
    }
    return length;
}

}  // namespace

Result<void> validateColour(const ColourParams& params) {
    if (params.matrix == ColourMatrix::Custom) {
        const bool valid = std::isfinite(params.kr) && std::isfinite(params.kb) && params.kr > 0 &&
                           params.kb > 0 && params.kr + params.kb < 1;
        if (!valid) {
            return makeError(
                Errc::Format,
                std::format("custom matrix needs kr > 0, kb > 0 and kr + kb < 1 (kr={}, kb={})",
                            params.kr, params.kb));
        }
        return {};
    }
    if (params.kr != 0 || params.kb != 0) {
        return makeError(Errc::Format, "kr and kb apply only to the custom matrix");
    }
    return {};
}

ColourId internColour(const ColourParams& params) {
    ColourTable& table = colourTable();
    {
        const std::shared_lock lock(table.mutex);
        const std::size_t id = table.find(params);
        if (id < table.entries.size()) {
            return static_cast<ColourId>(id);
        }
    }
    const std::unique_lock lock(table.mutex);
    const std::size_t id = table.find(params);
    if (id == table.entries.size()) {
        table.entries.push_back(std::make_unique<ColourParams>(params));
    }
    return static_cast<ColourId>(id);
}

const ColourParams& colourParams(ColourId id) noexcept {
    ColourTable& table = colourTable();
    const std::shared_lock lock(table.mutex);
    return id < table.entries.size() ? *table.entries[id] : *table.entries[0];
}

std::filesystem::path pathFromUtf8(std::string_view utf8) {
    static constexpr std::u8string_view REPLACEMENT = u8"\uFFFD";
    std::u8string text;
    text.reserve(utf8.size());
    std::size_t i = 0;
    while (i < utf8.size()) {
        const std::size_t length = wellFormedLength(utf8, i);
        if (length == 0) {
            text.append(REPLACEMENT);
            ++i;
            continue;
        }
        for (std::size_t k = 0; k < length; ++k) {
            text.push_back(static_cast<char8_t>(utf8[i + k]));
        }
        i += length;
    }
    return std::filesystem::path(text);
}

std::string utf8FromPath(const std::filesystem::path& path) {
    try {
        const std::u8string text = path.u8string();
        return std::string(reinterpret_cast<const char*>(text.data()), text.size());
    } catch (const std::system_error&) {
        return "(path not representable as UTF-8)";
    }
}

}  // namespace dvv::media
