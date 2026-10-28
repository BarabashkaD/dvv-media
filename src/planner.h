#pragma once
// The frame store's planner (M2 design §5.2, §5.3): the cheapest converter path over the store's
// static graph, and the choice of an existing entry. Private to dvv-media.

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "converters.h"
#include "dvv/media/types.h"
#include "store_state.h"

namespace dvv::media::detail {

// One way into the graph: an existing entry's view (cost 0) or Produce (the frame's bytes).
struct PlanSource {
    StateKey state;
    LayoutPolicy layout = LayoutPolicy::Tight;
    Fidelity fidelity = Fidelity::Exact;
    double cost = 0;
};
struct FoundPath {
    std::size_t source = 0;                 // index into the sources given to findPath
    std::vector<const Converter*> steps{};  // in execution order; may be empty
    Fidelity fidelity = Fidelity::Exact;    // the worst of the source's and the steps'
    double cost = 0;                        // the source's cost plus the steps' costs
};

// The cheapest path from any source to `target` with at most `max_depth` converters. Its last
// converter (or a zero-step source) must give `layout` unless that is Any. Pass 1 ignores Derived
// sources and Derived converters; only when pass 1 finds nothing and `allow_derived` does pass 2
// use them. Ties: fewer steps, then the lower source index, then the converters' order.
[[nodiscard]] std::optional<FoundPath> findPath(std::span<const PlanSource> sources,
                                                std::span<const Converter* const> converters,
                                                const FrameDesc& desc, StateKey target,
                                                LayoutPolicy layout, std::size_t max_depth,
                                                bool allow_derived);

// The best entry for `want` (§5.2): Exact > Lossless > Derived, then published before pending,
// then insertion order. Derived never serves an original-format request; failed entries are never
// chosen. Caller holds the store mutex. {nullptr, {}} when no entry satisfies `want`.
[[nodiscard]] std::pair<std::shared_ptr<Entry>, ViewSpec> selectEntry(
    std::span<const std::shared_ptr<Entry>> entries, const ViewSpec& want, FormatId original);

// A view satisfies a request when everything but the layout is equal and the layout matches
// (Any matches every layout).
[[nodiscard]] bool satisfies(const ViewSpec& view, const ViewSpec& want) noexcept;
// The less faithful of the two (Exact > Lossless > Derived).
[[nodiscard]] Fidelity weaker(Fidelity a, Fidelity b) noexcept;

}  // namespace dvv::media::detail
