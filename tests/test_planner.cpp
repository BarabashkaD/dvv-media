#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "converters.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"
#include "planner.h"
#include "store_fakes.h"
#include "store_state.h"

namespace {

using dvv::media::Api;
using dvv::media::Domain;
using dvv::media::Fidelity;
using dvv::media::FormatId;
using dvv::media::FrameDesc;
using dvv::media::LayoutPolicy;
using dvv::media::MemKind;
using dvv::media::ViewSpec;
using dvv::media::detail::Converter;
using dvv::media::detail::Entry;
using dvv::media::detail::EntryState;
using dvv::media::detail::FoundPath;
using dvv::media::detail::PlanSource;
using dvv::media::detail::StateKey;
using dvv::media::test::FakeConverter;

constexpr StateKey P = dvv::media::detail::PRODUCED;
constexpr StateKey X{.unpacked = false, .api = Api::VkBuffer, .kind = MemKind::Host};
constexpr StateKey T{.unpacked = true, .api = Api::HostPlanes, .kind = MemKind::Host};

FakeConverter converter(std::string name, StateKey from, StateKey to, double cost,
                        Fidelity fidelity = Fidelity::Lossless,
                        LayoutPolicy layout = LayoutPolicy::Tight) {
    return FakeConverter(FakeConverter::Config{.name = std::move(name),
                                               .from = from,
                                               .to = to,
                                               .layout = layout,
                                               .fidelity = fidelity,
                                               .cost = cost});
}

FrameDesc grayDesc() {
    const auto gray = dvv::media::findFormat("gray");
    REQUIRE(gray.has_value());
    return FrameDesc{gray.value_or(FormatId{0}), 0, 16, 8};
}

std::optional<FoundPath> find(std::span<const PlanSource> sources,
                              std::span<const Converter* const> converters, StateKey target,
                              LayoutPolicy layout, std::size_t depth, bool allow_derived = false) {
    return dvv::media::detail::findPath(sources, converters, grayDesc(), target, layout, depth,
                                        allow_derived);
}

// The found path (REQUIREs one).
FoundPath found(const std::optional<FoundPath>& path) {
    REQUIRE(path.has_value());
    return path.value_or(FoundPath{});
}

std::vector<const Converter*> stepsOf(const std::optional<FoundPath>& path) {
    return found(path).steps;
}

const PlanSource PRODUCE{.state = P, .layout = LayoutPolicy::Tight, .fidelity = Fidelity::Exact};

std::shared_ptr<Entry> entry(Fidelity fidelity, EntryState state, const ViewSpec& view) {
    auto made = std::make_shared<Entry>();
    made->planned = view;
    made->fidelity = fidelity;
    made->state = state;
    if (state == EntryState::Published) {
        made->views = {view};
    }
    return made;
}

}  // namespace

