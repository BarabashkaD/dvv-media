#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "alpha_formats.h"
#include "dvv/base/error.h"
#include "dvv/base/executors.h"
#include "dvv/base/task.h"
#include "dvv/gpu/command_recorder.h"
#include "dvv/gpu/compute_pipeline.h"
#include "dvv/gpu/execution_context.h"
#include "dvv/gpu/image.h"
#include "dvv/gpu/resources.h"
#include "dvv/media/display.h"
#include "dvv/media/display_vulkan.h"
#include "dvv/media/frame.h"
#include "dvv/media/frame_store.h"
#include "dvv/media/frame_vulkan.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/reference.h"
#include "dvv/media/types.h"
#include "gpu_fixture.h"
#include "packed_source.h"
#include "patterns.h"
#include "result_message.h"
#include "store_state.h"
#include "to_display.h"

namespace media = dvv::media;
namespace detail = dvv::media::detail;
namespace gpu = dvv::gpu;

static_assert(media::DISPLAY_IMAGE_FORMAT == VK_FORMAT_R16G16B16A16_SFLOAT);
using dvv::Errc;
using dvv::syncWait;
using media::ColourMatrix;
using media::ColourParams;
using media::ColourRange;
using media::Fidelity;
using media::FormatId;
using media::FrameDesc;
using media::FrameKey;
using media::FrameStore;
using media::OriginId;
using media::Primaries;
using media::ReadLease;
using media::TransferFn;
using media::test::CodeValuesSource;
using media::test::GpuTest;
using media::test::PackedSource;
using media::test::runsOf;

