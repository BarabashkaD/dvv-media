#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "dvv/base/error.h"
#include "dvv/gpu/command_recorder.h"
#include "dvv/gpu/compute_pipeline.h"
#include "dvv/gpu/resources.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/reference.h"
#include "dvv/media/types.h"
#include "format_table.h"
#include "gpu_fixture.h"
#include "gpu_unpack.h"
#include "patterns.h"

namespace media = dvv::media;
namespace detail = dvv::media::detail;
namespace gpu = dvv::gpu;
using dvv::media::test::GpuTest;

namespace {

media::FormatId idOf(std::string_view name) {
    const auto id = media::findFormat(name);
    REQUIRE_MESSAGE(id.has_value(), name);
    return id.value_or(media::FormatId{0});  // REQUIRE above stops the test case first
}

std::shared_ptr<gpu::GpuBuffer> makeBuffer(gpu::ExecutionContext& context, std::uint64_t size,
                                           VkBufferUsageFlags usage, gpu::MemoryUsage memory,
                                           std::string name) {
    auto buffer = context.createBuffer(
        {.size = size, .usage = usage, .memory = memory, .name = std::move(name)});
    REQUIRE_MESSAGE(buffer.has_value(), buffer.error().describe());
    return std::move(*buffer);
}

std::shared_ptr<gpu::GpuImage> makeImage(gpu::ExecutionContext& context, std::uint32_t width,
                                         std::uint32_t height, VkFormat format, std::string name) {
    auto image =
        context.createImage({.width = width,
                             .height = height,
                             .format = format,
                             .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             .name = std::move(name)});
    REQUIRE_MESSAGE(image.has_value(), image.error().describe());
    return std::move(*image);
}

// A DeviceOnly buffer the unpack can read.
std::shared_ptr<gpu::GpuBuffer> frameBuffer(gpu::ExecutionContext& context, std::uint64_t size) {
    return makeBuffer(context, size,
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      gpu::MemoryUsage::DeviceOnly, "frame");
}

std::shared_ptr<gpu::ComputePipeline> unpackPipeline(gpu::ExecutionContext& context) {
    auto pipeline = detail::createUnpackPipeline(context);
    REQUIRE_MESSAGE(pipeline.has_value(), pipeline.error().describe());
    return std::move(*pipeline);
}

// One frame of a format: the generated code values, the buffers and the images of its unpack.
struct UnpackCase {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t seed = 0;
    std::uint64_t offset = 0;
    media::CodeValues values{};
    std::shared_ptr<gpu::GpuBuffer> staging{};
    std::shared_ptr<gpu::GpuBuffer> device{};
    std::vector<std::shared_ptr<gpu::GpuImage>> images{};
    std::vector<gpu::GpuImage*> imagePointers{};
    std::vector<std::shared_ptr<gpu::GpuBuffer>> readbacks{};
};

// The frame sizes of the bit-exactness test; a width multiple (v210: 2) rounds the widths up and
// drops 1x1.
std::vector<std::pair<std::uint32_t, std::uint32_t>> frameSizes(
    const media::PixelFormatDescriptor& format) {
    static constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 5> SIZES{
        {{1, 1}, {2, 2}, {33, 17}, {49, 3}, {64, 64}}};
    const std::uint32_t multiple = format.widthMultiple;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> sizes;
    for (const auto& [width, height] : SIZES) {
        if (multiple > 1 && width == 1 && height == 1) {
            continue;
        }
        sizes.emplace_back((width + multiple - 1) / multiple * multiple, height);
    }
    return sizes;
}

UnpackCase prepareCase(gpu::ExecutionContext& context, media::FormatId id, std::uint32_t width,
                       std::uint32_t height, std::uint64_t seed, std::uint64_t offset) {
    const media::PixelFormatDescriptor& format = media::descriptor(id);
    UnpackCase unpack{.width = width, .height = height, .seed = seed, .offset = offset};
    auto values = detail::generate(detail::Pattern::Noise, id, width, height, seed, 0);
    REQUIRE_MESSAGE(values.has_value(), values.error().describe());
    unpack.values = std::move(*values);
    const auto bytes = media::pack(unpack.values);
    REQUIRE_MESSAGE(bytes.has_value(), bytes.error().describe());
    const auto frame = media::geometry(id, width, height);
    REQUIRE_MESSAGE(frame.has_value(), frame.error().describe());
    REQUIRE(bytes->size() == frame->bytes);
    // The GPU result is compared with these values; the CPU reference reads them back first.
    const auto reference = media::unpack(id, width, height, *bytes);
    REQUIRE_MESSAGE(reference.has_value(), reference.error().describe());
    REQUIRE(*reference == unpack.values);

    const std::uint64_t size = detail::unpackBufferBytes(offset, frame->bytes);
    unpack.staging = makeBuffer(context, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                gpu::MemoryUsage::HostUpload, "staging");
    const std::span<std::byte> mapped = unpack.staging->mapped();
    REQUIRE(mapped.size() >= size);
    std::memset(mapped.data(), 0, size);
    std::memcpy(mapped.data() + offset, bytes->data(), bytes->size());
    REQUIRE(unpack.staging->flush().has_value());
    unpack.device = frameBuffer(context, size);

    for (std::size_t c = 0; c < format.componentCount; ++c) {
        const auto [w, h] = media::componentSize(id, c, width, height);
        const VkFormat image_format = detail::unpackedImageFormat(format.components[c]);
        unpack.images.push_back(makeImage(context, w, h, image_format, "component"));
        unpack.imagePointers.push_back(unpack.images.back().get());
        const std::uint64_t texel_bytes = image_format == VK_FORMAT_R8_UINT ? 1 : 2;
        unpack.readbacks.push_back(makeBuffer(context, std::uint64_t{w} * h * texel_bytes,
                                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                              gpu::MemoryUsage::HostReadback, "readback"));
    }
    return unpack;
}

// Compares the read-back images with the code values; FAIL_CHECK on the first mismatch.
void compareCase(const media::PixelFormatDescriptor& format, const UnpackCase& unpack) {
    for (std::size_t c = 0; c < unpack.images.size(); ++c) {
        const gpu::GpuImage& image = *unpack.images[c];
        gpu::GpuBuffer& readback = *unpack.readbacks[c];
        REQUIRE(readback.invalidate().has_value());
        const std::span<const std::byte> texels = readback.mapped();
        const bool wide = image.format() == VK_FORMAT_R16_UINT;
        const std::vector<std::uint16_t>& expected = unpack.values.components[c];
        REQUIRE(expected.size() == std::size_t{image.width()} * image.height());
        for (std::uint32_t y = 0; y < image.height(); ++y) {
            for (std::uint32_t x = 0; x < image.width(); ++x) {
                const std::size_t i = std::size_t{y} * image.width() + x;
                const std::uint32_t got =
                    wide ? (std::to_integer<std::uint32_t>(texels[2 * i]) |
                            (std::to_integer<std::uint32_t>(texels[(2 * i) + 1]) << 8U))
                         : std::to_integer<std::uint32_t>(texels[i]);
                if (got != expected[i]) {
                    FAIL_CHECK(
                        std::format("{} {}x{} seed {}: component {} at ({}, {}): expected "
                                    "{}, got {}",
                                    format.name, unpack.width, unpack.height, unpack.seed, c, x, y,
                                    expected[i], got));
                    return;
                }
            }
        }
    }
}

// Unpacks noise frames of every size of frameSizes() on the GPU and compares them bit for bit.
void checkFormat(GpuTest& test, media::FormatId id) {
    const media::PixelFormatDescriptor& format = media::descriptor(id);
    const auto pipeline = unpackPipeline(test.context());
    // Seed 1 at offset 0; seed 2 at offset 260 (4-aligned, not 16-aligned).
    std::vector<UnpackCase> cases;
    for (const auto& [width, height] : frameSizes(format)) {
        cases.push_back(prepareCase(test.context(), id, width, height, 1, 0));
        cases.push_back(prepareCase(test.context(), id, width, height, 2, 260));
    }
    std::vector<dvv::Result<void>> recorded;
    const auto submitted = test.submitAndWait([&](gpu::CommandRecorder& recorder) {
        for (UnpackCase& unpack : cases) {
            recorder.copyBuffer(*unpack.staging, *unpack.device);
            recorded.push_back(
                detail::recordUnpack(recorder, *pipeline, *unpack.device, unpack.offset,
                                     {.format = id, .width = unpack.width, .height = unpack.height},
                                     unpack.imagePointers));
            for (std::size_t c = 0; c < unpack.images.size(); ++c) {
                recorder.copyImageToBuffer(*unpack.images[c], *unpack.readbacks[c]);
            }
        }
    });
    REQUIRE_MESSAGE(submitted.has_value(), submitted.error().describe());
    REQUIRE(recorded.size() == cases.size());
    for (const dvv::Result<void>& result : recorded) {
        REQUIRE_MESSAGE(result.has_value(), result.error().describe());
    }
    for (const UnpackCase& unpack : cases) {
        compareCase(format, unpack);
    }
}

media::ComponentDesc component(media::ComponentKind kind, std::uint8_t bits,
                               std::uint8_t log2_sub_x = 0, std::uint8_t log2_sub_y = 0) {
    return {.kind = kind, .bits = bits, .log2SubX = log2_sub_x, .log2SubY = log2_sub_y};
}

media::SampleSlot slot(std::uint8_t component, std::uint8_t index, std::uint8_t word,
                       std::uint8_t shift) {
    return {.component = component, .index = index, .word = word, .shift = shift};
}

// A plane with these slots (rowAlign 1).
media::PlaneDesc plane(std::uint8_t group_pixels, std::uint8_t group_bytes, std::uint8_t word_bytes,
                       media::ByteOrder order, std::uint8_t log2_sub_y,
                       std::initializer_list<media::SampleSlot> slots) {
    media::PlaneDesc result{.groupPixels = group_pixels,
                            .groupBytes = group_bytes,
                            .wordBytes = word_bytes,
                            .order = order,
                            .log2SubY = log2_sub_y,
                            .slotCount = static_cast<std::uint8_t>(slots.size())};
    std::ranges::copy(slots, result.slots.begin());
    return result;
}

// Custom formats that use the shader paths no catalogue format reaches: 4-byte big-endian words
// and 4:1:x chroma (log2SubX 2).
std::vector<media::PixelFormatDescriptor> customFormats() {
    using media::ByteOrder;
    using media::ComponentKind;
    std::vector<media::PixelFormatDescriptor> formats;

    // 10-bit RGB in one big-endian 32-bit word per pixel (R in bits 20..29, B in 0..9).
    media::PixelFormatDescriptor rgb{.name = "test-rgb30-be32",
                                     .model = media::ColourModel::Rgb,
                                     .componentCount = 3,
                                     .planeCount = 1};
    rgb.components = {component(ComponentKind::R, 10), component(ComponentKind::G, 10),
                      component(ComponentKind::B, 10)};
    rgb.planes[0] =
        plane(1, 4, 4, ByteOrder::Big, 0, {slot(0, 0, 0, 20), slot(1, 0, 0, 10), slot(2, 0, 0, 0)});
    formats.push_back(rgb);

    // Packed 4:1:1 (FFmpeg uyyvyy411 order): U Y0 Y1 V Y2 Y3, 6 bytes per 4 pixels.
    media::PixelFormatDescriptor packed{.name = "test-uyyvyy411",
                                        .model = media::ColourModel::Yuv,
                                        .componentCount = 3,
                                        .planeCount = 1};
    packed.components = {component(ComponentKind::Y, 8), component(ComponentKind::U, 8, 2),
                         component(ComponentKind::V, 8, 2)};
    packed.planes[0] = plane(4, 6, 1, ByteOrder::Little, 0,
                             {slot(1, 0, 0, 0), slot(0, 0, 1, 0), slot(0, 1, 2, 0),
                              slot(2, 0, 3, 0), slot(0, 2, 4, 0), slot(0, 3, 5, 0)});
    formats.push_back(packed);

    // Planar 4:1:0, 12-bit samples, two per big-endian 32-bit word (bits 16..27 and 0..11).
    media::PixelFormatDescriptor planar{.name = "test-yuv410p12-be32",
                                        .model = media::ColourModel::Yuv,
                                        .componentCount = 3,
                                        .planeCount = 3};
    planar.components = {component(ComponentKind::Y, 12), component(ComponentKind::U, 12, 2, 2),
                         component(ComponentKind::V, 12, 2, 2)};
    planar.planes[0] = plane(2, 4, 4, ByteOrder::Big, 0, {slot(0, 0, 0, 16), slot(0, 1, 0, 0)});
    planar.planes[1] = plane(8, 4, 4, ByteOrder::Big, 2, {slot(1, 0, 0, 16), slot(1, 1, 0, 0)});
    planar.planes[2] = plane(8, 4, 4, ByteOrder::Big, 2, {slot(2, 0, 0, 16), slot(2, 1, 0, 0)});
    formats.push_back(planar);
    return formats;
}

}  // namespace