TEST_SUITE("planner") {
    TEST_CASE("the cheapest path wins") {
        const FakeConverter a = converter("A", P, X, 1);
        const FakeConverter b = converter("B", X, T, 1);
        const FakeConverter c = converter("C", P, T, 5);
        const std::array<const Converter*, 3> converters{&a, &b, &c};
        const std::array<PlanSource, 1> sources{PRODUCE};
        const auto path = find(sources, converters, T, LayoutPolicy::Any, 4);
        CHECK(stepsOf(path) == std::vector<const Converter*>{&a, &b});
        CHECK(found(path).cost == 2);
        CHECK(found(path).source == 0);
        CHECK(found(path).fidelity == Fidelity::Lossless);
    }

    TEST_CASE("the depth limit is exact") {
        const FakeConverter a = converter("A", P, X, 1);
        const FakeConverter b = converter("B", X, T, 1);
        const std::array<const Converter*, 2> converters{&a, &b};
        const std::array<PlanSource, 1> sources{PRODUCE};
        CHECK_FALSE(find(sources, converters, T, LayoutPolicy::Any, 1).has_value());
        CHECK(stepsOf(find(sources, converters, T, LayoutPolicy::Any, 2)) ==
              std::vector<const Converter*>{&a, &b});
    }

    TEST_CASE("an existing entry is a free source") {
        const FakeConverter a = converter("A", P, X, 1);
        const FakeConverter b = converter("B", X, T, 1);
        const std::array<const Converter*, 2> converters{&a, &b};
        const std::array<PlanSource, 2> sources{
            PlanSource{.state = P, .layout = LayoutPolicy::Tight, .cost = 10},
            PlanSource{.state = X, .layout = LayoutPolicy::Tight, .cost = 0}};
        const auto path = find(sources, converters, T, LayoutPolicy::Any, 4);
        CHECK(stepsOf(path) == std::vector<const Converter*>{&b});
        CHECK(found(path).source == 1);
        CHECK(found(path).cost == 1);
    }

    TEST_CASE("Derived only when allowed and nothing else exists") {
        const FakeConverter d = converter("D", P, T, 1, Fidelity::Derived);
        const FakeConverter l = converter("L", P, T, 9, Fidelity::Lossless);
        const std::array<PlanSource, 1> sources{PRODUCE};

        const std::array<const Converter*, 2> both{&d, &l};
        const auto lossless = find(sources, both, T, LayoutPolicy::Any, 4, true);
        CHECK(stepsOf(lossless) == std::vector<const Converter*>{&l});
        CHECK(found(lossless).fidelity == Fidelity::Lossless);

        const std::array<const Converter*, 1> derived_only{&d};
        CHECK_FALSE(find(sources, derived_only, T, LayoutPolicy::Any, 4, false).has_value());
        const auto derived = find(sources, derived_only, T, LayoutPolicy::Any, 4, true);
        CHECK(stepsOf(derived) == std::vector<const Converter*>{&d});
        CHECK(found(derived).fidelity == Fidelity::Derived);
    }

    TEST_CASE("the last step must give the wanted layout") {
        const FakeConverter q1 = converter("Q1", P, T, 1, Fidelity::Lossless, LayoutPolicy::Tight);
        const FakeConverter q2 =
            converter("Q2", P, T, 9, Fidelity::Lossless, LayoutPolicy::VkOptimal);
        const std::array<const Converter*, 2> converters{&q1, &q2};
        const std::array<PlanSource, 1> sources{PRODUCE};
        CHECK(stepsOf(find(sources, converters, T, LayoutPolicy::VkOptimal, 4)) ==
              std::vector<const Converter*>{&q2});
        CHECK(stepsOf(find(sources, converters, T, LayoutPolicy::Any, 4)) ==
              std::vector<const Converter*>{&q1});
        CHECK_FALSE(find(sources, converters, T, LayoutPolicy::RowAlign256, 4).has_value());
    }

    TEST_CASE("a source at the target is a zero-step path") {
        const std::array<PlanSource, 1> sources{
            PlanSource{.state = P, .layout = LayoutPolicy::Tight, .cost = 128}};
        const auto path = find(sources, {}, P, LayoutPolicy::Tight, 4);
        CHECK(found(path).steps.empty());
        CHECK(found(path).cost == 128);
        CHECK(found(path).source == 0);
    }

    TEST_CASE("selectEntry ranks fidelity, then published over pending") {
        const FrameDesc desc = grayDesc();
        const ViewSpec host_view{desc.format, 0, LayoutPolicy::Any, Api::HostPlanes, Domain{}, 0};
        ViewSpec planes_view = host_view;
        planes_view.format = dvv::media::unpackedFormat(desc.format);

        std::vector<std::shared_ptr<Entry>> entries{
            entry(Fidelity::Derived, EntryState::Published, planes_view),
            entry(Fidelity::Lossless, EntryState::Published, planes_view)};
        auto chosen = dvv::media::detail::selectEntry(entries, planes_view, desc.format);
        CHECK(chosen.first == entries[1]);
        CHECK(chosen.second == planes_view);

        entries.push_back(entry(Fidelity::Exact, EntryState::Pending, planes_view));
        chosen = dvv::media::detail::selectEntry(entries, planes_view, desc.format);
        CHECK(chosen.first == entries[2]);

        const std::vector<std::shared_ptr<Entry>> derived{
            entry(Fidelity::Derived, EntryState::Published, host_view)};
        chosen = dvv::media::detail::selectEntry(derived, host_view, desc.format);
        CHECK(chosen.first == nullptr);

        const std::vector<std::shared_ptr<Entry>> failed{
            entry(Fidelity::Exact, EntryState::Failed, planes_view),
            entry(Fidelity::Derived, EntryState::Published, planes_view)};
        chosen = dvv::media::detail::selectEntry(failed, planes_view, desc.format);
        CHECK(chosen.first == failed[1]);
        const std::vector<std::shared_ptr<Entry>> only_failed{
            entry(Fidelity::Exact, EntryState::Failed, planes_view)};
        chosen = dvv::media::detail::selectEntry(only_failed, planes_view, desc.format);
        CHECK(chosen.first == nullptr);
    }
}