namespace {

constexpr std::uint64_t SEED = 11;
FormatId idOf(std::string_view name) {
    const auto id = media::findFormat(name);
    REQUIRE_MESSAGE(id.has_value(), name);
    return id.value_or(FormatId{0});  // REQUIRE above stops the test case first
}

// The colour sets of the reference test; sets 7 and 8 (index 6, 7) are tone mapped.
struct ColourSet {
    std::string_view name;
    ColourParams params;
};
const std::array<ColourSet, 8>& colourSets() {
    static const std::array<ColourSet, 8> SETS{{
        {"S1", {}},
        {"S2", {.matrix = ColourMatrix::Bt601, .range = ColourRange::Limited}},
        {"S3", {.matrix = ColourMatrix::Bt709, .range = ColourRange::Full}},
        {"S4",
         {.matrix = ColourMatrix::Bt2020Ncl,
          .range = ColourRange::Limited,
          .transfer = TransferFn::Bt1886,
          .primaries = Primaries::Bt2020}},
        {"S5", {.transfer = TransferFn::Linear}},
        {"S6",
         {.matrix = ColourMatrix::Custom,
          .range = ColourRange::Limited,
          .kr = 0.212F,
          .kb = 0.087F}},
        {"S7",
         {.matrix = ColourMatrix::Bt2020Ncl,
          .range = ColourRange::Limited,
          .transfer = TransferFn::Pq,
          .primaries = Primaries::Bt2020}},
        {"S8",
         {.matrix = ColourMatrix::Bt2020Ncl,
          .range = ColourRange::Limited,
          .transfer = TransferFn::Hlg,
          .primaries = Primaries::Bt2020}},
    }};
    return SETS;
}

// A GPU store on the fixture's context. Declared in the test body, so it is destroyed before the
// fixture's context.
struct DisplayStore {
    explicit DisplayStore(GpuTest& test) {
        pool = dvv::ExecutorFactory::makeThreadPool(2).value();
        auto made =
            FrameStore::create({.cpu = pool, .io = pool, .gpu = test.context().shared_from_this()});
        REQUIRE_MESSAGE(made.has_value(),
                        (made.has_value() ? std::string{} : made.error().describe()));
        store = std::move(*made);
        REQUIRE(store->deviceDomain().has_value());
        device = store->deviceDomain().value_or(media::Domain{});
    }
    std::shared_ptr<dvv::ThreadPool> pool;
    std::shared_ptr<FrameStore> store;
    media::Domain device{};
};

// The display image of a lease (REQUIREs one image).
gpu::GpuImage& displayImageOf(const ReadLease& lease) {
    gpu::GpuImage* image = media::vulkan::image(lease, 0);
    REQUIRE(image != nullptr);
    REQUIRE(media::vulkan::image(lease, 1) == nullptr);
    return *image;
}

// The half floats of an RGBA16F image, read back in one submission.
std::vector<std::uint16_t> readHalves(GpuTest& test, gpu::GpuImage& image) {
    const std::uint64_t bytes = std::uint64_t{image.width()} * image.height() * 8;
    auto buffer = gpu::createBuffer(test.context(), {.size = bytes,
                                                     .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                     .memory = gpu::MemoryUsage::HostReadback,
                                                     .name = "display readback"});
    REQUIRE_MESSAGE(buffer.has_value(),
                    (buffer.has_value() ? std::string{} : buffer.error().describe()));
    const std::shared_ptr<gpu::GpuBuffer> readback = std::move(*buffer);
    const auto submitted = test.submitAndWait(
        [&](gpu::CommandRecorder& recorder) { recorder.copyImageToBuffer(image, *readback); });
    REQUIRE_MESSAGE(submitted.has_value(),
                    (submitted.has_value() ? std::string{} : submitted.error().describe()));
    const auto read = readback->mapRead();
    REQUIRE_MESSAGE(read.has_value(), (read.has_value() ? std::string{} : read.error().describe()));
    const std::span<const std::byte> mapped = read->bytes();
    REQUIRE(mapped.size() >= bytes);
    std::vector<std::uint16_t> halves(static_cast<std::size_t>(bytes / 2));
    std::memcpy(halves.data(), mapped.data(), static_cast<std::size_t>(bytes));
    return halves;
}

// The distance from the half nearest to a non-negative value to the next half up.
float halfStep(float value) {
    const std::uint16_t half = media::floatToHalf(value);
    return media::halfToFloat(static_cast<std::uint16_t>(half + 1U)) - media::halfToFloat(half);
}

// The noise code values of frame 0 of a PackedSource.
media::CodeValues noiseCodes(FormatId format, std::uint32_t width, std::uint32_t height) {
    auto values = detail::generate(detail::Pattern::Noise, format, width, height, SEED, 0);
    REQUIRE_MESSAGE(values.has_value(), media::test::messageOf(values));
    return std::move(*values);
}

// The noise codes with the A component (the last, never subsampled) set to 0, the middle code and
// the maximum in the first three pixels of row 0.
media::CodeValues alphaCodes(FormatId format, std::uint32_t width, std::uint32_t height) {
    media::CodeValues values = noiseCodes(format, width, height);
    const media::PixelFormatDesc& d = media::formatDesc(format);
    REQUIRE((d.componentCount == 2 || d.componentCount == 4));
    const media::ComponentDesc& alpha = d.components[d.componentCount - 1];
    REQUIRE(alpha.kind == media::ComponentKind::A);
    std::vector<std::uint16_t>& plane = values.components[d.componentCount - 1];
    REQUIRE(plane.size() >= 3);
    plane[0] = 0;
    plane[1] = static_cast<std::uint16_t>(1U << (alpha.bits - 1U));
    plane[2] = static_cast<std::uint16_t>((1U << alpha.bits) - 1U);
    return values;
}

// Compares the display image of one frame of `values` with the CPU reference; returns the failures
// and keeps the first one's text.
std::size_t compareFrame(const media::CodeValues& values, std::string_view set, bool tone_mapped,
                         const FrameDesc& desc, const std::vector<std::uint16_t>& halves,
                         std::optional<std::string>& first) {
    const FormatId format = values.format;
    const media::PixelFormatDesc& d = media::formatDesc(format);
    const ColourParams resolved = media::resolveColour(desc).params;
    std::array<std::uint32_t, media::MAX_COMPONENTS> grid_width{};
    for (std::size_t k = 0; k < d.componentCount; ++k) {
        grid_width[k] = media::componentSize(format, k, desc.width, desc.height).first;
    }
    std::size_t failures = 0;
    const auto fail = [&](std::uint32_t x, std::uint32_t y, std::size_t channel, float expected,
                          std::uint16_t got) {
        ++failures;
        if (!first) {
            first = std::format(
                "{} {}: ({}, {}) channel {}: reference {} (half 0x{:04X}), GPU "
                "{} (half 0x{:04X})",
                d.name, set, x, y, channel, expected, media::floatToHalf(expected),
                media::halfToFloat(got), got);
        }
    };
    for (std::uint32_t y = 0; y < desc.height; ++y) {
        for (std::uint32_t x = 0; x < desc.width; ++x) {
            std::array<std::uint16_t, media::MAX_COMPONENTS> codes{};
            for (std::size_t k = 0; k < d.componentCount; ++k) {
                const media::ComponentDesc& component = d.components[k];
                const std::size_t at = (std::size_t{y >> component.log2SubY} * grid_width[k]) +
                                       (x >> component.log2SubX);
                codes[k] = values.components[k][at];
            }
            const std::array<float, 4> reference = media::toDisplayReference(
                format, resolved, std::span<const std::uint16_t>(codes).first(d.componentCount));
            // PQ/HLG run float32 transcendentals (pow as exp2(y * log2 x), larger error on the GPU)
            // through steep curves (HLG's OOTF also mixes the channels through Ys). So the
            // tone-mapped tolerance is relative to the brightest channel, plus one half step of
            // the reference for the half-float store (1e-3 alone is about one half step).
            const float brightest = std::max({reference[0], reference[1], reference[2]});
            const std::size_t pixel = (std::size_t{y} * desc.width + x) * 4;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                std::uint16_t got = halves[pixel + channel];
                if (got == 0x8000U) {
                    got = 0;  // -0 and +0 are the same black
                }
                const float expected = reference[channel];
                const float gpu_value = media::halfToFloat(got);
                const int srgb_difference = std::abs(int{media::encodeSrgb8(gpu_value)} -
                                                     int{media::encodeSrgb8(expected)});
                bool ok = srgb_difference <= 1 && (got & 0x8000U) == 0;
                if (tone_mapped) {
                    const float tolerance = std::max(1e-3F * brightest, 1e-5F) + halfStep(expected);
                    ok = ok && std::fabs(gpu_value - expected) <= tolerance;
                } else {
                    ok = ok && std::abs(int{got} - int{media::floatToHalf(expected)}) <= 2;
                }
                if (!ok) {
                    fail(x, y, channel, expected, got);
                }
            }
            // The frame's alpha, straight and full range (1 without A). 0 and 1 are exact (no A,
            // A = 0, A = max); a value in between is within one half step.
            const std::uint16_t alpha_expected = media::floatToHalf(reference[3]);
            const int alpha_tolerance = (reference[3] == 0.0F || reference[3] == 1.0F) ? 0 : 1;
            if (std::abs(int{halves[pixel + 3]} - int{alpha_expected}) > alpha_tolerance) {
                fail(x, y, 3, reference[3], halves[pixel + 3]);
            }
        }
    }
    return failures;
}

