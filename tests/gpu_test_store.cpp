#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "converters.h"
#include "dvv/base/error.h"
#include "dvv/base/executor.h"
#include "dvv/base/executors.h"
#include "dvv/base/task.h"
#include "dvv/gpu/context_vulkan.h"
#include "dvv/gpu/execution_context.h"
#include "dvv/gpu/image.h"
#include "dvv/gpu/resources.h"
#include "dvv/media/display.h"
#include "dvv/media/frame.h"
#include "dvv/media/frame_store.h"
#include "dvv/media/frame_vulkan.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"
#include "frames_vulkan.h"
#include "gpu_fixture.h"
#include "gpu_unpack.h"
#include "packed_source.h"
#include "store_fakes.h"
#include "store_state.h"

namespace media = dvv::media;
namespace gpu = dvv::gpu;
using dvv::Errc;
using dvv::syncWait;
using media::BindingKind;
using media::Domain;
using media::Fidelity;
using media::FormatId;
using media::FrameDesc;
using media::FrameKey;
using media::FrameStore;
using media::HOST_PLANES;
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
        REQUIRE_MESSAGE(made.has_value(),
                        (made.has_value() ? std::string{} : made.error().describe()));
        store = std::move(*made);
        auto attached = store->attach(std::make_shared<PackedSource>(desc, frames, SEED));
        REQUIRE_MESSAGE(attached.has_value(),
                        (attached.has_value() ? std::string{} : attached.error().describe()));
        origin = *attached;
        REQUIRE(store->stagingDomain().has_value());
        REQUIRE(store->deviceDomain().has_value());
        staging = store->stagingDomain().value_or(Domain{});
        device = store->deviceDomain().value_or(Domain{});
    }

    [[nodiscard]] dvv::Result<media::OriginalRead> read(std::int64_t index, BindingKind binding,
                                                        Domain domain, dvv::IExecutor& resume_on,
                                                        bool require_exact = false) const {
        const media::OriginalRequest request{.binding = binding,
                                             .domain = domain,
                                             .fidelity = require_exact
                                                             ? media::OriginalFidelity::Exact
                                                             : media::OriginalFidelity::Lossless};
        return syncWait(store->acquireOriginal({origin, index}, request, resume_on));
    }

    [[nodiscard]] ReadLease take(std::int64_t index, const ViewSpec& want,
                                 dvv::IExecutor& resume_on) const {
        dvv::Result<ReadLease> lease = syncWait(store->acquire({origin, index}, want, resume_on));
        REQUIRE_MESSAGE(lease.has_value(),
                        (lease.has_value() ? std::string{} : lease.error().describe()));
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
    const auto* planes = lease.planes();
    REQUIRE(planes != nullptr);
    return *planes;
}

// An adopted original in a DeviceOnly buffer of exactly the frame's bytes (no unpack padding). A
// test double, not managed.
class UnpaddedBufferFrame final : public media::IFrame {
public:
    UnpaddedBufferFrame(FrameDesc desc, ViewSpec view, std::shared_ptr<gpu::GpuBuffer> buffer)
        : desc_(desc), view_(view), buffer_(std::move(buffer)) {}
    [[nodiscard]] const FrameDesc& desc() const noexcept override { return desc_; }
    [[nodiscard]] std::span<const ViewSpec> nativeViews() const noexcept override {
        return {&view_, 1};
    }
    [[nodiscard]] media::Binding bind(const ViewSpec& native) const override {
        if (native != view_) {
            return HostPlanesB{};
        }
        return media::vulkan::bufferBinding(buffer_);
    }
    [[nodiscard]] std::size_t bytes() const noexcept override {
        return static_cast<std::size_t>(buffer_->size());
    }

private:
    FrameDesc desc_;
    ViewSpec view_;
    std::shared_ptr<gpu::GpuBuffer> buffer_;
};

