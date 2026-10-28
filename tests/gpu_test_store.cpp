#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "dvv/base/error.h"
#include "dvv/base/executor.h"
#include "dvv/base/executors.h"
#include "dvv/base/task.h"
#include "dvv/gpu/execution_context.h"
#include "dvv/gpu/resources.h"
#include "dvv/media/frame.h"
#include "dvv/media/frame_store.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"
#include "gpu_fixture.h"
#include "gpu_unpack.h"
#include "packed_source.h"
#include "store_state.h"

namespace media = dvv::media;
namespace gpu = dvv::gpu;
using dvv::Errc;
using dvv::syncWait;
using media::Api;
using media::Domain;
using media::Fidelity;
using media::FormatId;
using media::FrameDesc;
using media::FrameKey;
using media::FrameStore;
using media::HostPlanesB;
using media::LayoutPolicy;
using media::OriginId;
using media::ReadLease;
using media::ViewSpec;
using media::test::concatenated;
using media::test::GpuTest;
using media::test::packedOriginal;
using media::test::packedPlanes;
using media::test::PackedSource;
using media::test::runsOf;

namespace {

constexpr std::uint64_t SEED = 3;
constexpr std::array<std::string_view, 6> FORMATS{"yuv420p10le", "v210",    "nv12",
                                                  "p010le",      "rgb48be", "yuyv422"};

FormatId idOf(std::string_view name) {
    const auto id = media::findFormat(name);
    REQUIRE_MESSAGE(id.has_value(), name);
    return id.value_or(FormatId{0});
}

// A store on the fixture's context with a PackedSource (seed 3) attached. Declared in the test
// body, so it is destroyed before the fixture's context.
struct GpuStore {
    GpuStore(GpuTest& test, FrameDesc frame, std::int64_t frames, std::size_t device_budget = 0)
        : desc(frame) {
        pool = dvv::ExecutorFactory::makeThreadPool(2).value();
        auto made = FrameStore::create({.cpu = pool,
                                        .io = pool,
                                        .gpu = test.context().shared_from_this(),
                                        .deviceBudget = device_budget});
        REQUIRE_MESSAGE(made.has_value(), made.error().describe());
        store = std::move(*made);
        auto attached = store->attach(std::make_shared<PackedSource>(desc, frames, SEED));
        REQUIRE_MESSAGE(attached.has_value(), attached.error().describe());
        origin = *attached;
        REQUIRE(store->stagingDomain().has_value());
        REQUIRE(store->deviceDomain().has_value());
        staging = store->stagingDomain().value_or(Domain{});
        device = store->deviceDomain().value_or(Domain{});
    }

    [[nodiscard]] dvv::Result<media::OriginalRead> read(std::int64_t index, Api api, Domain domain,
                                                        dvv::IExecutor& resume_on,
                                                        bool require_exact = false) const {
        return syncWait(
            store->acquireOriginal({origin, index}, api, domain, resume_on, require_exact));
    }

    [[nodiscard]] ReadLease take(std::int64_t index, const ViewSpec& want,
                                 dvv::IExecutor& resume_on) const {
        dvv::Result<ReadLease> lease = syncWait(store->acquire({origin, index}, want, resume_on));
        REQUIRE_MESSAGE(lease.has_value(), lease.error().describe());
        return std::move(*lease);
    }

    [[nodiscard]] std::size_t tierBytes(const Domain& domain) const {
        for (const media::TierStats& tier : store->stats().tiers) {
            if (tier.domain == domain) {
                return tier.bytes;
            }
        }
        FAIL("no tier for the domain");
        return 0;
    }

