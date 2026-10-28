#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "converters.h"
#include "dvv/base/error.h"
#include "dvv/base/task.h"
#include "dvv/media/display.h"
#include "dvv/media/frame_store.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"
#include "packed_source.h"
#include "store_fakes.h"
#include "store_state.h"

namespace {

using dvv::Errc;
using dvv::Result;
using dvv::syncWait;
using dvv::media::Api;
using dvv::media::Domain;
using dvv::media::Fidelity;
using dvv::media::FrameDesc;
using dvv::media::FrameKey;
using dvv::media::FrameStore;
using dvv::media::FrameStoreOptions;
using dvv::media::LayoutPolicy;
using dvv::media::MemKind;
using dvv::media::OriginId;
using dvv::media::ReadLease;
using dvv::media::ViewSpec;
using dvv::media::detail::Converter;
using dvv::media::detail::DISPLAY_STATE;
using dvv::media::detail::PRODUCED;
using dvv::media::detail::StateKey;
using dvv::media::detail::StoreAccess;
using dvv::media::test::FakeConverter;
using dvv::media::test::PackedSource;
using dvv::media::test::StoreTest;

constexpr Domain STAGING{MemKind::HostVisible};
constexpr Domain DEVICE{MemKind::DeviceLocal};
constexpr StateKey ON_STAGING{
    .unpacked = false, .api = Api::VkBuffer, .kind = MemKind::HostVisible};
constexpr StateKey ON_DEVICE{.unpacked = false, .api = Api::VkBuffer, .kind = MemKind::DeviceLocal};
constexpr StateKey DEVICE_PLANES{
    .unpacked = true, .api = Api::VkImage, .kind = MemKind::DeviceLocal};

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

void requireInvariants(const FrameStore& store) {
    const Result<void> invariants = StoreAccess::checkInvariants(store);
    REQUIRE_MESSAGE(invariants.has_value(),
                    (invariants ? std::string() : invariants.error().message));
}

// Fakes mirroring the real chain to the display state; the raw pointers stay valid while the
// store lives.
struct DisplayChain {
    explicit DisplayChain(bool with_to_display) {
        const auto add = [this](std::string name, StateKey from, StateKey to, LayoutPolicy layout,
                                Fidelity fidelity, double cost) {
            auto fake =
                std::make_unique<FakeConverter>(FakeConverter::Config{.name = std::move(name),
                                                                      .from = from,
                                                                      .to = to,
                                                                      .layout = layout,
                                                                      .fidelity = fidelity,
                                                                      .cost = cost});
            fakes.push_back(fake.get());
            converters.push_back(std::move(fake));
        };
        add("ToStaging", PRODUCED, ON_STAGING, LayoutPolicy::Tight, Fidelity::Exact, 1);
        add("Upload", ON_STAGING, ON_DEVICE, LayoutPolicy::Tight, Fidelity::Exact, 2);
        add("GpuUnpack", ON_DEVICE, DEVICE_PLANES, LayoutPolicy::VkOptimal, Fidelity::Lossless,
            0.5);
        if (with_to_display) {
            add("FakeToDisplay", DEVICE_PLANES, DISPLAY_STATE, LayoutPolicy::VkOptimal,
                Fidelity::Derived, 1);
        }
    }
    std::vector<std::unique_ptr<Converter>> converters;
    std::vector<FakeConverter*> fakes;
};

}  // namespace