// Forwards every member to the wrapped converter and awaits a gate before running it.
class GatedConverter final : public media::detail::Converter {
public:
    GatedConverter(std::unique_ptr<media::detail::Converter> wrapped, media::test::Gate& gate)
        : wrapped_(std::move(wrapped)), gate_(&gate) {}
    [[nodiscard]] std::string_view name() const noexcept override { return wrapped_->name(); }
    [[nodiscard]] media::detail::StateKey from() const noexcept override {
        return wrapped_->from();
    }
    [[nodiscard]] media::detail::StateKey to() const noexcept override { return wrapped_->to(); }
    [[nodiscard]] LayoutPolicy outputLayout() const noexcept override {
        return wrapped_->outputLayout();
    }
    [[nodiscard]] Fidelity fidelity() const noexcept override { return wrapped_->fidelity(); }
    [[nodiscard]] double cost(const FrameDesc& desc) const noexcept override {
        return wrapped_->cost(desc);
    }
    [[nodiscard]] std::size_t outputBytes(const FrameDesc& desc) const noexcept override {
        return wrapped_->outputBytes(desc);
    }
    [[nodiscard]] dvv::Task<dvv::Result<std::shared_ptr<media::IFrame>>> run(
        media::detail::ConvertInput input, media::detail::ConvertContext context) const override {
        co_await gate_->wait(context.cpu);
        co_return co_await wrapped_->run(input,
                                         context);  // copied: ConvertInput is trivially copyable
    }

private:
    std::unique_ptr<media::detail::Converter> wrapped_;
    media::test::Gate* gate_;
};

// The view a converter makes: its target state with the frame's colour (the display colour for
// the display state), layout Any, in the tier of its memory kind.
ViewSpec viewMadeBy(const media::detail::Converter& converter, const FrameDesc& desc,
                    const FrameStore& store) {
    const media::detail::StateKey to = converter.to();
    std::optional<Domain> domain = store.hostDomain();
    if (to.kind == media::MemKind::HostVisible) {
        domain = store.stagingDomain();
    } else if (to.kind == media::MemKind::DeviceLocal) {
        domain = store.deviceDomain();
    }
    REQUIRE(domain.has_value());
    if (to.display) {
        return media::displayView(domain.value_or(Domain{}));
    }
    return ViewSpec{to.unpacked ? media::unpackedFormat(desc.format) : desc.format,
                    desc.colour,
                    LayoutPolicy::Any,
                    to.binding,
                    domain.value_or(Domain{}),
                    0};
}

}  // namespace

TEST_CASE_FIXTURE(GpuTest, "store gpu: planes via the GPU equal the CPU reference") {
    for (const std::string_view name : FORMATS) {
        SUBCASE(std::string(name).c_str()) {
            const FrameDesc desc{.format = idOf(name), .colour = 0, .width = 34, .height = 17};
            const GpuStore test(*this, desc, 3);
            const auto read = test.read(2, HOST_PLANES, test.staging, executor());
            REQUIRE_MESSAGE(read.has_value(),
                            (read.has_value() ? std::string{} : read.error().describe()));
            CHECK(read->lease().fidelity() == Fidelity::Lossless);
            CHECK(read->lease().view().format == media::unpackedFormat(desc.format));
            const auto expected = packedPlanes(desc.format, desc.width, desc.height, SEED, 2);
            REQUIRE_MESSAGE(expected.has_value(),
                            (expected.has_value() ? std::string{} : expected.error().describe()));
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
            const auto read = test.read(2, HOST_PLANES, test.staging, executor(), true);
            REQUIRE_MESSAGE(read.has_value(),
                            (read.has_value() ? std::string{} : read.error().describe()));
            CHECK(read->lease().fidelity() == Fidelity::Exact);
            CHECK(read->lease().view().format == desc.format);
            const auto expected = packedOriginal(desc.format, desc.width, desc.height, SEED, 2);
            REQUIRE_MESSAGE(expected.has_value(),
                            (expected.has_value() ? std::string{} : expected.error().describe()));
            CHECK(concatenated(hostPlanesOf(read->lease())) == *expected);
            const auto stats = test.store->stats();
            CHECK(runsOf(stats, "Upload") == 1);
            CHECK(runsOf(stats, "Readback") == 1);
            CHECK(runsOf(stats, "GpuUnpack") == 0);
            // The lease keeps its entry: its planes stay valid and equal after the trims.
            test.store->trim(test.staging, 0);
            test.store->trim(test.device, 0);
            CHECK(concatenated(hostPlanesOf(read->lease())) == *expected);
        }
    }
}