    FrameDesc desc;
    std::shared_ptr<dvv::ThreadPool> pool;
    std::shared_ptr<FrameStore> store;
    OriginId origin = 0;
    Domain staging{};
    Domain device{};
};

const HostPlanesB& hostPlanesOf(const ReadLease& lease) {
    const auto* planes = std::get_if<HostPlanesB>(&lease.binding());
    REQUIRE(planes != nullptr);
    return *planes;
}

// An adopted original in a DeviceOnly buffer of exactly the frame's bytes (no unpack padding). A
// test double, not managed.
class UnpaddedBufferFrame final : public media::IFrame {
public:
    UnpaddedBufferFrame(FrameDesc desc, ViewSpec view, std::shared_ptr<gpu::GpuBuffer> buffer,
                        media::FrameGeometry geometry)
        : desc_(desc), view_(view), buffer_(std::move(buffer)), geometry_(geometry) {}
    [[nodiscard]] const FrameDesc& desc() const noexcept override { return desc_; }
    [[nodiscard]] std::span<const ViewSpec> nativeViews() const noexcept override {
        return {&view_, 1};
    }
    [[nodiscard]] media::Binding bind(const ViewSpec& native) const override {
        if (native != view_) {
            return HostPlanesB{};
        }
        return media::VkBufferB{.buffer = buffer_, .offset = 0, .geometry = geometry_};
    }
    [[nodiscard]] std::size_t bytes() const noexcept override {
        return static_cast<std::size_t>(buffer_->size());
    }

private:
    FrameDesc desc_;
    ViewSpec view_;
    std::shared_ptr<gpu::GpuBuffer> buffer_;
    media::FrameGeometry geometry_;
};

}  // namespace

TEST_CASE_FIXTURE(GpuTest, "store gpu: planes via the GPU equal the CPU reference") {
    for (const std::string_view name : FORMATS) {
        SUBCASE(std::string(name).c_str()) {
            const FrameDesc desc{.format = idOf(name), .colour = 0, .width = 34, .height = 17};
            const GpuStore test(*this, desc, 3);
            const auto read = test.read(2, Api::HostPlanes, test.staging, executor());
            REQUIRE_MESSAGE(read.has_value(), read.error().describe());
            CHECK(read->lease().fidelity() == Fidelity::Lossless);
            CHECK(read->lease().view().format == media::unpackedFormat(desc.format));
            const auto expected = packedPlanes(desc.format, desc.width, desc.height, SEED, 2);
            REQUIRE_MESSAGE(expected.has_value(), expected.error().describe());
            CHECK(concatenated(hostPlanesOf(read->lease())) == *expected);
            const auto stats = test.store->stats();
            CHECK(runsOf(stats, "HostToStaging") == 1);
            CHECK(runsOf(stats, "Upload") == 1);
            CHECK(runsOf(stats, "GpuUnpack") == 1);
            CHECK(runsOf(stats, "Readback") == 1);
            CHECK(runsOf(stats, "CpuUnpack") == 0);
            REQUIRE(media::detail::StoreAccess::checkInvariants(*test.store).has_value());
        }
    }
}

TEST_CASE_FIXTURE(GpuTest, "store gpu: requireExact via staging returns the original bytes") {
    for (const std::string_view name : FORMATS) {
        SUBCASE(std::string(name).c_str()) {
            const FrameDesc desc{.format = idOf(name), .colour = 0, .width = 34, .height = 17};
            const GpuStore test(*this, desc, 3);
            const auto read = test.read(2, Api::HostPlanes, test.staging, executor(), true);
            REQUIRE_MESSAGE(read.has_value(), read.error().describe());
            CHECK(read->lease().fidelity() == Fidelity::Exact);
            CHECK(read->lease().view().format == desc.format);
            const auto expected = packedOriginal(desc.format, desc.width, desc.height, SEED, 2);
            REQUIRE_MESSAGE(expected.has_value(), expected.error().describe());
            CHECK(concatenated(hostPlanesOf(read->lease())) == *expected);
            const auto stats = test.store->stats();
            CHECK(runsOf(stats, "Upload") == 1);
            CHECK(runsOf(stats, "Readback") == 1);
            CHECK(runsOf(stats, "GpuUnpack") == 0);
        }
    }
}