TEST_CASE_FIXTURE(GpuTest, "GPU unpack is bit-exact for every catalogue format") {
    for (const media::FormatId id : media::catalogue()) {
        SUBCASE(media::descriptor(id).name.c_str()) {
            checkFormat(*this, id);
        }
    }
}

TEST_CASE_FIXTURE(GpuTest, "GPU unpack is bit-exact for custom formats") {
    for (const media::PixelFormatDescriptor& format : customFormats()) {
        SUBCASE(format.name.c_str()) {
            const dvv::Result<void> valid = media::validate(format);
            REQUIRE_MESSAGE(valid.has_value(), valid.error().describe());
            const dvv::Result<media::FormatId> id = detail::internCustomFormat(format);
            REQUIRE_MESSAGE(id.has_value(), id.error().describe());
            checkFormat(*this, *id);
        }
    }
}

TEST_CASE_FIXTURE(GpuTest, "an unpadded buffer is Internal") {
    const media::FormatId id = idOf("yuv420p");
    const auto pipeline = unpackPipeline(context());
    // 561 + 153 + 153 = 867 bytes, not a multiple of 4.
    const auto buffer = frameBuffer(context(), 867);
    std::optional<dvv::Result<void>> recorded;
    const auto submitted = submitAndWait([&](gpu::CommandRecorder& recorder) {
        recorded = detail::recordUnpack(recorder, *pipeline, *buffer, 0,
                                        {.format = id, .width = 33, .height = 17}, {});
    });
    REQUIRE_MESSAGE(submitted.has_value(), submitted.error().describe());
    REQUIRE(recorded.has_value());
    const dvv::Result<void> result = recorded.value_or(dvv::Result<void>{});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == dvv::Errc::Internal);
    CHECK(result.error().message.find("needs 872") != std::string::npos);
}