TEST_CASE_FIXTURE(GpuTest,
                  "store gpu: HostReadback serves both views, HostUpload only the buffer view") {
    const FrameDesc desc{.format = idOf("v210"), .colour = 0, .width = 34, .height = 17};
    const GpuStore test(*this, desc, 3);
    FrameStore& store = *test.store;

    // 1. The exact read's Readback entry stays leased; the HostUpload entry is evicted.
    const auto exact = test.read(0, HOST_PLANES, test.staging, executor(), true);
    REQUIRE_MESSAGE(exact.has_value(),
                    (exact.has_value() ? std::string{} : exact.error().describe()));
    const auto before = store.stats();
    store.trim(test.staging, 0);
    const auto layout = media::geometry(desc.format, desc.width, desc.height);
    REQUIRE(layout.has_value());
    CHECK(test.tierBytes(test.staging) == layout->bytes);  // only the Readback buffer is left
    {
        const ReadLease buffer = test.take(
            0, media::originalView(desc, media::vulkan::BUFFER, test.staging), executor());
        const auto after = store.stats();
        CHECK(after.hits == before.hits + 1);
        CHECK(runsOf(after, "Readback") == runsOf(before, "Readback"));
        CHECK(runsOf(after, "HostToStaging") == runsOf(before, "HostToStaging"));
        const gpu::GpuBuffer* resolved = media::vulkan::buffer(buffer);
        REQUIRE(resolved != nullptr);
        CHECK(resolved->desc().memory == gpu::MemoryUsage::HostReadback);
    }

    // 2. A new key's device planes go through a HostUpload staging.
    const auto staged = store.stats();
    {
        const ReadLease planes =
            test.take(1, media::planesView(desc, media::vulkan::IMAGE, test.device), executor());
        const auto after = store.stats();
        CHECK(runsOf(after, "HostToStaging") == runsOf(staged, "HostToStaging") + 1);
        const auto views = media::detail::StoreAccess::views(store, {test.origin, 1});
        const ViewSpec upload{desc.format,           desc.colour,  LayoutPolicy::Tight,
                              media::vulkan::BUFFER, test.staging, 0};
        CHECK(std::ranges::any_of(views, [&](const auto& view) { return view.first == upload; }));
    }

    // 3. The host view in the staging domain comes only from Readback, from the device original.
    const auto uploaded = store.stats();
    const ReadLease host =
        test.take(1, media::originalView(desc, HOST_PLANES, test.staging), executor());
    const auto after = store.stats();
    CHECK(runsOf(after, "Readback") == runsOf(uploaded, "Readback") + 1);
    CHECK(runsOf(after, "Upload") == runsOf(uploaded, "Upload"));
    CHECK(runsOf(after, "HostToStaging") == runsOf(uploaded, "HostToStaging"));
    const auto expected = packedOriginal(desc.format, desc.width, desc.height, SEED, 1);
    REQUIRE_MESSAGE(expected.has_value(),
                    (expected.has_value() ? std::string{} : expected.error().describe()));
    CHECK(concatenated(hostPlanesOf(host)) == *expected);
}

TEST_CASE_FIXTURE(GpuTest, "store gpu: a HostReadback staging entry is an Upload source") {
    const FrameDesc desc{.format = idOf("v210"), .colour = 0, .width = 34, .height = 17};
    const GpuStore test(*this, desc, 3);
    FrameStore& store = *test.store;

    // Only the leased Readback entry is left: the HostUpload staging and the device original are
    // evicted, so the cheapest way to the device is Upload from the Readback buffer.
    const auto exact = test.read(0, HOST_PLANES, test.staging, executor(), true);
    REQUIRE_MESSAGE(exact.has_value(),
                    (exact.has_value() ? std::string{} : exact.error().describe()));
    store.trim(test.staging, 0);
    store.trim(test.device, 0);
    REQUIRE(test.tierBytes(test.device) == 0);

    const auto before = store.stats();
    const auto planes = syncWait(store.acquire(
        {test.origin, 0}, media::planesView(desc, media::vulkan::IMAGE, test.device), executor()));
    REQUIRE_MESSAGE(planes.has_value(),
                    (planes.has_value() ? std::string{} : planes.error().describe()));
    const auto uploaded = store.stats();
    CHECK(runsOf(uploaded, "Upload") == runsOf(before, "Upload") + 1);
    CHECK(runsOf(uploaded, "HostToStaging") == runsOf(before, "HostToStaging"));
    CHECK(runsOf(uploaded, "GpuUnpack") == runsOf(before, "GpuUnpack") + 1);

    // The device planes, read back, equal the CPU reference.
    const ReadLease host =
        test.take(0, media::planesView(desc, HOST_PLANES, test.staging), executor());
    const auto after = store.stats();
    CHECK(runsOf(after, "Readback") == runsOf(uploaded, "Readback") + 1);
    CHECK(runsOf(after, "CpuUnpack") == runsOf(uploaded, "CpuUnpack"));
    const auto expected = packedPlanes(desc.format, desc.width, desc.height, SEED, 0);
    REQUIRE_MESSAGE(expected.has_value(),
                    (expected.has_value() ? std::string{} : expected.error().describe()));
    CHECK(concatenated(hostPlanesOf(host)) == *expected);
}