TEST_CASE_FIXTURE(GpuTest,
                  "store gpu: HostReadback serves both views, HostUpload only the buffer view") {
    const FrameDesc desc{.format = idOf("v210"), .colour = 0, .width = 34, .height = 17};
    const GpuStore test(*this, desc, 3);
    FrameStore& store = *test.store;

    // 1. The exact read's Readback entry stays leased; the HostUpload entry is evicted.
    const auto exact = test.read(0, Api::HostPlanes, test.staging, executor(), true);
    REQUIRE_MESSAGE(exact.has_value(), exact.error().describe());
    const auto before = store.stats();
    store.trim(test.staging, 0);
    const auto layout = media::geometry(desc.format, desc.width, desc.height);
    REQUIRE(layout.has_value());
    CHECK(test.tierBytes(test.staging) == layout->bytes);  // only the Readback buffer is left
    {
        const ReadLease buffer =
            test.take(0, media::originalView(desc, Api::VkBuffer, test.staging), executor());
        const auto after = store.stats();
        CHECK(after.hits == before.hits + 1);
        CHECK(runsOf(after, "Readback") == runsOf(before, "Readback"));
        CHECK(runsOf(after, "HostToStaging") == runsOf(before, "HostToStaging"));
        const auto* binding = std::get_if<media::VkBufferB>(&buffer.binding());
        REQUIRE(binding != nullptr);
        REQUIRE(binding->buffer != nullptr);
        CHECK(binding->buffer->desc().memory == gpu::MemoryUsage::HostReadback);
    }

    // 2. A new key's device planes go through a HostUpload staging.
    const auto staged = store.stats();
    {
        const ReadLease planes =
            test.take(1, media::planesView(desc, Api::VkImage, test.device), executor());
        const auto after = store.stats();
        CHECK(runsOf(after, "HostToStaging") == runsOf(staged, "HostToStaging") + 1);
        const auto views = media::detail::StoreAccess::views(store, {test.origin, 1});
        const ViewSpec upload{desc.format,   desc.colour,  LayoutPolicy::Tight,
                              Api::VkBuffer, test.staging, 0};
        CHECK(std::ranges::any_of(views, [&](const auto& view) { return view.first == upload; }));
    }

    // 3. The host view in the staging domain comes only from Readback, from the device original.
    const auto uploaded = store.stats();
    const ReadLease host =
        test.take(1, media::originalView(desc, Api::HostPlanes, test.staging), executor());
    const auto after = store.stats();
    CHECK(runsOf(after, "Readback") == runsOf(uploaded, "Readback") + 1);
    CHECK(runsOf(after, "Upload") == runsOf(uploaded, "Upload"));
    CHECK(runsOf(after, "HostToStaging") == runsOf(uploaded, "HostToStaging"));
    const auto expected = packedOriginal(desc.format, desc.width, desc.height, SEED, 1);
    REQUIRE_MESSAGE(expected.has_value(), expected.error().describe());
    CHECK(concatenated(hostPlanesOf(host)) == *expected);
}

TEST_CASE_FIXTURE(GpuTest, "store gpu: a HostReadback staging entry is an Upload source") {
    const FrameDesc desc{.format = idOf("v210"), .colour = 0, .width = 34, .height = 17};
    const GpuStore test(*this, desc, 3);
    FrameStore& store = *test.store;

    // Only the leased Readback entry is left: the HostUpload staging and the device original are
    // evicted, so the cheapest way to the device is Upload from the Readback buffer.
    const auto exact = test.read(0, Api::HostPlanes, test.staging, executor(), true);
    REQUIRE_MESSAGE(exact.has_value(), exact.error().describe());
    store.trim(test.staging, 0);
    store.trim(test.device, 0);
    REQUIRE(test.tierBytes(test.device) == 0);

    const auto before = store.stats();
    const auto planes = syncWait(store.acquire(
        {test.origin, 0}, media::planesView(desc, Api::VkImage, test.device), executor()));
    REQUIRE_MESSAGE(planes.has_value(), planes.error().describe());
    const auto uploaded = store.stats();
    CHECK(runsOf(uploaded, "Upload") == runsOf(before, "Upload") + 1);
    CHECK(runsOf(uploaded, "HostToStaging") == runsOf(before, "HostToStaging"));
    CHECK(runsOf(uploaded, "GpuUnpack") == runsOf(before, "GpuUnpack") + 1);

    // The device planes, read back, equal the CPU reference.
    const ReadLease host =
        test.take(0, media::planesView(desc, Api::HostPlanes, test.staging), executor());
    const auto after = store.stats();
    CHECK(runsOf(after, "Readback") == runsOf(uploaded, "Readback") + 1);
    CHECK(runsOf(after, "CpuUnpack") == runsOf(uploaded, "CpuUnpack"));
    const auto expected = packedPlanes(desc.format, desc.width, desc.height, SEED, 0);
    REQUIRE_MESSAGE(expected.has_value(), expected.error().describe());
    CHECK(concatenated(hostPlanesOf(host)) == *expected);
}