std::shared_ptr<gpu::GpuImage> makeImage(gpu::ExecutionContext& context, std::uint32_t width,
                                         std::uint32_t height, VkFormat format, std::string name) {
    auto image = gpu::createImage(
        context, {.width = width,
                  .height = height,
                  .format = format,
                  .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                  .name = std::move(name)});
    REQUIRE_MESSAGE(image.has_value(),
                    (image.has_value() ? std::string{} : image.error().describe()));
    return std::move(*image);
}

// For every colour set: attaches make_source(desc) for a frame of the size and format of `values`,
// reads frame 0 through the display view and compares it with the CPU reference of `values`.
template <typename MakeSource>
void checkColourSets(GpuTest& gpu_test, const media::CodeValues& values, MakeSource make_source) {
    const DisplayStore test(gpu_test);
    const std::string& name = media::formatDesc(values.format).name;
    std::vector<OriginId> origins;
    for (std::size_t s = 0; s < colourSets().size(); ++s) {
        const ColourSet& set = colourSets()[s];
        CAPTURE(set.name);
        const FrameDesc desc{.format = values.format,
                             .colour = media::internColour(set.params),
                             .width = values.width,
                             .height = values.height};
        const auto attached = test.store->attach(make_source(desc));
        REQUIRE_MESSAGE(attached.has_value(), media::test::messageOf(attached));
        origins.push_back(*attached);
        std::vector<std::uint16_t> halves;
        {
            const dvv::Result<ReadLease> lease = syncWait(test.store->acquire(
                {*attached, 0}, media::displayView(test.device), gpu_test.executor()));
            REQUIRE_MESSAGE(lease.has_value(), media::test::messageOf(lease));
            CHECK(lease->fidelity() == Fidelity::Derived);
            gpu::GpuImage& image = displayImageOf(*lease);
            REQUIRE(image.width() == values.width);
            REQUIRE(image.height() == values.height);
            halves = readHalves(gpu_test, image);
        }
        std::optional<std::string> first;
        const std::size_t failures = compareFrame(values, set.name, s >= 6, desc, halves, first);
        if (failures > 0) {
            FAIL_CHECK(std::format("{} {}: {} mismatches; first: {}", name, set.name, failures,
                                   first.value_or("?")));
        }
    }
    for (const OriginId origin : origins) {
        test.store->detach(origin);
    }
    REQUIRE(detail::StoreAccess::checkInvariants(*test.store).has_value());
}
}  // namespace