TEST_CASE_FIXTURE(GpuTest,
                  "store gpu: the planner picks the CPU for host targets and the GPU for device "
                  "targets") {
    const FormatId id = idOf("p010le");
    const FrameDesc desc{.format = id, .colour = 0, .width = 34, .height = 17};
    const GpuStore test(*this, desc, 3);

    const auto read = test.read(0, HOST_PLANES, test.store->hostDomain(), executor());
    REQUIRE_MESSAGE(read.has_value(), (read.has_value() ? std::string{} : read.error().describe()));
    auto stats = test.store->stats();
    CHECK(runsOf(stats, "CpuUnpack") == 1);
    CHECK(runsOf(stats, "GpuUnpack") == 0);
    CHECK(runsOf(stats, "Upload") == 0);

    const ReadLease planes =
        test.take(0, media::planesView(desc, media::vulkan::IMAGE, test.device), executor());
    stats = test.store->stats();
    CHECK(runsOf(stats, "GpuUnpack") == 1);
    const media::PixelFormatDesc& format = media::formatDesc(id);
    CHECK(media::vulkan::image(planes, format.componentCount) == nullptr);
    for (std::size_t c = 0; c < format.componentCount; ++c) {
        const gpu::GpuImage* image = media::vulkan::image(planes, c);
        REQUIRE(image != nullptr);
        const auto [w, h] = media::componentSize(id, c, desc.width, desc.height);
        CHECK(gpu::vulkanFormat(*image) == VK_FORMAT_R16_UINT);
        CHECK(image->width() == w);
        CHECK(image->height() == h);
        CHECK(gpu::vulkanLayout(*image) == VK_IMAGE_LAYOUT_GENERAL);
        CHECK(gpu::vulkanView(*image) != VK_NULL_HANDLE);
    }
}

TEST_CASE_FIXTURE(GpuTest, "store gpu: a lease resolves its images and its buffer") {
    const FormatId id = idOf("yuv420p10le");
    const FrameDesc desc{.format = id, .colour = 0, .width = 34, .height = 17};
    const GpuStore test(*this, desc, 3);
    const media::PixelFormatDesc& format = media::formatDesc(id);
    const std::size_t plane_count = format.componentCount;
    REQUIRE(plane_count > 0);

    const ViewSpec planes_view = media::planesView(desc, media::vulkan::IMAGE, test.device);
    const ReadLease lease = test.take(0, planes_view, executor());
    for (std::size_t c = 0; c < plane_count; ++c) {
        const gpu::GpuImage* image = media::vulkan::image(lease, c);
        REQUIRE(image != nullptr);
        const auto [w, h] = media::componentSize(id, c, desc.width, desc.height);
        CHECK(image->width() == w);
        CHECK(image->height() == h);
        CHECK(gpu::vulkanFormat(*image) ==
              media::detail::unpackedImageFormat(format.components[c]));
    }
    CHECK(media::vulkan::image(lease, plane_count) == nullptr);
    CHECK(media::vulkan::buffer(lease) == nullptr);

    SUBCASE("a VkBuffer acquire resolves its buffer and no image") {
        const ReadLease original =
            test.take(0, media::originalView(desc, media::vulkan::BUFFER, test.device), executor());
        CHECK(media::vulkan::buffer(original) != nullptr);
        CHECK(media::vulkan::image(original, 0) == nullptr);
    }

    SUBCASE("a pin of the same view resolves the same images") {
        const dvv::Result<media::PinLease> pin =
            syncWait(test.store->pin({test.origin, 0}, planes_view, executor()));
        REQUIRE_MESSAGE(pin.has_value(),
                        (pin.has_value() ? std::string{} : pin.error().describe()));
        if (pin) {
            for (std::size_t c = 0; c < plane_count; ++c) {
                CHECK(media::vulkan::image(*pin, c) == media::vulkan::image(lease, c));
            }
            CHECK(media::vulkan::buffer(*pin) == nullptr);
        }
    }
}

