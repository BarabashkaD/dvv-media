#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "converters.h"
#include "dvv/base/error.h"
#include "dvv/base/executors.h"
#include "dvv/base/lifetime.h"
#include "dvv/base/task.h"
#include "dvv/base/testing/test_authority.h"
#include "dvv/base/when_all.h"
#include "dvv/media/frame.h"
#include "dvv/media/frame_store.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/source.h"
#include "dvv/media/types.h"
#include "dvv/platform/file_reader.h"
#include "store_fakes.h"
#include "store_state.h"

namespace {

using dvv::Errc;
using dvv::IExecutor;
using dvv::Result;
using dvv::syncWait;
using dvv::Task;
using dvv::whenAll;
using dvv::media::Api;
using dvv::media::Domain;
using dvv::media::Fidelity;
using dvv::media::FormatId;
using dvv::media::FrameDesc;
using dvv::media::FrameKey;
using dvv::media::FrameStore;
using dvv::media::HostPlanesB;
using dvv::media::LayoutPolicy;
using dvv::media::LeaseKind;
using dvv::media::MemKind;
using dvv::media::OriginId;
using dvv::media::PinLease;
using dvv::media::ReadLease;
using dvv::media::ViewSpec;
using dvv::media::detail::Converter;
using dvv::media::detail::StateKey;
using dvv::media::detail::StoreAccess;
using dvv::media::detail::StoreState;
using dvv::media::test::FakeConverter;
using dvv::media::test::FakeFrame;
using dvv::media::test::FakeSource;
using dvv::media::test::Gate;
using dvv::media::test::pattern;
using dvv::media::test::StoreTest;
using dvv::media::test::waitFor;
using dvv::media::test::waitUntil;
using dvv::test::TestAuthority;

// The host planes of a lease (REQUIREs that the binding holds them).
template <class Lease>
const HostPlanesB& planesOf(const Lease& lease) {
    const auto* planes = std::get_if<HostPlanesB>(&lease.binding());
    REQUIRE(planes != nullptr);
    return *planes;
}

bool holdsPattern(std::span<const std::byte> bytes, std::int64_t index) {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] != pattern(index, i)) {
            return false;
        }
    }
    return true;
}

ReadLease take(FrameStore& store, FrameKey key, const ViewSpec& view, IExecutor& resume_on) {
    Result<ReadLease> lease = syncWait(store.acquire(key, view, resume_on));
    REQUIRE(lease.has_value());
    return std::move(*lease);
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::thread::id threadOf(IExecutor& executor) {
    std::promise<std::thread::id> promise;
    std::future<std::thread::id> future = promise.get_future();
    executor.post([promise = std::move(promise)]() mutable {
        promise.set_value(std::this_thread::get_id());
    });
    return future.get();
}

// Acquires and records the thread the requester resumed on.
Task<Result<ReadLease>> acquireAndRecord(std::shared_ptr<FrameStore> store, FrameKey key,
                                         ViewSpec view, IExecutor* resume_on,
                                         std::thread::id* resumed) {
    Result<ReadLease> lease = co_await store->acquire(key, view, *resume_on);
    *resumed = std::this_thread::get_id();
    co_return lease;
}

std::unique_ptr<FakeConverter> makeUnpack() {
    return std::make_unique<FakeConverter>(FakeConverter::Config{
        .name = "Unpack",
        .from = dvv::media::detail::PRODUCED,
        .to = {.unpacked = true, .api = Api::HostPlanes, .kind = dvv::media::MemKind::Host},
        .fidelity = dvv::media::Fidelity::Lossless});
}

constexpr StateKey P = dvv::media::detail::PRODUCED;
constexpr StateKey X{.unpacked = false, .api = Api::VkBuffer, .kind = MemKind::Host};
constexpr StateKey U{.unpacked = true, .api = Api::VkBuffer, .kind = MemKind::Host};

std::unique_ptr<FakeConverter> makeConverter(std::string name, StateKey from, StateKey to,
                                             Fidelity fidelity, double cost,
                                             LayoutPolicy layout = LayoutPolicy::Tight) {
    return std::make_unique<FakeConverter>(FakeConverter::Config{.name = std::move(name),
                                                                 .from = from,
                                                                 .to = to,
                                                                 .layout = layout,
                                                                 .fidelity = fidelity,
                                                                 .cost = cost});
}

// The converters as a list for makeStore, in this order.
template <class... Fakes>
std::vector<std::unique_ptr<Converter>> listOf(std::unique_ptr<Fakes>... fakes) {
    std::vector<std::unique_ptr<Converter>> converters;
    (converters.push_back(std::move(fakes)), ...);
    return converters;
}

void requireInvariants(const FrameStore& store) {
    const Result<void> invariants = StoreAccess::checkInvariants(store);
    REQUIRE_MESSAGE(invariants.has_value(),
                    (invariants ? std::string() : invariants.error().message));
}

// The readers of a key's original, read under the store's mutex; nullopt without one. An opener
// reads them while its gate holds the original pending: every reader counted then was reserved
// when its requester or plan chose the pending entry (prepareLocked, planLocked).
std::optional<std::uint32_t> rootReaders(FrameStore& store, const FrameKey& key) {
    const StoreState& s = StoreAccess::state(store);
    const std::scoped_lock lock(s.mutex);
    const auto record = s.records.find(key);
    if (record == s.records.end()) {
        return std::nullopt;
    }
    for (const auto& entry : record->second.entries) {
        if (entry->root) {
            return entry->readers;
        }
    }
    return std::nullopt;
}

// A view of the frame's own format and colour.
ViewSpec viewOf(FormatId format, Api api, Domain domain) {
    return ViewSpec{format, 0, LayoutPolicy::Any, api, domain, 0};
}

}  // namespace

