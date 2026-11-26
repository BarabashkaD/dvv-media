// shaders/sample_grid.slang against its C++ rule (dvv/media/sample_grid.h): the probe shader
// (tests/shaders/sample_grid_probe.slang) writes sampleOf and blockOf for every pixel into an
// R32_UINT image; this file computes the same with the C++ functions and compares.

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "dvv/gpu/command_recorder.h"
#include "dvv/gpu/compute_pipeline.h"
#include "dvv/gpu/context_vulkan.h"
#include "dvv/gpu/execution_context.h"
#include "dvv/gpu/image.h"
#include "dvv/gpu/resources.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/sample_grid.h"
#include "dvv/media/view.h"
#include "gpu_fixture.h"
#include "media_test_shaders/sample_grid_probe.h"

namespace gpu = dvv::gpu;
namespace media = dvv::media;
using dvv::media::test::GpuTest;

namespace {

// The probe's push constants (ProbePush in sample_grid_probe.slang).
struct ProbePush {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t log2SubX = 0;
    std::uint32_t log2SubY = 0;
};
static_assert(sizeof(ProbePush) == 16);

constexpr std::uint32_t PLANES = 6;  // sample x, y; block x, y, width, height
constexpr std::uint32_t GROUP = 8;
constexpr std::uint32_t FRAME_WIDTH = 37;
constexpr std::uint32_t FRAME_HEIGHT = 23;

}  // namespace

TEST_CASE_FIXTURE(GpuTest, "sample grid: the shader matches the C++ rule") {
    auto pipeline_created = gpu::createComputePipeline(
        context(), media_test_shaders::sampleGridProbe(), sizeof(ProbePush), {.storageImages = 1});
    REQUIRE_MESSAGE(
        pipeline_created.has_value(),
        (pipeline_created.has_value() ? std::string{} : pipeline_created.error().describe()));
    const std::shared_ptr<gpu::ComputePipeline> pipeline = std::move(*pipeline_created);
    const std::array<std::array<std::uint8_t, 2>, 4> subsamplings{{{0, 0}, {1, 0}, {1, 1}, {2, 0}}};
    for (const auto& [sub_x, sub_y] : subsamplings) {
        CAPTURE(sub_x);
        CAPTURE(sub_y);
        const ProbePush push{
            .width = FRAME_WIDTH, .height = FRAME_HEIGHT, .log2SubX = sub_x, .log2SubY = sub_y};
        auto created = gpu::createImage(
            context(), {.width = FRAME_WIDTH,
                        .height = FRAME_HEIGHT * PLANES,
                        .format = VK_FORMAT_R32_UINT,
                        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        .name = "sample grid probe"});
        REQUIRE_MESSAGE(created.has_value(),
                        (created.has_value() ? std::string{} : created.error().describe()));
        const std::shared_ptr<gpu::GpuImage> image = std::move(*created);
        const std::uint64_t bytes = std::uint64_t{FRAME_WIDTH} * FRAME_HEIGHT * PLANES * 4;
        auto buffer_created =
            gpu::createBuffer(context(), {.size = bytes,
                                          .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          .memory = gpu::MemoryUsage::HostReadback,
                                          .name = "sample grid readback"});
        REQUIRE_MESSAGE(
            buffer_created.has_value(),
            (buffer_created.has_value() ? std::string{} : buffer_created.error().describe()));
        const std::shared_ptr<gpu::GpuBuffer> readback = std::move(*buffer_created);
        const std::array<gpu::GpuImage*, 1> bindings{image.get()};
        const auto submitted = submitAndWait([&](gpu::CommandRecorder& recorder) {
            recorder.dispatch(*pipeline, std::as_bytes(std::span(&push, 1)), bindings,
                              gpu::GroupCount{.x = (FRAME_WIDTH + GROUP - 1) / GROUP,
                                              .y = (FRAME_HEIGHT + GROUP - 1) / GROUP});
            recorder.copyImageToBuffer(*image, *readback);
        });
        REQUIRE_MESSAGE(submitted.has_value(),
                        (submitted.has_value() ? std::string{} : submitted.error().describe()));
        const auto read = readback->mapRead();
        REQUIRE_MESSAGE(read.has_value(),
                        (read.has_value() ? std::string{} : read.error().describe()));
        const std::span<const std::byte> mapped = read->bytes();
        REQUIRE(mapped.size() >= bytes);
        std::vector<std::uint32_t> cells(static_cast<std::size_t>(bytes / 4));
        std::memcpy(cells.data(), mapped.data(), static_cast<std::size_t>(bytes));

        const media::ComponentDesc component{.log2SubX = sub_x, .log2SubY = sub_y};
        const dvv::platform::PixelSize size{.width = FRAME_WIDTH, .height = FRAME_HEIGHT};
        const auto cell = [&](std::uint32_t plane, std::uint32_t x, std::uint32_t y) {
            return cells[(std::size_t{plane * FRAME_HEIGHT + y} * FRAME_WIDTH) + x];
        };
        std::size_t mismatches = 0;
        for (std::uint32_t y = 0; y < FRAME_HEIGHT; ++y) {
            for (std::uint32_t x = 0; x < FRAME_WIDTH; ++x) {
                const media::SamplePosition sample = media::sampleOf(component, {x, y});
                const media::PixelBlock block = media::blockOf(component, {x, y}, size);
                const std::array<std::uint32_t, PLANES> expected{
                    sample.x, sample.y, block.x, block.y, block.width, block.height};
                for (std::uint32_t plane = 0; plane < PLANES; ++plane) {
                    if (cell(plane, x, y) != expected[plane]) {
                        if (mismatches < 8) {
                            INFO("plane ", plane, ", x ", x, ", y ", y, ": GPU ", cell(plane, x, y),
                                 ", C++ ", expected[plane]);
                            CHECK(cell(plane, x, y) == expected[plane]);
                        }
                        ++mismatches;
                    }
                }
            }
        }
        CHECK(mismatches == 0);
    }
}