TEST_CASE_FIXTURE(GpuTest, "a GPU binding gives only its own kind") {
    const auto make_image = [this](std::string name) {
        auto made = gpu::createImage(context(), {.width = 4,
                                                 .height = 2,
                                                 .format = VK_FORMAT_R8_UINT,
                                                 .usage = VK_IMAGE_USAGE_STORAGE_BIT,
                                                 .name = std::move(name)});
        REQUIRE_MESSAGE(made.has_value(),
                        (made.has_value() ? std::string{} : made.error().describe()));
        return made ? std::move(*made) : std::shared_ptr<gpu::GpuImage>{};
    };
    auto made = gpu::createBuffer(context(), {.size = 64,
                                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                              .memory = gpu::MemoryUsage::DeviceOnly,
                                              .name = "binding buffer"});
    REQUIRE_MESSAGE(made.has_value(), (made.has_value() ? std::string{} : made.error().describe()));
    const std::shared_ptr<gpu::GpuBuffer> buffer =
        made ? std::move(*made) : std::shared_ptr<gpu::GpuBuffer>{};

    const media::Binding from_buffer = media::vulkan::bufferBinding(buffer);
    CHECK(from_buffer.kind() == media::vulkan::BUFFER);
    CHECK(from_buffer.planes() == nullptr);
    CHECK(media::vulkan::buffer(from_buffer) == buffer.get());
    CHECK(media::vulkan::image(from_buffer, 0) == nullptr);
    CHECK(media::vulkan::imageCount(from_buffer) == 0);

    const std::shared_ptr<gpu::GpuImage> first = make_image("binding image 0");
    std::shared_ptr<gpu::GpuImage> second = make_image("binding image 1");
    {
        const std::array two{first, second};
        const media::Binding from_images = media::vulkan::imageBinding(two);
        CHECK(from_images.kind() == media::vulkan::IMAGE);
        CHECK(media::vulkan::imageCount(from_images) == 2);
        CHECK(media::vulkan::image(from_images, 1) == second.get());
        CHECK(media::vulkan::image(from_images, 2) == nullptr);
        CHECK(media::vulkan::buffer(from_images) == nullptr);
        CHECK(from_images.planes() == nullptr);
    }

    // A copy of the binding keeps its images alive.
    const std::weak_ptr<gpu::GpuImage> weak = second;
    std::optional<media::Binding> copy;
    {
        const std::array two{first, second};
        const media::Binding from_images = media::vulkan::imageBinding(two);
        copy = from_images;
    }
    second.reset();
    CHECK(copy.has_value());
    CHECK_FALSE(weak.expired());
    copy.reset();
    CHECK(weak.expired());

    // Exactly MAX_COMPONENTS images (a four-component format) are kept, the last one readable.
    std::array<std::shared_ptr<gpu::GpuImage>, media::MAX_COMPONENTS> full{};
    for (std::size_t c = 0; c < full.size(); ++c) {
        full[c] = make_image(std::format("binding image {}", c));
    }
    const media::Binding all = media::vulkan::imageBinding(full);
    CHECK(all.kind() == media::vulkan::IMAGE);
    CHECK(media::vulkan::imageCount(all) == media::MAX_COMPONENTS);
    CHECK(media::vulkan::image(all, media::MAX_COMPONENTS - 1) == full.back().get());

    // Refused: the empty host binding, nothing cut silently.
    std::array<std::shared_ptr<gpu::GpuImage>, media::MAX_COMPONENTS + 1> too_many{};
    std::ranges::copy(full, too_many.begin());
    too_many.back() = first;
    const std::array<std::shared_ptr<gpu::GpuImage>, 2> with_null{first, nullptr};
    const std::array refused{media::vulkan::bufferBinding(nullptr), media::vulkan::imageBinding({}),
                             media::vulkan::imageBinding(with_null),
                             media::vulkan::imageBinding(too_many)};
    for (const media::Binding& binding : refused) {
        CHECK(binding.kind() == HOST_PLANES);
        const HostPlanesB* planes = binding.planes();
        REQUIRE(planes != nullptr);
        CHECK(planes->planeCount == 0);
    }
}

