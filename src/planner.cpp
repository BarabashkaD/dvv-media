#include "planner.h"

#include <algorithm>
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

namespace {

[[nodiscard]] int rank(Fidelity fidelity) noexcept {
    return fidelity == Fidelity::Exact ? 2 : fidelity == Fidelity::Lossless ? 1 : 0;
}

[[nodiscard]] bool layoutMatches(LayoutPolicy given, LayoutPolicy wanted) noexcept {
    return wanted == LayoutPolicy::Any || given == wanted;
}

// The cheapest path with exactly `depth` steps into one state.
struct Node {
    bool valid = false;
    double cost = 0;
    Fidelity fidelity = Fidelity::Exact;
    std::size_t source = 0;
    std::size_t previous = 0;   // its state at depth - 1
    std::size_t converter = 0;  // the step into it
};

// Same depth: the lower cost, then the lower source index. Equal candidates keep the first, so the
// earlier converter wins.
[[nodiscard]] bool better(double cost, std::size_t source, const Node& current) noexcept {
    if (!current.valid) {
        return true;
    }
    if (cost != current.cost) {
        return cost < current.cost;
    }
    return source < current.source;
}

struct Ending {
    bool found = false;
    double cost = 0;
    Fidelity fidelity = Fidelity::Exact;
    std::size_t source = 0;
    std::size_t depth = 0;
    std::size_t previous = 0;   // the last step's input state (depth > 0)
    std::size_t converter = 0;  // the last step (depth > 0)
};

// One pass of findPath: bounded relaxation over (depth, state). `derived`: Derived sources and
// converters take part.
[[nodiscard]] std::optional<FoundPath> search(std::span<const PlanSource> sources,
                                              std::span<const Converter* const> converters,
                                              const FrameDesc& desc, StateKey target,
                                              LayoutPolicy layout, std::size_t max_depth,
                                              bool derived) {
    std::vector<StateKey> states;
    const auto index_of = [&states](StateKey state) {
        const auto found = std::ranges::find(states, state);
        if (found != states.end()) {
            return static_cast<std::size_t>(found - states.begin());
        }
        states.push_back(state);
        return states.size() - 1;
    };
    const auto usable = [derived](Fidelity fidelity) {
        return derived || fidelity != Fidelity::Derived;
    };
    const std::size_t goal = index_of(target);
    std::vector<std::size_t> source_state(sources.size());
    for (std::size_t i = 0; i < sources.size(); ++i) {
        source_state[i] = index_of(sources[i].state);
    }
    std::vector<std::pair<std::size_t, std::size_t>> edges(converters.size());  // from, to
    for (std::size_t k = 0; k < converters.size(); ++k) {
        edges[k] = {index_of(converters[k]->from()), index_of(converters[k]->to())};
    }

    Ending best;
    // Zero steps: a source at the target with the wanted layout.
    std::vector<Node> layer(states.size());
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const PlanSource& source = sources[i];
        if (!usable(source.fidelity)) {
            continue;
        }
        Node& node = layer[source_state[i]];
        if (better(source.cost, i, node)) {
            node =
                Node{.valid = true, .cost = source.cost, .fidelity = source.fidelity, .source = i};
        }
        if (source_state[i] == goal && layoutMatches(source.layout, layout) &&
            (!best.found || source.cost < best.cost)) {
            best = Ending{
                .found = true, .cost = source.cost, .fidelity = source.fidelity, .source = i};
        }
    }

    // A path with more steps than states repeats a state; with costs >= 0 the path without the
    // cycle is as cheap and shorter, so deeper layers cannot win.
    const std::size_t depth_limit = std::min(max_depth, states.size());
    std::vector<std::vector<Node>> layers;
    layers.reserve(depth_limit + 1);
    layers.push_back(std::move(layer));
    for (std::size_t depth = 1; depth <= depth_limit; ++depth) {
        std::vector<Node> next(states.size());
        const std::vector<Node>& previous = layers.back();
        for (std::size_t k = 0; k < converters.size(); ++k) {
            const Converter& converter = *converters[k];
            if (!usable(converter.fidelity())) {
                continue;
            }
            const auto [from, to] = edges[k];
            const Node& input = previous[from];
            if (!input.valid) {
                continue;
            }
            const double cost = input.cost + converter.cost(desc);
            const Fidelity fidelity = weaker(input.fidelity, converter.fidelity());
            if (better(cost, input.source, next[to])) {
                next[to] = Node{.valid = true,
                                .cost = cost,
                                .fidelity = fidelity,
                                .source = input.source,
                                .previous = from,
                                .converter = k};
            }
            // Earlier depths win ties; at the same depth the lower source, then the first
            // converter.
            const bool cheaper =
                !best.found || cost < best.cost ||
                (cost == best.cost && depth == best.depth && input.source < best.source);
            if (to == goal && layoutMatches(converter.outputLayout(), layout) && cheaper) {
                best = Ending{.found = true,
                              .cost = cost,
                              .fidelity = fidelity,
                              .source = input.source,
                              .depth = depth,
                              .previous = from,
                              .converter = k};
            }
        }
        layers.push_back(std::move(next));
    }
    if (!best.found) {
        return std::nullopt;
    }
    FoundPath path{.source = best.source, .fidelity = best.fidelity, .cost = best.cost};
    path.steps.resize(best.depth);
    if (best.depth > 0) {
        path.steps[best.depth - 1] = converters[best.converter];
        std::size_t state = best.previous;
        for (std::size_t depth = best.depth - 1; depth > 0; --depth) {
            const Node& node = layers[depth][state];
            path.steps[depth - 1] = converters[node.converter];
            state = node.previous;
        }
    }
    return path;
}

}  // namespace

bool satisfies(const ViewSpec& view, const ViewSpec& want) noexcept {
    return view.format == want.format && view.colour == want.colour && view.api == want.api &&
           view.domain == want.domain && view.downscaleLog2 == want.downscaleLog2 &&
           (want.layout == LayoutPolicy::Any || view.layout == want.layout);
}

Fidelity weaker(Fidelity a, Fidelity b) noexcept {
    return rank(a) <= rank(b) ? a : b;
}

std::optional<FoundPath> findPath(std::span<const PlanSource> sources,
                                  std::span<const Converter* const> converters,
                                  const FrameDesc& desc, StateKey target, LayoutPolicy layout,
                                  std::size_t max_depth, bool allow_derived) {
    std::optional<FoundPath> path =
        search(sources, converters, desc, target, layout, max_depth, false);
    if (!path && allow_derived) {
        path = search(sources, converters, desc, target, layout, max_depth, true);
    }
    return path;
}

std::pair<std::shared_ptr<Entry>, ViewSpec> selectEntry(
    std::span<const std::shared_ptr<Entry>> entries, const ViewSpec& want, FormatId original) {
    std::pair<std::shared_ptr<Entry>, ViewSpec> best{};
    int best_score = -1;
    for (const auto& entry : entries) {
        if (entry->state == EntryState::Failed ||
            (entry->fidelity == Fidelity::Derived && want.format == original)) {
            continue;
        }
        const bool published = entry->state == EntryState::Published;
        const std::span<const ViewSpec> views =
            published ? std::span<const ViewSpec>(entry->views) : std::span(&entry->planned, 1);
        for (const ViewSpec& view : views) {
            if (!satisfies(view, want)) {
                continue;
            }
            const int score = rank(entry->fidelity) * 2 + (published ? 1 : 0);
            if (score > best_score) {
                best_score = score;
                best = {entry, view};
            }
            break;
        }
    }
    return best;
}

}  // namespace dvv::media::detail