TEST_SUITE("store") {
    TEST_CASE_FIXTURE(StoreTest, "display: reachable only through the colour-changing converter") {
        {
            DisplayChain chain(false);
            const auto store =
                makeStore(std::move(chain.converters), {.staging = STAGING, .device = DEVICE});
            const OriginId origin = store->attach(makeSource()).value();
            const Result<ReadLease> result =
                syncWait(store->acquire({origin, 0}, dvv::media::displayView(DEVICE), *requester));
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().code == Errc::Unsupported);
            CHECK(contains(result.error().message, "no converter path"));
            requireInvariants(*store);
        }
        DisplayChain chain(true);
        const std::vector<FakeConverter*> fakes = chain.fakes;
        const auto store =
            makeStore(std::move(chain.converters), {.staging = STAGING, .device = DEVICE});
        const auto source = makeSource();
        const OriginId origin = store->attach(source).value();
        const FrameKey key{origin, 0};
        {
            const Result<ReadLease> lease =
                syncWait(store->acquire(key, dvv::media::displayView(DEVICE), *requester));
            REQUIRE_MESSAGE(lease.has_value(), lease.error().describe());
            CHECK(lease->fidelity() == Fidelity::Derived);
            CHECK(lease->view().format == dvv::media::displayFormat());
            CHECK(lease->view().colour == dvv::media::displayColour());
            CHECK(lease->view().api == Api::VkImage);
            CHECK(lease->view().domain == DEVICE);
        }
        CHECK(source->produced.load() == 1);
        for (const FakeConverter* fake : fakes) {
            CAPTURE(fake->name());
            CHECK(fake->runs.load() == 1);
        }
        const std::uint64_t hits = store->stats().hits;
        {
            const Result<ReadLease> again =
                syncWait(store->acquire(key, dvv::media::displayView(DEVICE), *requester));
            REQUIRE(again.has_value());
            CHECK(again->fidelity() == Fidelity::Derived);
        }
        CHECK(store->stats().hits == hits + 1);
        CHECK(source->produced.load() == 1);
        for (const FakeConverter* fake : fakes) {
            CAPTURE(fake->name());
            CHECK(fake->runs.load() == 1);
        }
        requireInvariants(*store);
    }

    TEST_CASE_FIXTURE(StoreTest, "display: the five-step plan fits the default depth") {
        CHECK(FrameStoreOptions{}.maxPlanDepth == 6);
        {
            DisplayChain chain(true);
            const auto store =
                makeStore(std::move(chain.converters), {.staging = STAGING, .device = DEVICE});
            const OriginId origin = store->attach(makeSource()).value();
            const Result<ReadLease> lease =
                syncWait(store->acquire({origin, 0}, dvv::media::displayView(DEVICE), *requester));
            REQUIRE_MESSAGE(lease.has_value(), lease.error().describe());
            CHECK(store->stats().creations == 5);
            requireInvariants(*store);
        }
        DisplayChain chain(true);
        const auto store =
            makeStore(std::move(chain.converters), {.staging = STAGING, .device = DEVICE},
                      [](FrameStoreOptions& options) { options.maxPlanDepth = 3; });
        const OriginId origin = store->attach(makeSource()).value();
        const Result<ReadLease> result =
            syncWait(store->acquire({origin, 0}, dvv::media::displayView(DEVICE), *requester));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Errc::Unsupported);
        requireInvariants(*store);
    }

    TEST_CASE_FIXTURE(StoreTest, "display: the display colour on another format is refused") {
        DisplayChain chain(true);
        const auto store =
            makeStore(std::move(chain.converters), {.staging = STAGING, .device = DEVICE});
        const OriginId origin = store->attach(makeSource()).value();
        const ViewSpec planes{.format = dvv::media::unpackedFormat(desc.format),
                              .colour = dvv::media::displayColour(),
                              .layout = LayoutPolicy::Any,
                              .api = Api::VkImage,
                              .domain = DEVICE};
        const Result<ReadLease> other = syncWait(store->acquire({origin, 0}, planes, *requester));
        REQUIRE_FALSE(other.has_value());
        CHECK(other.error().code == Errc::Unsupported);
        CHECK(contains(other.error().message, "no colour conversion"));

        ViewSpec display = dvv::media::displayView(DEVICE);
        display.colour = desc.colour;
        const Result<ReadLease> own = syncWait(store->acquire({origin, 0}, display, *requester));
        REQUIRE_FALSE(own.has_value());
        CHECK(own.error().code == Errc::Unsupported);
        CHECK(contains(own.error().message, "display view has colour"));
        requireInvariants(*store);
    }

    TEST_CASE_FIXTURE(StoreTest, "display: a host-only store has no display path") {
        auto made = FrameStore::create({.cpu = cpu, .io = io});
        REQUIRE(made.has_value());
        const std::shared_ptr<FrameStore> store = std::move(*made);
        const OriginId origin = store->attach(std::make_shared<PackedSource>(desc, 1, 11)).value();
        const Domain device{
            .kind = MemKind::DeviceLocal, .device = dvv::gpu::DeviceId{0}, .generation = 1};
        const Result<ReadLease> result =
            syncWait(store->acquire({origin, 0}, dvv::media::displayView(device), *requester));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Errc::Unsupported);
        requireInvariants(*store);
    }

    TEST_CASE_FIXTURE(StoreTest, "display: acquireOriginal refuses a display-format origin") {
        auto made = FrameStore::create({.cpu = cpu, .io = io});
        REQUIRE(made.has_value());
        const std::shared_ptr<FrameStore> store = std::move(*made);
        const FrameDesc display{
            .format = dvv::media::displayFormat(), .colour = 0, .width = 1, .height = 1};
        const OriginId origin =
            store->attach(std::make_shared<PackedSource>(display, 1, 11)).value();
        const auto read = syncWait(store->acquireOriginal({origin, 0}, Api::HostPlanes,
                                                          store->hostDomain(), *requester, true));
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().code == Errc::Internal);
        CHECK(contains(read.error().message, "never an original"));
        requireInvariants(*store);
    }
}