TEST_CASE_FIXTURE(GpuTest,
                  "store gpu: a copied binding keeps its images after the lease and the tier are "
                  "gone") {
    const FormatId id = idOf("yuv420p10le");
    const FrameDesc desc{.format = id, .colour = 0, .width = 64, .height = 64};
    const GpuStore test(*this, desc, 1);
    media::Binding held;
    {
        const ReadLease lease =
            test.take(0, media::planesView(desc, media::vulkan::IMAGE, test.device), executor());
        held = lease.binding();
    }
    test.store->trim(test.device, 0);
    CHECK(test.tierBytes(test.device) == 0);
    CHECK(media::vulkan::imageCount(held) == 3);
    const media::PixelFormatDesc& format = media::formatDesc(id);
    for (std::size_t c = 0; c < format.componentCount; ++c) {
        const gpu::GpuImage* image = media::vulkan::image(held, c);
        REQUIRE(image != nullptr);
        const auto [w, h] = media::componentSize(id, c, 64, 64);
        CHECK(image->width() == w);
        CHECK(image->height() == h);
    }
    REQUIRE(context().drain().has_value());
}

TEST_CASE_FIXTURE(GpuTest, "store gpu: device-tier eviction empties the tier") {
    const FormatId id = idOf("yuv420p10le");
    const auto frame = media::geometry(id, 64, 64);
    REQUIRE(frame.has_value());
    const auto unpacked = media::geometry(media::unpackedFormat(id), 64, 64);
    REQUIRE(unpacked.has_value());
    const auto budget = static_cast<std::size_t>(
        3 * (media::detail::unpackBufferBytes(0, frame->bytes) + unpacked->bytes));
    const FrameDesc desc{.format = id, .colour = 0, .width = 64, .height = 64};
    const GpuStore test(*this, desc, 10, budget);

    for (std::int64_t index = 0; index < 10; ++index) {
        {
            const ReadLease planes = test.take(
                index, media::planesView(desc, media::vulkan::IMAGE, test.device), executor());
        }
        CHECK(test.tierBytes(test.device) <= budget);
    }
    CHECK(test.store->stats().evictions > 0);
    test.store->trim(test.device, 0);
    CHECK(test.tierBytes(test.device) == 0);
    REQUIRE(context().drain().has_value());
}

TEST_CASE_FIXTURE(GpuTest, "store gpu: an adopted unpadded buffer fails the unpack") {
    const FormatId id = idOf("yuv420p10le");
    const FrameDesc desc{.format = id, .colour = 0, .width = 33, .height = 17};
    const GpuStore test(*this, desc, 1);
    const auto layout = media::geometry(id, desc.width, desc.height);
    REQUIRE(layout.has_value());
    auto buffer = gpu::createBuffer(context(), {.size = layout->bytes,
                                                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                .memory = gpu::MemoryUsage::DeviceOnly,
                                                .name = "unpadded"});
    REQUIRE_MESSAGE(buffer.has_value(),
                    (buffer.has_value() ? std::string{} : buffer.error().describe()));
    const ViewSpec view{desc.format,           desc.colour, LayoutPolicy::Tight,
                        media::vulkan::BUFFER, test.device, 0};
    auto frame = std::make_shared<UnpaddedBufferFrame>(desc, view, std::move(*buffer));
    const auto adopted = test.store->adopt({test.origin, 0}, std::move(frame));
    REQUIRE_MESSAGE(adopted.has_value(),
                    (adopted.has_value() ? std::string{} : adopted.error().describe()));

    const auto planes = syncWait(test.store->acquire(
        {test.origin, 0}, media::planesView(desc, media::vulkan::IMAGE, test.device), executor()));
    REQUIRE_FALSE(planes.has_value());
    CHECK(planes.error().code == Errc::Internal);
}