TEST_SUITE("store") {
    TEST_CASE_FIXTURE(StoreTest, "acquire produces the original once and then hits") {
        const auto store = makeStore();
        const auto source = makeSource(4);
        const OriginId origin = store->attach(source).value();
        const Result<ReadLease> first = syncWait(store->acquire({origin, 1}, hostView, *requester));
        REQUIRE(first.has_value());
        CHECK(first->fidelity() == dvv::media::Fidelity::Exact);
        const HostPlanesB& planes = planesOf(*first);
        CHECK(planes.planeCount == 1);
        CHECK(planes.strides[0] == 16);
        REQUIRE(planes.planes[0].size() == 128);
        CHECK(holdsPattern(planes.planes[0], 1));

        const Result<ReadLease> second =
            syncWait(store->acquire({origin, 1}, hostView, *requester));
        REQUIRE(second.has_value());
        CHECK(planesOf(*second).planes[0].data() == planes.planes[0].data());
        CHECK(source->produced.load() == 1);
        const auto stats = store->stats();
        CHECK(stats.hits == 1);
        CHECK(stats.misses == 1);
        CHECK(stats.creations == 1);
        CHECK(stats.frames == 1);
        CHECK(stats.leases == 2);
    }

    TEST_CASE_FIXTURE(StoreTest, "32 concurrent acquires of one view produce once") {
        const auto store = makeStore();
        const auto source = makeSource();
        Gate gate;
        source->gate = &gate;
        const OriginId origin = store->attach(source).value();
        const FrameKey key{origin, 0};
        bool reached = false;
        std::optional<std::uint32_t> reserved;
        std::jthread opener([&] {
            reached = waitFor(
                [&] { return gate.waiting() == 1 && StoreAccess::waiterCount(*store, key) == 31; });
            reserved = rootReaders(*store, key);
            gate.open();
        });
        std::vector<Task<Result<ReadLease>>> tasks;
        tasks.reserve(32);
        for (int i = 0; i < 32; ++i) {
            tasks.push_back(store->acquire(key, hostView, *requester));
        }
        std::vector<Result<ReadLease>> results = syncWait(whenAll(std::move(tasks)));
        opener.join();
        CHECK(reached);
        // Each of the 31 joiners reserved its reader of the pending original when it chose to join,
        // so the original cannot be evicted between its publication and the joiner's lease.
        CHECK(reserved.value_or(0) == 31);
        REQUIRE(results.size() == 32);
        REQUIRE(results[0].has_value());
        const std::byte* const data = planesOf(*results[0]).planes[0].data();
        for (const Result<ReadLease>& result : results) {
            REQUIRE(result.has_value());
            CHECK(planesOf(*result).planes[0].data() == data);
        }
        CHECK(source->produced.load() == 1);
        const auto stats = store->stats();
        CHECK(stats.misses == 1);
        CHECK(stats.hits == 31);
        CHECK(stats.creations == 1);
        CHECK(stats.failures == 0);

        // Every reader reserved for a joiner is released with its lease: the frame is evictable.
        results.clear();
        store->trim(store->hostDomain(), 0);
        CHECK(store->stats().tiers[0].bytes == 0);
        CHECK(store->stats().frames == 0);
    }

    TEST_CASE_FIXTURE(StoreTest, "a failed produce is not cached") {
        const auto store = makeStore();
        const auto source = makeSource();
        source->failNext = 1;
        const OriginId origin = store->attach(source).value();
        const Result<ReadLease> failed =
            syncWait(store->acquire({origin, 0}, hostView, *requester));
        REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error().code == Errc::Io);
        CHECK(store->stats().failures == 1);
        CHECK(store->stats().frames == 0);
        const Result<ReadLease> second =
            syncWait(store->acquire({origin, 0}, hostView, *requester));
        CHECK(second.has_value());
        CHECK(source->attempts.load() == 2);
        CHECK(source->produced.load() == 1);
    }

    TEST_CASE_FIXTURE(StoreTest, "joiners of a failed creation get its error") {
        const auto store = makeStore();
        const auto source = makeSource();
        Gate gate;
        source->gate = &gate;
        source->failNext = 1;
        const OriginId origin = store->attach(source).value();
        const FrameKey key{origin, 0};
        bool reached = false;
        std::jthread opener([&] {
            reached = waitFor(
                [&] { return gate.waiting() == 1 && StoreAccess::waiterCount(*store, key) == 3; });
            gate.open();
        });
        std::vector<Task<Result<ReadLease>>> tasks;
        tasks.reserve(4);
        for (int i = 0; i < 4; ++i) {
            tasks.push_back(store->acquire(key, hostView, *requester));
        }
        const std::vector<Result<ReadLease>> results = syncWait(whenAll(std::move(tasks)));
        opener.join();
        CHECK(reached);
        for (const Result<ReadLease>& result : results) {
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().code == Errc::Io);
            CHECK(result.error().message == "fake read error");
        }
        CHECK(store->stats().failures == 1);
        const Result<ReadLease> fifth = syncWait(store->acquire(key, hostView, *requester));
        CHECK(fifth.has_value());
    }

    TEST_CASE_FIXTURE(StoreTest, "an exception in produce is Internal") {
        const auto store = makeStore();
        const auto source = makeSource();
        source->throwNext = true;
        const OriginId origin = store->attach(source).value();
        const Result<ReadLease> result =
            syncWait(store->acquire({origin, 0}, hostView, *requester));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Errc::Internal);
        CHECK(contains(result.error().message, "threw: boom"));
    }

    TEST_CASE_FIXTURE(StoreTest, "every requester resumes on its own executor") {
        auto made_a = dvv::ExecutorFactory::makeThreadPool(1);
        auto made_b = dvv::ExecutorFactory::makeThreadPool(1);
        REQUIRE(made_a.has_value());
        REQUIRE(made_b.has_value());
        const std::shared_ptr<dvv::ThreadPool> a = std::move(*made_a);
        const std::shared_ptr<dvv::ThreadPool> b = std::move(*made_b);
        const std::thread::id a_id = threadOf(*a);
        const std::thread::id b_id = threadOf(*b);
        REQUIRE(a_id != b_id);

        const auto store = makeStore();
        const auto source = makeSource();
        Gate gate;
        source->gate = &gate;
        const OriginId origin = store->attach(source).value();
        const FrameKey key{origin, 0};
        bool reached = false;
        std::jthread opener([&] {
            reached = waitFor(
                [&] { return gate.waiting() == 1 && StoreAccess::waiterCount(*store, key) == 1; });
            gate.open();
        });
        std::thread::id x_id;
        std::thread::id y_id;
        auto [x, y] = syncWait(whenAll(acquireAndRecord(store, key, hostView, a.get(), &x_id),
                                       acquireAndRecord(store, key, hostView, b.get(), &y_id)));
        opener.join();
        CHECK(reached);
        REQUIRE(x.has_value());
        REQUIRE(y.has_value());
        CHECK(x_id == a_id);
        CHECK(y_id == b_id);

        dvv::media::test::CountingExecutor counting(*b);
        const Result<ReadLease> hit = syncWait(store->acquire(key, hostView, counting));
        REQUIRE(hit.has_value());
        CHECK(counting.posts() == 1);
    }

    TEST_CASE_FIXTURE(StoreTest, "a released lease touches the LRU") {
        const auto store = makeStore();
        const auto source = makeSource();
        const OriginId origin = store->attach(source).value();
        for (std::int64_t index = 0; index < 3; ++index) {
            static_cast<void>(take(*store, {origin, index}, hostView, *requester));
        }
        // LRU order, least recently used first: 0, 1, 2.
        std::optional<ReadLease> held = take(*store, {origin, 0}, hostView, *requester);  // 1 2 0
        static_cast<void>(take(*store, {origin, 1}, hostView, *requester));               // 2 0 1
        held.reset();                           // only the release touch moves 0 behind 1: 2 1 0
        store->trim(store->hostDomain(), 128);  // evicts 2 and 1
        const int produced = source->produced.load();
        const std::uint64_t hits = store->stats().hits;
        static_cast<void>(take(*store, {origin, 0}, hostView, *requester));
        CHECK(source->produced.load() == produced);
        CHECK(store->stats().hits == hits + 1);
        static_cast<void>(take(*store, {origin, 1}, hostView, *requester));
        CHECK(source->produced.load() == produced + 1);
        const Result<void> invariants = StoreAccess::checkInvariants(*store);
        CHECK_MESSAGE(invariants.has_value(),
                      (invariants ? std::string() : invariants.error().message));
    }

    TEST_CASE_FIXTURE(StoreTest, "leased and pinned entries are not trimmed") {
        const auto store = makeStore();
        const auto source = makeSource();
        const OriginId origin = store->attach(source).value();
        std::optional<ReadLease> lease = take(*store, {origin, 0}, hostView, *requester);
        Result<PinLease> pin = syncWait(store->pin({origin, 1}, hostView, *requester));
        REQUIRE(pin.has_value());
        store->trim(store->hostDomain(), 0);
        auto stats = store->stats();
        REQUIRE_FALSE(stats.tiers.empty());
        CHECK(stats.tiers[0].bytes == 256);
        CHECK(stats.frames == 2);
        CHECK(stats.pins == 1);
        CHECK(stats.leases == 1);

        lease.reset();
        *pin = PinLease{};
        store->trim(store->hostDomain(), 0);
        stats = store->stats();
        CHECK(stats.tiers[0].bytes == 0);
        CHECK(stats.frames == 0);
        CHECK(stats.evictions == 2);
    }

    TEST_CASE_FIXTURE(StoreTest, "outstandingLeases names the acquiring location") {
        const auto store = makeStore();
        const auto source = makeSource();
        const OriginId origin = store->attach(source).value();
        const FrameKey key{origin, 0};
        const auto here = std::source_location::current();
        auto lease = syncWait(store->acquire(key, hostView, *requester, here)).value();
        auto leases = store->outstandingLeases();
        REQUIRE(leases.size() == 1);
        CHECK(leases[0].where.line() == here.line());
        CHECK(leases[0].kind == LeaseKind::Read);
        CHECK(leases[0].key == key);
        CHECK(leases[0].view == lease.view());

        auto pin = syncWait(store->pin(key, hostView, *requester)).value();
        leases = store->outstandingLeases();
        REQUIRE(leases.size() == 2);
        const bool has_pin = leases[0].kind == LeaseKind::Pin || leases[1].kind == LeaseKind::Pin;
        CHECK(has_pin);

        lease = ReadLease{};
        pin = PinLease{};
        CHECK(store->outstandingLeases().empty());
    }

    TEST_CASE_FIXTURE(StoreTest, "adopt inserts an original; a second adopt is Internal") {
        const auto store = makeStore();
        const ViewSpec tight{desc.format, 0, LayoutPolicy::Tight, Api::HostPlanes, {}, 0};
        const std::vector<std::byte> payload(128, std::byte{7});
        const FrameKey adopted_key{77, 0};
        const Result<ReadLease> adopted = store->adopt(
            adopted_key,
            TestAuthority::makeShared<FakeFrame>(desc, std::vector<ViewSpec>{tight}, payload));
        REQUIRE(adopted.has_value());
        CHECK(adopted->fidelity() == dvv::media::Fidelity::Exact);

        const Result<ReadLease> hit = syncWait(store->acquire(adopted_key, hostView, *requester));
        REQUIRE(hit.has_value());
        CHECK(store->stats().hits == 1);
        CHECK(store->stats().misses == 0);

        const Result<ReadLease> again = store->adopt(
            adopted_key,
            TestAuthority::makeShared<FakeFrame>(desc, std::vector<ViewSpec>{tight}, payload));
        REQUIRE_FALSE(again.has_value());
        CHECK(again.error().code == Errc::Internal);
        CHECK(contains(again.error().message, "already has an original"));

        ViewSpec other = tight;
        other.format = planesView.format;
        const Result<ReadLease> wrong = store->adopt(
            {78, 0},
            TestAuthority::makeShared<FakeFrame>(desc, std::vector<ViewSpec>{other}, payload));
        REQUIRE_FALSE(wrong.has_value());
        CHECK(wrong.error().code == Errc::Internal);

        const auto source = makeSource();
        Gate gate;
        source->gate = &gate;
        const OriginId origin = store->attach(source).value();
        Result<ReadLease> pending = dvv::makeError(Errc::Internal, "not run");
        std::jthread acquirer(
            [&] { pending = syncWait(store->acquire({origin, 2}, hostView, *requester)); });
        waitUntil([&] { return gate.waiting() == 1; });
        const Result<ReadLease> refused = store->adopt(
            {origin, 2},
            TestAuthority::makeShared<FakeFrame>(desc, std::vector<ViewSpec>{tight}, payload));
        gate.open();
        acquirer.join();
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == Errc::Internal);
        CHECK(contains(refused.error().message, "already has an original"));
        CHECK(pending.has_value());
    }

    TEST_CASE_FIXTURE(StoreTest, "detach drops unleased entries and refuses later acquires") {
        const auto store = makeStore();
        auto first = makeSource();
        const auto second = makeSource();
        const OriginId one = store->attach(first).value();
        const OriginId two = store->attach(second).value();
        REQUIRE(one == 1);
        REQUIRE(two == 2);
        const ReadLease kept = take(*store, {one, 0}, hostView, *requester);
        static_cast<void>(take(*store, {one, 1}, hostView, *requester));

        store->detach(one);
        REQUIRE(planesOf(kept).planes[0].size() == 128);
        CHECK(holdsPattern(planesOf(kept).planes[0], 0));
        CHECK(store->stats().frames == 0);
        const Result<ReadLease> refused = syncWait(store->acquire({one, 1}, hostView, *requester));
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == Errc::Internal);
        CHECK(contains(refused.error().message, "origin 1 is not attached"));

        const ReadLease other = take(*store, {two, 0}, hostView, *requester);
        CHECK(holdsPattern(planesOf(other).planes[0], 0));
        CHECK(second->produced.load() == 1);

        const std::weak_ptr<FakeSource> weak = first;
        first.reset();
        CHECK(weak.expired());
    }

    TEST_CASE_FIXTURE(StoreTest, "acquire and drop leases while another thread trims") {
        const auto store = makeStore();
        const auto source = makeSource();
        const OriginId origin = store->attach(source).value();
        constexpr int WORKERS = 8;
        std::atomic<int> failures{0};
        std::atomic<int> done{0};
        {
            std::jthread trimmer([&] {
                while (done.load() < WORKERS) {
                    store->trim(store->hostDomain(), 0);
                }
            });
            std::vector<std::jthread> workers;
            workers.reserve(WORKERS);
            for (int worker = 0; worker < WORKERS; ++worker) {
                workers.emplace_back([&] {
                    for (std::int64_t i = 0; i < 200; ++i) {
                        const Result<ReadLease> lease =
                            syncWait(store->acquire({origin, i % 4}, hostView, *requester));
                        if (!lease) {
                            failures.fetch_add(1);
                        }
                    }
                    done.fetch_add(1);
                });
            }
        }
        CHECK(failures.load() == 0);
        const Result<void> invariants = StoreAccess::checkInvariants(*store);
        CHECK_MESSAGE(invariants.has_value(),
                      (invariants ? std::string() : invariants.error().message));
        const auto stats = store->stats();
        CHECK(stats.leases == 0);
        CHECK(stats.pins == 0);

        // No reader or pin leaked: the quiescent store trims to nothing.
        store->trim(store->hostDomain(), 0);
        CHECK(store->stats().tiers[0].bytes == 0);
        CHECK(store->stats().frames == 0);
    }

    TEST_CASE_FIXTURE(StoreTest, "acquire the original and the unpacked view while trimming") {
        // Plans that join a pending original or read a published one, under contention.
        const auto store = makeStore(listOf(makeUnpack()));
        const auto source = makeSource();
        const OriginId origin = store->attach(source).value();
        constexpr int WORKERS = 8;
        std::atomic<int> failures{0};
        std::atomic<int> wrong_bytes{0};
        std::atomic<int> done{0};
        int broken = 0;  // invariant violations seen by the trimmer
        std::string first_broken;
        {
            std::jthread trimmer([&] {
                while (done.load() < WORKERS) {
                    store->trim(store->hostDomain(), 0);
                    // Every rule holds whenever the store mutex is free.
                    const Result<void> invariants = StoreAccess::checkInvariants(*store);
                    if (!invariants && broken++ == 0) {
                        first_broken = invariants.error().message;
                    }
                }
            });
            std::vector<std::jthread> workers;
            workers.reserve(WORKERS);
            for (int worker = 0; worker < WORKERS; ++worker) {
                workers.emplace_back([&, worker] {
                    for (std::int64_t i = 0; i < 200; ++i) {
                        const ViewSpec& view = (worker + i) % 2 == 0 ? hostView : planesView;
                        const Result<ReadLease> lease =
                            syncWait(store->acquire({origin, i % 4}, view, *requester));
                        if (!lease) {
                            failures.fetch_add(1);
                            continue;
                        }
                        const auto* planes = std::get_if<HostPlanesB>(&lease->binding());
                        if (planes == nullptr || !holdsPattern(planes->planes[0], i % 4)) {
                            wrong_bytes.fetch_add(1);
                        }
                    }
                    done.fetch_add(1);
                });
            }
        }
        CHECK(failures.load() == 0);
        CHECK(wrong_bytes.load() == 0);
        CHECK_MESSAGE(broken == 0, first_broken);
        requireInvariants(*store);
        const auto stats = store->stats();
        CHECK(stats.leases == 0);
        CHECK(stats.pins == 0);

        // No reader or pin leaked: the quiescent store trims to nothing.
        store->trim(store->hostDomain(), 0);
        CHECK(store->stats().tiers[0].bytes == 0);
        CHECK(store->stats().frames == 0);
    }

    TEST_CASE_FIXTURE(StoreTest, "the store and its frames are gone after the last reference") {
        const std::size_t baseline = dvv::LifetimeRegistry::instance().liveCount();
        {
            auto store = makeStore();
            auto source = makeSource();
            const OriginId origin = store->attach(source).value();
            for (std::int64_t index = 0; index < 3; ++index) {
                static_cast<void>(take(*store, {origin, index}, hostView, *requester));
            }
            store.reset();
            source.reset();
        }
        CHECK(dvv::LifetimeRegistry::instance().liveCount() == baseline);
    }

    TEST_CASE_FIXTURE(StoreTest, "a view no converter makes is Unsupported") {
        const auto store = makeStore();
        const auto source = makeSource();
        const OriginId origin = store->attach(source).value();
        const Result<ReadLease> result =
            syncWait(store->acquire({origin, 0}, planesView, *requester));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Errc::Unsupported);
        CHECK(contains(result.error().message, "no converter path"));
        CHECK(source->produced.load() == 0);
    }

    TEST_CASE_FIXTURE(StoreTest, "Produce then one converter, single flight") {
        auto unpack = makeUnpack();
        const FakeConverter* const converter = unpack.get();
        std::vector<std::unique_ptr<dvv::media::detail::Converter>> converters;
        converters.push_back(std::move(unpack));
        const auto store = makeStore(std::move(converters));
        const auto source = makeSource();
        Gate gate;
        source->gate = &gate;
        const OriginId origin = store->attach(source).value();
        const FrameKey key{origin, 0};
        bool reached = false;
        std::jthread opener([&] {
            reached = waitFor(
                [&] { return gate.waiting() == 1 && StoreAccess::waiterCount(*store, key) == 1; });
            gate.open();
        });
        auto [x, y] = syncWait(whenAll(store->acquire(key, planesView, *requester),
                                       store->acquire(key, planesView, *requester)));
        opener.join();
        CHECK(reached);
        REQUIRE(x.has_value());
        REQUIRE(y.has_value());
        CHECK(x->fidelity() == dvv::media::Fidelity::Lossless);
        CHECK(y->fidelity() == dvv::media::Fidelity::Lossless);
        CHECK(holdsPattern(planesOf(*x).planes[0], 0));
        CHECK(source->produced.load() == 1);
        CHECK(converter->runs.load() == 1);
        CHECK(store->stats().creations == 2);

        const std::uint64_t hits = store->stats().hits;
        const Result<ReadLease> original = syncWait(store->acquire(key, hostView, *requester));
        REQUIRE(original.has_value());
        CHECK(original->fidelity() == dvv::media::Fidelity::Exact);
        CHECK(store->stats().hits == hits + 1);
        CHECK(source->produced.load() == 1);
    }

    TEST_CASE_FIXTURE(StoreTest, "a frame without the planned view fails the creation") {
        auto unpack = makeUnpack();
        unpack->wrongView = true;
        std::vector<std::unique_ptr<dvv::media::detail::Converter>> converters;
        converters.push_back(std::move(unpack));
        const auto store = makeStore(std::move(converters));
        const auto source = makeSource();
        const OriginId origin = store->attach(source).value();
        const Result<ReadLease> result =
            syncWait(store->acquire({origin, 0}, planesView, *requester));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Errc::Internal);
        CHECK(contains(result.error().message, "lacks the planned view"));
        CHECK(store->stats().failures == 1);
        const Result<void> invariants = StoreAccess::checkInvariants(*store);
        CHECK_MESSAGE(invariants.has_value(),
                      (invariants ? std::string() : invariants.error().message));
        CHECK(store->stats().frames == 1);
    }

    TEST_CASE_FIXTURE(StoreTest, "a converter plan joins a pending original") {
        auto unpack = makeUnpack();
        const FakeConverter* const converter = unpack.get();
        std::vector<std::unique_ptr<dvv::media::detail::Converter>> converters;
        converters.push_back(std::move(unpack));
        const auto store = makeStore(std::move(converters));
        const auto source = makeSource();
        Gate gate;
        source->gate = &gate;
        const OriginId origin = store->attach(source).value();
        {
            // (i) Pending start: the Unpack plan joins the original the first acquire produces.
            const FrameKey key{origin, 0};
            bool reached = false;
            std::jthread opener([&] {
                reached = waitFor([&] {
                    return gate.waiting() == 1 && StoreAccess::waiterCount(*store, key) == 1;
                });
                gate.open();
            });
            auto [original, planes] =
                syncWait(whenAll(store->acquire(key, hostView, *requester),
                                 store->acquire(key, planesView, *requester)));
            opener.join();
            CHECK(reached);
            REQUIRE(original.has_value());
            REQUIRE(planes.has_value());
            CHECK(original->fidelity() == dvv::media::Fidelity::Exact);
            CHECK(planes->fidelity() == dvv::media::Fidelity::Lossless);
            CHECK(holdsPattern(planesOf(*planes).planes[0], 0));
            CHECK(source->produced.load() == 1);
            CHECK(converter->runs.load() == 1);
            const auto stats = store->stats();
            CHECK(stats.misses == 2);
            CHECK(stats.hits == 0);
            CHECK(stats.creations == 2);
            CHECK(stats.failures == 0);
            const Result<void> invariants = StoreAccess::checkInvariants(*store);
            CHECK_MESSAGE(invariants.has_value(),
                          (invariants ? std::string() : invariants.error().message));

            // (ii) Published start: the plan reads the leased original.
            const FrameKey key2{origin, 1};
            const ReadLease original2 = take(*store, key2, hostView, *requester);
            const ReadLease planes2 = take(*store, key2, planesView, *requester);
            CHECK(source->produced.load() == 2);
            CHECK(converter->runs.load() == 2);
            CHECK(planes2.fidelity() == dvv::media::Fidelity::Lossless);
            CHECK(holdsPattern(planesOf(planes2).planes[0], 1));
        }
        // (iii) Every lease is dropped, and both plans released their start input.
        store->trim(store->hostDomain(), 0);
        CHECK(store->stats().tiers[0].bytes == 0);
        CHECK(store->stats().frames == 0);

        // (iv) The joined original fails: the plan fails its output without running Unpack.
        const FrameKey key3{origin, 2};
        Gate gate3;
        source->gate = &gate3;
        source->failNext = 1;
        {
            bool reached = false;
            std::jthread opener([&] {
                reached = waitFor([&] {
                    return gate3.waiting() == 1 && StoreAccess::waiterCount(*store, key3) == 1;
                });
                gate3.open();
            });
            auto [original, planes] =
                syncWait(whenAll(store->acquire(key3, hostView, *requester),
                                 store->acquire(key3, planesView, *requester)));
            opener.join();
            CHECK(reached);
            REQUIRE_FALSE(original.has_value());
            REQUIRE_FALSE(planes.has_value());
            CHECK(original.error().code == Errc::Io);
            CHECK(original.error().message == "fake read error");
            CHECK(planes.error().code == Errc::Io);
            CHECK(planes.error().message == "fake read error");
        }
        REQUIRE(store->stats().failures == 2);  // the original and the Unpack output
        CHECK(converter->runs.load() == 2);
        CHECK(StoreAccess::waiterCount(*store, key3) == 0);
        const Result<ReadLease> later = syncWait(store->acquire(key3, planesView, *requester));
        CHECK(later.has_value());
    }

    // ---- Task 7b: the planner, fidelity, budgets and eviction ----

    TEST_CASE_FIXTURE(StoreTest, "one produce for two concurrent views of a key") {
        auto unpack = makeUnpack();
        const FakeConverter* const converter = unpack.get();
        const auto store = makeStore(listOf(std::move(unpack)));
        const auto source = makeSource();
        Gate gate;
        source->gate = &gate;
        const OriginId origin = store->attach(source).value();
        const FrameKey key{origin, 0};
        bool reached = false;
        std::optional<std::uint32_t> reserved;
        std::jthread opener([&] {
            reached = waitFor(
                [&] { return gate.waiting() == 1 && StoreAccess::waiterCount(*store, key) == 1; });
            reserved = rootReaders(*store, key);
            gate.open();
        });
        auto [original, planes] = syncWait(whenAll(store->acquire(key, hostView, *requester),
                                                   store->acquire(key, planesView, *requester)));
        opener.join();
        CHECK(reached);
        // The second request starts from the pending original (a plan or a join) and reserved its
        // reader when it chose it.
        CHECK(reserved.value_or(0) == 1);
        REQUIRE(original.has_value());
        REQUIRE(planes.has_value());
        CHECK(source->produced.load() == 1);
        CHECK(converter->runs.load() == 1);
        CHECK(StoreAccess::noPathCount(*store) == 0);
        CHECK(store->stats().misses == 2);
        requireInvariants(*store);
    }

    TEST_CASE_FIXTURE(StoreTest, "no path is cached by format, colour and target only") {
        const auto store = makeStore(listOf(makeUnpack()));
        const auto source = makeSource();
        const OriginId origin = store->attach(source).value();
        const ViewSpec buffer = viewOf(desc.format, Api::VkBuffer, Domain{});
        const Result<ReadLease> first = syncWait(store->acquire({origin, 0}, buffer, *requester));
        REQUIRE_FALSE(first.has_value());
        CHECK(first.error().code == Errc::Unsupported);
        CHECK(contains(first.error().message, "no converter path"));
        CHECK(source->attempts.load() == 0);
        CHECK(StoreAccess::noPathCount(*store) == 1);
        CHECK(store->stats().frames == 0);

        const Result<ReadLease> other_frame =
            syncWait(store->acquire({origin, 3}, buffer, *requester));
        REQUIRE_FALSE(other_frame.has_value());
        CHECK(other_frame.error().code == Errc::Unsupported);
        const OriginId second = store->attach(makeSource()).value();
        const Result<ReadLease> other_origin =
            syncWait(store->acquire({second, 0}, buffer, *requester));
        REQUIRE_FALSE(other_origin.has_value());
        CHECK(other_origin.error().code == Errc::Unsupported);
        CHECK(StoreAccess::noPathCount(*store) == 1);

        const auto yuv = dvv::media::findFormat("yuv420p");
        REQUIRE(yuv.has_value());
        const FormatId yuv_format = yuv.value_or(FormatId{0});
        const OriginId third =
            store->attach(TestAuthority::makeShared<FakeSource>(FrameDesc{yuv_format, 0, 16, 8}, 4))
                .value();
        const Result<ReadLease> other_format = syncWait(
            store->acquire({third, 0}, viewOf(yuv_format, Api::VkBuffer, Domain{}), *requester));
        REQUIRE_FALSE(other_format.has_value());
        CHECK(other_format.error().code == Errc::Unsupported);
        CHECK(StoreAccess::noPathCount(*store) == 2);
        requireInvariants(*store);
    }

    TEST_CASE_FIXTURE(StoreTest, "a colour the frame does not have is Unsupported and not cached") {
        const auto store = makeStore();
        const OriginId origin = store->attach(makeSource()).value();
        ViewSpec want = hostView;
        want.colour = dvv::media::internColour(
            {dvv::media::ColourMatrix::Bt709, dvv::media::ColourRange::Limited,
             dvv::media::TransferFn::Bt1886, dvv::media::Primaries::Bt709});
        const Result<ReadLease> result = syncWait(store->acquire({origin, 0}, want, *requester));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Errc::Unsupported);
        CHECK(contains(result.error().message, "no colour conversion"));
        CHECK(StoreAccess::noPathCount(*store) == 0);
        requireInvariants(*store);
    }

    TEST_CASE_FIXTURE(StoreTest, "a domain the store does not have is Unsupported") {
        // A converter into a memory kind the host-only store has no tier for is not registered.
        const auto store = makeStore(listOf(makeConverter(
            "ToStaging", P, {.unpacked = false, .api = Api::VkBuffer, .kind = MemKind::HostVisible},
            Fidelity::Exact, 1)));
        REQUIRE(store->stats().converters.size() == 1);
        CHECK(store->stats().converters[0].name == "Produce");
        const auto source = makeSource();
        const OriginId origin = store->attach(source).value();
        ViewSpec want = hostView;
        want.domain = Domain{MemKind::DeviceLocal};
        const Result<ReadLease> result = syncWait(store->acquire({origin, 0}, want, *requester));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Errc::Unsupported);
        const Result<ReadLease> staging = syncWait(store->acquire(
            {origin, 0}, viewOf(desc.format, Api::VkBuffer, Domain{MemKind::HostVisible}),
            *requester));
        REQUIRE_FALSE(staging.has_value());
        CHECK(staging.error().code == Errc::Unsupported);
        CHECK(StoreAccess::noPathCount(*store) == 0);
        CHECK(source->attempts.load() == 0);
        requireInvariants(*store);
    }

    TEST_CASE_FIXTURE(StoreTest, "a multi-step plan publishes every step and accounts every tier") {
        const Domain staging{MemKind::HostVisible};
        const Domain device{MemKind::DeviceLocal};
        const StateKey on_staging{.unpacked = false, .api = Api::VkBuffer, .kind = staging.kind};
        const StateKey on_device{.unpacked = false, .api = Api::VkBuffer, .kind = device.kind};
        const StateKey image{.unpacked = true, .api = Api::VkImage, .kind = device.kind};
        const auto store =
            makeStore(listOf(makeConverter("ToStaging", P, on_staging, Fidelity::Exact, 1),
                             makeConverter("Upload", on_staging, on_device, Fidelity::Exact, 2),
                             makeConverter("GpuUnpack", on_device, image, Fidelity::Lossless, 0.5,
                                           LayoutPolicy::VkOptimal)),
                      {.staging = staging, .device = device});
        const OriginId origin = store->attach(makeSource()).value();
        const FrameKey key{origin, 0};
        const Result<ReadLease> unpacked = syncWait(
            store->acquire(key, viewOf(planesView.format, Api::VkImage, device), *requester));
        REQUIRE(unpacked.has_value());
        CHECK(unpacked->fidelity() == Fidelity::Lossless);
        auto stats = store->stats();
        CHECK(stats.creations == 4);
        REQUIRE(stats.tiers.size() == 3);
        CHECK(stats.tiers[0].bytes == 128);
        CHECK(stats.tiers[1].bytes == 128);
        CHECK(stats.tiers[2].bytes == 256);

        const std::uint64_t hits = stats.hits;
        const Result<ReadLease> uploaded =
            syncWait(store->acquire(key, viewOf(desc.format, Api::VkBuffer, device), *requester));
        CHECK(uploaded.has_value());
        const Result<ReadLease> staged =
            syncWait(store->acquire(key, viewOf(desc.format, Api::VkBuffer, staging), *requester));
        CHECK(staged.has_value());
        stats = store->stats();
        CHECK(stats.hits == hits + 2);
        CHECK(stats.creations == 4);
        requireInvariants(*store);
    }

    TEST_CASE_FIXTURE(StoreTest, "maxPlanDepth bounds the plan") {
        const Domain staging{MemKind::HostVisible};
        const Domain device{MemKind::DeviceLocal};
        const StateKey on_staging{.unpacked = false, .api = Api::VkBuffer, .kind = staging.kind};
        const StateKey on_device{.unpacked = false, .api = Api::VkBuffer, .kind = device.kind};
        const StateKey image{.unpacked = true, .api = Api::VkImage, .kind = device.kind};
        const auto store =
            makeStore(listOf(makeConverter("ToStaging", P, on_staging, Fidelity::Exact, 1),
                             makeConverter("Upload", on_staging, on_device, Fidelity::Exact, 2),
                             makeConverter("GpuUnpack", on_device, image, Fidelity::Lossless, 0.5,
                                           LayoutPolicy::VkOptimal)),
                      {.staging = staging, .device = device},
                      [](dvv::media::FrameStoreOptions& options) { options.maxPlanDepth = 2; });
        const auto source = makeSource();
        const OriginId origin = store->attach(source).value();
        const Result<ReadLease> result = syncWait(store->acquire(
            {origin, 0}, viewOf(planesView.format, Api::VkImage, device), *requester));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Errc::Unsupported);
        CHECK(StoreAccess::noPathCount(*store) == 1);
        CHECK(source->attempts.load() == 0);
        requireInvariants(*store);
    }

    TEST_CASE_FIXTURE(StoreTest, "an original-format request never returns Derived") {
        const ViewSpec unpacked_buffer = viewOf(planesView.format, Api::VkBuffer, Domain{});
        const ViewSpec buffer = viewOf(desc.format, Api::VkBuffer, Domain{});
        {
            const auto store =
                makeStore(listOf(makeConverter("LossyCopy", P, X, Fidelity::Derived, 1),
                                 makeConverter("BufUnpack", X, U, Fidelity::Lossless, 1)));
            const OriginId origin = store->attach(makeSource()).value();
            const Result<ReadLease> derived =
                syncWait(store->acquire({origin, 0}, unpacked_buffer, *requester));
            REQUIRE(derived.has_value());
            CHECK(derived->fidelity() == Fidelity::Derived);
            const Result<ReadLease> original =
                syncWait(store->acquire({origin, 0}, buffer, *requester));
            REQUIRE_FALSE(original.has_value());
            CHECK(original.error().code == Errc::Unsupported);
            requireInvariants(*store);
        }
        auto lossy = makeConverter("LossyCopy", P, X, Fidelity::Derived, 1);
        auto copy = makeConverter("Copy", P, X, Fidelity::Exact, 5);
        const FakeConverter* const lossy_copy = lossy.get();
        const FakeConverter* const exact_copy = copy.get();
        const auto store = makeStore(listOf(std::move(lossy),
                                            makeConverter("BufUnpack", X, U, Fidelity::Lossless, 1),
                                            std::move(copy)));
        const OriginId origin = store->attach(makeSource()).value();
        // Pass 1 finds Copy then BufUnpack, so the Derived converter is never needed.
        const Result<ReadLease> lossless =
            syncWait(store->acquire({origin, 0}, unpacked_buffer, *requester));
        REQUIRE(lossless.has_value());
        CHECK(lossless->fidelity() == Fidelity::Lossless);
        const Result<ReadLease> exact = syncWait(store->acquire({origin, 0}, buffer, *requester));
        REQUIRE(exact.has_value());
        CHECK(exact->fidelity() == Fidelity::Exact);
        CHECK(exact_copy->runs.load() == 1);
        CHECK(lossy_copy->runs.load() == 0);
        const auto views = StoreAccess::views(*store, {origin, 0});
        std::vector<Fidelity> x_views;
        for (const auto& [view, fidelity] : views) {
            if (view.format == desc.format && view.api == Api::VkBuffer) {
                x_views.push_back(fidelity);
            }
        }
        CHECK(x_views == std::vector<Fidelity>{Fidelity::Exact});
        requireInvariants(*store);
    }

    TEST_CASE_FIXTURE(StoreTest, "the planner prefers Lossless over a cheaper Derived converter") {
        const StateKey planes = {.unpacked = true, .api = Api::HostPlanes, .kind = MemKind::Host};
        {
            auto lossy = makeConverter("Lossy", P, planes, Fidelity::Derived, 1);
            auto unpack = makeConverter("Unpack", P, planes, Fidelity::Lossless, 4);
            const FakeConverter* const lossy_runs = lossy.get();
            const FakeConverter* const unpack_runs = unpack.get();
            const auto store = makeStore(listOf(std::move(lossy), std::move(unpack)));
            const OriginId origin = store->attach(makeSource()).value();
            const ReadLease lease = take(*store, {origin, 0}, planesView, *requester);
            CHECK(lease.fidelity() == Fidelity::Lossless);
            CHECK(unpack_runs->runs.load() == 1);
            CHECK(lossy_runs->runs.load() == 0);
            requireInvariants(*store);
        }
        const auto store =
            makeStore(listOf(makeConverter("Lossy", P, planes, Fidelity::Derived, 1)));
        const OriginId origin = store->attach(makeSource()).value();
        const ReadLease lease = take(*store, {origin, 0}, planesView, *requester);
        CHECK(lease.fidelity() == Fidelity::Derived);
        requireInvariants(*store);
    }

    TEST_CASE_FIXTURE(StoreTest, "eviction order is Derived, Lossless, Exact, then LRU") {
        const auto store =
            makeStore(listOf(makeUnpack(), makeConverter("LossyBuf", P, U, Fidelity::Derived, 1)));
        const OriginId origin = store->attach(makeSource()).value();
        const FrameKey key{origin, 0};
        static_cast<void>(take(*store, key, hostView, *requester));
        static_cast<void>(take(*store, key, planesView, *requester));
        static_cast<void>(
            take(*store, key, viewOf(planesView.format, Api::VkBuffer, Domain{}), *requester));
        CHECK(store->stats().tiers[0].bytes == 384);
        const auto fidelities = [&] {
            std::vector<Fidelity> list;
            for (const auto& [view, fidelity] : StoreAccess::views(*store, key)) {
                list.push_back(fidelity);
            }
            return list;
        };
        CHECK(fidelities() ==
              std::vector<Fidelity>{Fidelity::Exact, Fidelity::Lossless, Fidelity::Derived});

        store->trim(store->hostDomain(), 256);
        CHECK(fidelities() == std::vector<Fidelity>{Fidelity::Exact, Fidelity::Lossless});
        store->trim(store->hostDomain(), 128);
        CHECK(fidelities() == std::vector<Fidelity>{Fidelity::Exact});
        store->trim(store->hostDomain(), 0);
        CHECK(StoreAccess::views(*store, key).empty());
        CHECK(store->stats().evictions == 3);
        requireInvariants(*store);
    }

    TEST_CASE_FIXTURE(StoreTest, "over budget, unleased entries are evicted after an acquire") {
        const auto store = makeStore(
            {}, {}, [](dvv::media::FrameStoreOptions& options) { options.hostBudget = 256; });
        const OriginId origin = store->attach(makeSource(6)).value();
        std::vector<ReadLease> leases;
        leases.reserve(6);
        for (std::int64_t index = 0; index < 5; ++index) {
            leases.push_back(take(*store, {origin, index}, hostView, *requester));
        }
        auto stats = store->stats();
        CHECK(stats.tiers[0].bytes == 640);
        CHECK(stats.evictions == 0);

        leases.erase(leases.begin(), leases.begin() + 3);
        leases.push_back(take(*store, {origin, 5}, hostView, *requester));
        stats = store->stats();
        CHECK(stats.tiers[0].bytes == 384);
        CHECK(stats.evictions == 3);
        CHECK(stats.frames == 3);
        requireInvariants(*store);
    }

    TEST_CASE_FIXTURE(StoreTest, "the budget is enforced after a converter plan and after adopt") {
        const auto budget = [](dvv::media::FrameStoreOptions& options) {
            options.hostBudget = 256;
        };
        {
            // allocateHost of frame 1 sees 128 bytes and evicts nothing. The plan then publishes
            // frame 1 and its unpacked view (384 bytes), so only the eviction after the plan
            // brings the tier back to its budget.
            const auto store = makeStore(listOf(makeUnpack()), {}, budget);
            const OriginId origin = store->attach(makeSource()).value();
            static_cast<void>(take(*store, {origin, 0}, hostView, *requester));
            const ReadLease planes = take(*store, {origin, 1}, planesView, *requester);
            const auto stats = store->stats();
            CHECK(stats.tiers[0].bytes == 256);
            CHECK(stats.evictions == 1);
            CHECK(StoreAccess::views(*store, {origin, 0}).empty());
            requireInvariants(*store);
        }
        // Two unleased frames fill the budget; adopt allocates nothing through the store, so only
        // its own eviction brings the tier back to its budget.
        const auto store = makeStore({}, {}, budget);
        const OriginId origin = store->attach(makeSource()).value();
        static_cast<void>(take(*store, {origin, 0}, hostView, *requester));
        static_cast<void>(take(*store, {origin, 1}, hostView, *requester));
        CHECK(store->stats().tiers[0].bytes == 256);
        const ViewSpec tight{desc.format, 0, LayoutPolicy::Tight, Api::HostPlanes, {}, 0};
        auto frame = TestAuthority::makeShared<FakeFrame>(
            desc, std::vector<ViewSpec>{tight}, std::vector<std::byte>(128, std::byte{7}));
        const Result<ReadLease> adopted = store->adopt({77, 0}, std::move(frame));
        REQUIRE(adopted.has_value());
        const auto stats = store->stats();
        CHECK(stats.tiers[0].bytes == 256);
        CHECK(stats.evictions == 1);
        CHECK(StoreAccess::views(*store, {origin, 0}).empty());
        requireInvariants(*store);
    }

    TEST_CASE_FIXTURE(StoreTest, "an OutOfMemory step evicts to half the budget and retries once") {
        const auto budget = [](dvv::media::FrameStoreOptions& options) {
            options.hostBudget = 512;
        };
        {
            auto unpack = makeUnpack();
            unpack->failWith = Errc::OutOfMemory;
            unpack->failNext = 1;
            const FakeConverter* const converter = unpack.get();
            const auto store = makeStore(listOf(std::move(unpack)), {}, budget);
            const OriginId origin = store->attach(makeSource()).value();
            for (std::int64_t index = 1; index < 4; ++index) {
                static_cast<void>(take(*store, {origin, index}, hostView, *requester));
            }
            CHECK(store->stats().tiers[0].bytes == 384);
            const Result<ReadLease> planes =
                syncWait(store->acquire({origin, 0}, planesView, *requester));
            REQUIRE(planes.has_value());
            CHECK(converter->runs.load() == 2);
            CHECK(store->stats().evictions == 2);  // frames 1 and 2: 512 -> 256
            requireInvariants(*store);
        }
        auto unpack = makeUnpack();
        unpack->failWith = Errc::OutOfMemory;
        unpack->failNext = 2;
        const FakeConverter* const converter = unpack.get();
        const auto store = makeStore(listOf(std::move(unpack)), {}, budget);
        const OriginId origin = store->attach(makeSource()).value();
        const Result<ReadLease> planes =
            syncWait(store->acquire({origin, 0}, planesView, *requester));
        REQUIRE_FALSE(planes.has_value());
        CHECK(planes.error().code == Errc::OutOfMemory);
        CHECK(converter->runs.load() == 2);
        CHECK(store->stats().failures == 1);
        requireInvariants(*store);
    }

    TEST_CASE_FIXTURE(StoreTest, "allocateHost evicts and retries once") {
        const auto store = makeStore(
            {}, {}, [](dvv::media::FrameStoreOptions& options) { options.hostBudget = 512; });
        const OriginId origin = store->attach(makeSource(6)).value();
        for (std::int64_t index = 1; index < 4; ++index) {
            static_cast<void>(take(*store, {origin, index}, hostView, *requester));
        }
        std::atomic<int> calls{0};
        StoreAccess::setHostAllocator(
            *store,
            [&calls](std::size_t bytes,
                     std::size_t alignment) -> Result<dvv::platform::AlignedBuffer> {
                if (calls.fetch_add(1) == 0) {
                    return dvv::makeError(Errc::OutOfMemory, "fake allocation failure");
                }
                return dvv::platform::allocateAligned(bytes, alignment);
            });
        const Result<ReadLease> first = syncWait(store->acquire({origin, 0}, hostView, *requester));
        REQUIRE(first.has_value());
        CHECK(calls.load() == 2);
        CHECK(store->stats().evictions == 1);  // frame 1: 384 -> 256

        std::atomic<int> failing{0};
        StoreAccess::setHostAllocator(
            *store,
            [&failing](std::size_t /*bytes*/,
                       std::size_t /*alignment*/) -> Result<dvv::platform::AlignedBuffer> {
                failing.fetch_add(1);
                return dvv::makeError(Errc::OutOfMemory, "fake allocation failure");
            });
        const Result<ReadLease> second =
            syncWait(store->acquire({origin, 4}, hostView, *requester));
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error().code == Errc::OutOfMemory);
        CHECK(failing.load() == 2);
        requireInvariants(*store);
    }

    TEST_CASE_FIXTURE(StoreTest, "stats count converter runs and bytes") {
        const auto store = makeStore(listOf(makeUnpack()));
        const OriginId origin = store->attach(makeSource()).value();
        static_cast<void>(take(*store, {origin, 0}, planesView, *requester));
        static_cast<void>(take(*store, {origin, 1}, planesView, *requester));
        const auto stats = store->stats();
        REQUIRE(stats.converters.size() == 2);
        CHECK(stats.converters[0].name == "Produce");
        CHECK(stats.converters[0].runs == 2);
        CHECK(stats.converters[0].bytes == 256);
        CHECK(stats.converters[1].name == "Unpack");
        CHECK(stats.converters[1].runs == 2);
        CHECK(stats.converters[1].bytes == 256);
        CHECK(stats.misses == 2);
        CHECK(stats.creations == 4);
        requireInvariants(*store);
    }

    TEST_CASE_FIXTURE(StoreTest, "layout Any matches; an unmade layout is Unsupported") {
        const auto store = makeStore();
        const OriginId origin = store->attach(makeSource()).value();
        static_cast<void>(take(*store, {origin, 0}, hostView, *requester));
        const std::uint64_t hits = store->stats().hits;
        ViewSpec tight = hostView;
        tight.layout = LayoutPolicy::Tight;
        static_cast<void>(take(*store, {origin, 0}, tight, *requester));
        CHECK(store->stats().hits == hits + 1);
        ViewSpec aligned = hostView;
        aligned.layout = LayoutPolicy::RowAlign256;
        const Result<ReadLease> result = syncWait(store->acquire({origin, 0}, aligned, *requester));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Errc::Unsupported);
        CHECK(StoreAccess::noPathCount(*store) == 1);
        requireInvariants(*store);
    }

    TEST_CASE_FIXTURE(StoreTest, "trim of an unknown domain does nothing") {
        const auto store = makeStore();
        const OriginId origin = store->attach(makeSource()).value();
        static_cast<void>(take(*store, {origin, 0}, hostView, *requester));
        store->trim(Domain{MemKind::DeviceLocal}, 0);
        CHECK(store->stats().frames == 1);
        CHECK(store->stats().evictions == 0);
        requireInvariants(*store);
    }
}