TEST_CASE_FIXTURE(GpuTest, "a frame base that is not 4-aligned is Internal") {
    const media::FormatId id = idOf("gray");
    const auto pipeline = unpackPipeline(context());
    const auto buffer = frameBuffer(context(), detail::unpackBufferBytes(2, 16));
    const auto image = makeImage(context(), 4, 4, VK_FORMAT_R8_UINT, "gray");
    const std::array<gpu::GpuImage*, 1> images{image.get()};
    std::optional<dvv::Result<void>> recorded;
    const auto submitted = submitAndWait([&](gpu::CommandRecorder& recorder) {
        recorded = detail::recordUnpack(recorder, *pipeline, *buffer, 2,
                                        {.format = id, .width = 4, .height = 4}, images);
    });
    REQUIRE_MESSAGE(submitted.has_value(), submitted.error().describe());
    REQUIRE(recorded.has_value());
    const dvv::Result<void> result = recorded.value_or(dvv::Result<void>{});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == dvv::Errc::Internal);
    CHECK(result.error().message.find("not 4-aligned") != std::string::npos);
}

TEST_CASE_FIXTURE(GpuTest, "images must match the components") {
    const media::FormatId id = idOf("yuv420p");
    const auto pipeline = unpackPipeline(context());
    const auto frame = media::geometry(id, 4, 4);
    REQUIRE(frame.has_value());
    const auto buffer = frameBuffer(context(), detail::unpackBufferBytes(0, frame->bytes));
    const auto y8 = makeImage(context(), 4, 4, VK_FORMAT_R8_UINT, "Y");
    const auto y16 = makeImage(context(), 4, 4, VK_FORMAT_R16_UINT, "Y 16-bit");
    const auto u = makeImage(context(), 2, 2, VK_FORMAT_R8_UINT, "U");
    const auto u_full = makeImage(context(), 4, 4, VK_FORMAT_R8_UINT, "U full size");
    const auto v = makeImage(context(), 2, 2, VK_FORMAT_R8_UINT, "V");

    const auto record = [&](std::span<gpu::GpuImage* const> images) {
        std::optional<dvv::Result<void>> recorded;
        const auto submitted = submitAndWait([&](gpu::CommandRecorder& recorder) {
            recorded = detail::recordUnpack(recorder, *pipeline, *buffer, 0,
                                            {.format = id, .width = 4, .height = 4}, images);
        });
        REQUIRE_MESSAGE(submitted.has_value(), submitted.error().describe());
        REQUIRE(recorded.has_value());
        return *recorded;
    };

    const std::array<gpu::GpuImage*, 2> two{y8.get(), u.get()};
    const auto too_few = record(two);
    REQUIRE_FALSE(too_few.has_value());
    CHECK(too_few.error().code == dvv::Errc::Internal);
    CHECK(too_few.error().message.find("3 components, 2 images") != std::string::npos);

    const std::array<gpu::GpuImage*, 3> wrong_size{y8.get(), u_full.get(), v.get()};
    const auto sized = record(wrong_size);
    REQUIRE_FALSE(sized.has_value());
    CHECK(sized.error().code == dvv::Errc::Internal);

    const std::array<gpu::GpuImage*, 3> wrong_format{y16.get(), u.get(), v.get()};
    const auto formatted = record(wrong_format);
    REQUIRE_FALSE(formatted.has_value());
    CHECK(formatted.error().code == dvv::Errc::Internal);
}

TEST_CASE_FIXTURE(GpuTest, "the unpack pipeline binds one storage image") {
    const auto pipeline = detail::createUnpackPipeline(context());
    REQUIRE_MESSAGE(pipeline.has_value(), pipeline.error().describe());
    CHECK((*pipeline)->bindings().storageImages == 1);
    CHECK((*pipeline)->pushConstantSize() == 64);
}