TEST_CASE_FIXTURE(GpuTest, "ToDisplay matches the CPU reference for every format and colour set") {
    for (const FormatId format : media::catalogue()) {
        const media::PixelFormatDesc& d = media::formatDesc(format);
        SUBCASE(d.name.c_str()) {
            const std::uint32_t multiple = std::max<std::uint32_t>(d.widthMultiple, 1);
            const std::uint32_t width = (33 + multiple - 1) / multiple * multiple;
            checkColourSets(*this, noiseCodes(format, width, 17), [](const FrameDesc& desc) {
                return std::make_shared<PackedSource>(desc, 1, SEED);
            });
        }
    }
}

TEST_CASE_FIXTURE(GpuTest, "ToDisplay: alpha of custom Y+A and YUV+A formats") {
    for (const std::string_view text : {media::test::YA8_JSON, media::test::YUVA10_JSON}) {
        const dvv::Result<FormatId> format = media::test::loadFormat(text);
        REQUIRE_MESSAGE(format.has_value(), media::test::messageOf(format));
        SUBCASE(media::formatDesc(*format).name.c_str()) {
            const media::CodeValues values = alphaCodes(*format, 33, 17);
            checkColourSets(*this, values, [&values](const FrameDesc& desc) {
                return std::make_shared<CodeValuesSource>(desc, values);
            });
        }
    }
}

TEST_CASE_FIXTURE(GpuTest, "ToDisplay: BT.2020 primaries give the BT.709 output (no gamut map)") {
    const DisplayStore test(*this);
    const FormatId format = idOf("yuv420p10le");
    const auto render = [&](TransferFn transfer, Primaries primaries) {
        const FrameDesc desc{.format = format,
                             .colour = media::internColour({.matrix = ColourMatrix::Bt2020Ncl,
                                                            .range = ColourRange::Limited,
                                                            .transfer = transfer,
                                                            .primaries = primaries,
                                                            .kr = 0,
                                                            .kb = 0}),
                             .width = 32,
                             .height = 16};
        const auto attached = test.store->attach(std::make_shared<PackedSource>(desc, 1, SEED));
        REQUIRE_MESSAGE(attached.has_value(),
                        (attached.has_value() ? std::string{} : attached.error().describe()));
        std::vector<std::uint16_t> halves;
        {
            const dvv::Result<ReadLease> lease = syncWait(
                test.store->acquire({*attached, 0}, media::displayView(test.device), executor()));
            REQUIRE_MESSAGE(lease.has_value(),
                            (lease.has_value() ? std::string{} : lease.error().describe()));
            halves = readHalves(*this, displayImageOf(*lease));
        }
        test.store->detach(*attached);
        return halves;
    };
    for (const TransferFn transfer : {TransferFn::Bt1886, TransferFn::Pq, TransferFn::Hlg}) {
        CAPTURE(static_cast<int>(transfer));
        CHECK(render(transfer, Primaries::Bt2020) == render(transfer, Primaries::Bt709));
    }
    REQUIRE(detail::StoreAccess::checkInvariants(*test.store).has_value());
}

TEST_CASE_FIXTURE(GpuTest, "the display path runs every converter once and then hits") {
    const DisplayStore test(*this);
    const FrameDesc desc{.format = idOf("yuv420p10le"),
                         .colour = media::internColour(
                             {.matrix = ColourMatrix::Bt709, .range = ColourRange::Limited}),
                         .width = 33,
                         .height = 17};
    const OriginId origin =
        test.store->attach(std::make_shared<PackedSource>(desc, 1, SEED)).value();
    const FrameKey key{origin, 0};
    {
        const dvv::Result<ReadLease> lease =
            syncWait(test.store->acquire(key, media::displayView(test.device), executor()));
        REQUIRE_MESSAGE(lease.has_value(),
                        (lease.has_value() ? std::string{} : lease.error().describe()));
        CHECK(lease->fidelity() == Fidelity::Derived);
        CHECK(lease->view().format == media::displayFormat());
        CHECK(lease->view().colour == media::displayColour());
        const gpu::GpuImage& image = displayImageOf(*lease);
        CHECK(gpu::vulkanFormat(image) == media::DISPLAY_IMAGE_FORMAT);
        CHECK(image.width() == 33);
        CHECK(image.height() == 17);
        CHECK(gpu::vulkanLayout(image) == VK_IMAGE_LAYOUT_GENERAL);
        CHECK(gpu::vulkanView(image) != VK_NULL_HANDLE);
    }
    const auto counts = [&] {
        const media::StoreStats stats = test.store->stats();
        return std::array<std::uint64_t, 5>{runsOf(stats, "Produce"),
                                            runsOf(stats, "HostToStaging"), runsOf(stats, "Upload"),
                                            runsOf(stats, "GpuUnpack"), runsOf(stats, "ToDisplay")};
    };
    CHECK(counts() == std::array<std::uint64_t, 5>{1, 1, 1, 1, 1});
    const std::uint64_t hits = test.store->stats().hits;
    {
        const dvv::Result<ReadLease> again =
            syncWait(test.store->acquire(key, media::displayView(test.device), executor()));
        REQUIRE_MESSAGE(again.has_value(),
                        (again.has_value() ? std::string{} : again.error().describe()));
    }
    CHECK(test.store->stats().hits == hits + 1);
    CHECK(counts() == std::array<std::uint64_t, 5>{1, 1, 1, 1, 1});
    REQUIRE(detail::StoreAccess::checkInvariants(*test.store).has_value());
}

