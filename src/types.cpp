#include "dvv/media/types.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
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

}  // namespace dvv::media