TEST_CASE_FIXTURE(GpuTest,
                  "store gpu: the planner picks the CPU for host targets and the GPU for device "
                  "targets") {
    const FormatId id = idOf("p010le");
    const FrameDesc desc{.format = id, .colour = 0, .width = 34, .height = 17};
    const GpuStore test(*this, desc, 3);

    const auto read = test.read(0, Api::HostPlanes, test.store->hostDomain(), executor());
    REQUIRE_MESSAGE(read.has_value(), read.error().describe());
    auto stats = test.store->stats();
    CHECK(runsOf(stats, "CpuUnpack") == 1);
    CHECK(runsOf(stats, "GpuUnpack") == 0);
    CHECK(runsOf(stats, "Upload") == 0);

    const ReadLease planes =
        test.take(0, media::planesView(desc, Api::VkImage, test.device), executor());
    stats = test.store->stats();
    CHECK(runsOf(stats, "GpuUnpack") == 1);
    const auto* images = std::get_if<media::VkImageB>(&planes.binding());
    REQUIRE(images != nullptr);
    const media::PixelFormatDescriptor& format = media::descriptor(id);
    REQUIRE(images->planeCount == format.componentCount);
    for (std::size_t c = 0; c < images->planeCount; ++c) {
        const gpu::GpuImage* image = images->planes[c].get();
        REQUIRE(image != nullptr);
        const auto [w, h] = media::componentSize(id, c, desc.width, desc.height);
        CHECK(image->format() == VK_FORMAT_R16_UINT);
        CHECK(image->width() == w);
        CHECK(image->height() == h);
        CHECK(image->layout() == VK_IMAGE_LAYOUT_GENERAL);
        CHECK(image->view() != VK_NULL_HANDLE);
    }
}

TEST_CASE_FIXTURE(GpuTest, "store gpu: device-tier eviction frees memory") {
    const FormatId id = idOf("yuv420p10le");
    const auto frame = media::geometry(id, 64, 64);
    REQUIRE(frame.has_value());
    const auto unpacked = media::geometry(media::unpackedFormat(id), 64, 64);
    REQUIRE(unpacked.has_value());
    const auto budget = static_cast<std::size_t>(
        3 * (media::detail::unpackBufferBytes(0, frame->bytes) + unpacked->bytes));
    const FrameDesc desc{.format = id, .colour = 0, .width = 64, .height = 64};
    const GpuStore test(*this, desc, 10, budget);
    const std::uint64_t freed = context().retireStats().freed;

    for (std::int64_t index = 0; index < 10; ++index) {
        {
            const ReadLease planes =
                test.take(index, media::planesView(desc, Api::VkImage, test.device), executor());
        }
        CHECK(test.tierBytes(test.device) <= budget);
    }
    CHECK(test.store->stats().evictions > 0);
    test.store->trim(test.device, 0);
    CHECK(test.tierBytes(test.device) == 0);
    REQUIRE(context().drain().has_value());
    const gpu::RetireStats retired = context().retireStats();
    CHECK(retired.freed > freed);
    CHECK(retired.retiring == 0);
}

TEST_CASE_FIXTURE(GpuTest, "store gpu: an adopted unpadded buffer fails the unpack") {
    const FormatId id = idOf("yuv420p10le");
    const FrameDesc desc{.format = id, .colour = 0, .width = 33, .height = 17};
    const GpuStore test(*this, desc, 1);
    const auto layout = media::geometry(id, desc.width, desc.height);
    REQUIRE(layout.has_value());
    auto buffer = context().createBuffer({.size = layout->bytes,
                                          .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          .memory = gpu::MemoryUsage::DeviceOnly,
                                          .name = "unpadded"});
    REQUIRE_MESSAGE(buffer.has_value(), buffer.error().describe());
    const ViewSpec view{desc.format,   desc.colour, LayoutPolicy::Tight,
                        Api::VkBuffer, test.device, 0};
    auto frame = std::make_shared<UnpaddedBufferFrame>(desc, view, std::move(*buffer), *layout);
    const auto adopted = test.store->adopt({test.origin, 0}, std::move(frame));
    REQUIRE_MESSAGE(adopted.has_value(), adopted.error().describe());

    const auto planes = syncWait(test.store->acquire(
        {test.origin, 0}, media::planesView(desc, Api::VkImage, test.device), executor()));
    REQUIRE_FALSE(planes.has_value());
    CHECK(planes.error().code == Errc::Internal);
}