TEST_CASE_FIXTURE(GpuTest,
                  "store gpu: a readback staging frame offers host planes only with its read "
                  "scope") {
    const FormatId id = idOf("yuv420p10le");
    const FrameDesc desc{.format = id, .colour = 0, .width = 33, .height = 17};
    const GpuStore test(*this, desc, 1);
    const auto layout = media::geometry(id, desc.width, desc.height);
    REQUIRE(layout.has_value());
    const media::FrameGeometry frame_layout = *layout;
    auto made = gpu::createBuffer(context(), {.size = frame_layout.bytes,
                                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                              .memory = gpu::MemoryUsage::HostReadback,
                                              .name = "readback"});
    REQUIRE_MESSAGE(made.has_value(), (made.has_value() ? std::string{} : made.error().describe()));
    std::shared_ptr<gpu::GpuBuffer> buffer = std::move(*made);
    {
        // A byte pattern, so the planes compare against known bytes.
        gpu::MappedWrite write = buffer->mapWrite();
        const std::span<std::byte> bytes = write.bytes();
        REQUIRE(bytes.size() >= frame_layout.bytes);
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<std::byte>(((i * 7U) + 1U) & 0xFFU);
        }
        REQUIRE(write.flush().has_value());
    }

    // Without a scope: the VkBuffer view only, no empty planes.
    {
        const auto frame = media::detail::StoreAccess::makeFrame<media::StagingFrame>(
            *test.store, desc, desc.format, test.staging, buffer, frame_layout, nullptr);
        REQUIRE(frame->nativeViews().size() == 1);
        CHECK(frame->nativeViews()[0].binding == media::vulkan::BUFFER);
    }

    // With the scope: HostPlanes and VkBuffer, the planes inside the scope's bytes, valid after
    // the buffer's other holders dropped.
    auto read = buffer->mapRead();
    REQUIRE_MESSAGE(read.has_value(), (read.has_value() ? std::string{} : read.error().describe()));
    const std::span<const std::byte> scope = read->bytes();
    REQUIRE(scope.size() >= frame_layout.bytes);
    const std::byte* base = scope.data();
    const std::vector<std::byte> expected(scope.begin(), scope.end());
    auto frame = media::detail::StoreAccess::makeFrame<media::StagingFrame>(
        *test.store, desc, desc.format, test.staging, buffer, frame_layout,
        std::make_unique<gpu::MappedRead>(std::move(*read)));
    buffer.reset();  // the frame is the buffer's only holder
    const std::span<const ViewSpec> views = frame->nativeViews();
    REQUIRE(views.size() == 2);
    const auto host_view = std::ranges::find(views, HOST_PLANES, &ViewSpec::binding);
    REQUIRE(host_view != views.end());
    CHECK(std::ranges::find(views, media::vulkan::BUFFER, &ViewSpec::binding) != views.end());
    const media::Binding binding = frame->bind(*host_view);
    const HostPlanesB* planes = binding.planes();
    REQUIRE(planes != nullptr);
    REQUIRE(planes->planeCount == frame_layout.planeCount);
    REQUIRE(planes->planeCount > 0);
    CHECK(planes->planes[0].data() ==
          base + static_cast<std::size_t>(frame_layout.planes[0].offset));
    for (std::size_t p = 0; p < planes->planeCount; ++p) {
        const std::span<const std::byte> plane = planes->planes[p];
        const auto start = plane.data() - base;
        REQUIRE(start >= 0);
        REQUIRE(static_cast<std::size_t>(start) + plane.size() <= frame_layout.bytes);
        const auto offset = static_cast<std::size_t>(frame_layout.planes[p].offset);
        CHECK(static_cast<std::size_t>(start) == offset);
        CHECK(std::ranges::equal(plane, std::span(expected).subspan(offset, plane.size())));
    }
    frame.reset();  // the scope is destroyed before the buffer (member order)
}