TEST_CASE_FIXTURE(GpuTest, "the ToDisplay pipeline binds five storage images and 96 bytes") {
    const auto pipeline = detail::createToDisplayPipeline(context());
    REQUIRE_MESSAGE(pipeline.has_value(),
                    (pipeline.has_value() ? std::string{} : pipeline.error().describe()));
    CHECK((*pipeline)->bindings().storageImages == 5);
    CHECK((*pipeline)->pushConstantSize() == 96);
    const auto dummies = detail::createToDisplayDummies(context());
    REQUIRE_MESSAGE(dummies.has_value(),
                    (dummies.has_value() ? std::string{} : dummies.error().describe()));
    for (std::size_t i = 0; i < dummies->images.size(); ++i) {
        CAPTURE(i);
        const auto& image = dummies->images[i];
        REQUIRE(image != nullptr);
        CHECK(image->width() == 1);
        CHECK(image->height() == 1);
        CHECK(gpu::vulkanFormat(*image) == VK_FORMAT_R16_UINT);
        for (std::size_t j = 0; j < i; ++j) {
            CHECK(image != dummies->images[j]);
        }
    }
}

TEST_CASE_FIXTURE(GpuTest, "recordToDisplay checks its images") {
    const FormatId format = idOf("yuv420p");
    const auto pipeline = detail::createToDisplayPipeline(context());
    REQUIRE_MESSAGE(pipeline.has_value(),
                    (pipeline.has_value() ? std::string{} : pipeline.error().describe()));
    const auto dummies = detail::createToDisplayDummies(context());
    REQUIRE_MESSAGE(dummies.has_value(),
                    (dummies.has_value() ? std::string{} : dummies.error().describe()));
    const FrameDesc desc{.format = format, .colour = 0, .width = 4, .height = 4};
    const auto push = detail::toDisplayPush(format, media::resolveColour(desc).params, 4, 4);
    REQUIRE_MESSAGE(push.has_value(), (push.has_value() ? std::string{} : push.error().describe()));
    const auto y = makeImage(context(), 4, 4, VK_FORMAT_R8_UINT, "Y");
    const auto u = makeImage(context(), 2, 2, VK_FORMAT_R8_UINT, "U");
    const auto output = makeImage(context(), 4, 4, media::DISPLAY_IMAGE_FORMAT, "display");
    const auto rgba8 = makeImage(context(), 4, 4, VK_FORMAT_R8G8B8A8_UNORM, "rgba8");
    const auto v = makeImage(context(), 2, 2, VK_FORMAT_R8_UINT, "V");

    const std::array<gpu::GpuImage*, 2> two{y.get(), u.get()};
    const std::array<gpu::GpuImage*, 3> three{y.get(), u.get(), v.get()};
    std::optional<dvv::Result<void>> too_few;
    std::optional<dvv::Result<void>> wrong_output;
    const auto submitted = submitAndWait([&](gpu::CommandRecorder& recorder) {
        too_few =
            detail::recordToDisplay(recorder, **pipeline, *push, format, two, *output, *dummies);
        wrong_output =
            detail::recordToDisplay(recorder, **pipeline, *push, format, three, *rgba8, *dummies);
    });
    REQUIRE_MESSAGE(submitted.has_value(),
                    (submitted.has_value() ? std::string{} : submitted.error().describe()));
    REQUIRE(too_few.has_value());
    REQUIRE(wrong_output.has_value());
    const dvv::Result<void> few = too_few.value_or(dvv::Result<void>{});
    REQUIRE_FALSE(few.has_value());
    CHECK(few.error().code == Errc::Internal);
    const dvv::Result<void> output_result = wrong_output.value_or(dvv::Result<void>{});
    REQUIRE_FALSE(output_result.has_value());
    CHECK(output_result.error().code == Errc::Internal);
}