TEST_CASE_FIXTURE(GpuTest,
                  "store gpu: every converter's outputBytes equals the published frame's "
                  "bytes") {
    const FrameDesc desc{.format = idOf("yuv420p10le"), .colour = 0, .width = 34, .height = 17};
    const GpuStore test(*this, desc, 8);
    const auto& converters = media::detail::StoreAccess::state(*test.store).converters;
    REQUIRE(converters.size() == 8);
    for (std::size_t index = 0; index < converters.size(); ++index) {
        const media::detail::Converter& converter = *converters[index];
        if (converter.name() == "CpuUnpack") {
            continue;  // test_frame_store.cpp checks the host converter
        }
        SUBCASE(std::format("{} {}", index, converter.name()).c_str()) {
            const ViewSpec want = viewMadeBy(converter, desc, *test.store);
            const auto key_index = static_cast<std::int64_t>(index);
            const ReadLease lease = test.take(key_index, want, executor());
            const std::optional<std::size_t> bytes = media::detail::StoreAccess::bytesOf(
                *test.store, {test.origin, key_index}, lease.view());
            CHECK(bytes.has_value());
            CHECK(bytes.value_or(0) == converter.outputBytes(desc));
            CHECK(converter.outputBytes(desc) > 0);
        }
    }
}

TEST_CASE_FIXTURE(GpuTest,
                  "store gpu: every GPU tier is settled after a display acquire, and "
                  "ToDisplay reserves its output") {
    const FrameDesc desc{.format = idOf("yuv420p10le"), .colour = 0, .width = 34, .height = 17};
    auto pool = dvv::ExecutorFactory::makeThreadPool(2);
    REQUIRE(pool.has_value());
    const std::shared_ptr<dvv::ThreadPool> executors = std::move(*pool);
    auto made = media::detail::makeGpuConverters(context().shared_from_this(), executors);
    REQUIRE_MESSAGE(made.has_value(), (made.has_value() ? std::string{} : made.error().describe()));
    std::vector<std::unique_ptr<media::detail::Converter>> converters = std::move(*made);
    media::test::Gate gate;
    const media::detail::Converter* to_display = nullptr;
    for (std::unique_ptr<media::detail::Converter>& converter : converters) {
        if (converter->name() == "ToDisplay") {
            converter = std::make_unique<GatedConverter>(std::move(converter), gate);
            to_display = converter.get();
        }
    }
    REQUIRE(to_display != nullptr);
    auto created = media::detail::StoreAccess::create(
        {.cpu = executors, .io = executors, .gpu = context().shared_from_this()},
        std::move(converters));
    REQUIRE_MESSAGE(created.has_value(),
                    (created.has_value() ? std::string{} : created.error().describe()));
    const std::shared_ptr<FrameStore> store = std::move(*created);
    auto attached = store->attach(std::make_shared<PackedSource>(desc, 1, SEED));
    REQUIRE_MESSAGE(attached.has_value(),
                    (attached.has_value() ? std::string{} : attached.error().describe()));
    REQUIRE(store->deviceDomain().has_value());
    const Domain device = store->deviceDomain().value_or(Domain{});

    bool reached = false;
    std::vector<std::size_t> pending;  // per tier, while ToDisplay waits
    std::jthread opener([&] {
        reached = media::test::waitFor([&] { return gate.waiting() == 1; });
        for (const media::TierStats& tier : store->stats().tiers) {
            pending.push_back(tier.pending);
        }
        gate.open();
    });
    const auto display =
        syncWait(store->acquire({*attached, 0}, media::displayView(device), executor()));
    opener.join();
    REQUIRE_MESSAGE(display.has_value(),
                    (display.has_value() ? std::string{} : display.error().describe()));
    CHECK(reached);
    const std::vector<media::TierStats> tiers = store->stats().tiers;
    REQUIRE(pending.size() == tiers.size());
    for (std::size_t index = 0; index < tiers.size(); ++index) {
        // While ToDisplay ran, only its output was being made; afterwards nothing is.
        const std::size_t expected =
            tiers[index].domain == device ? to_display->outputBytes(desc) : 0;
        CHECK(pending[index] == expected);
        CHECK(tiers[index].pending == 0);
    }
    REQUIRE(media::detail::StoreAccess::checkInvariants(*store).has_value());
}
