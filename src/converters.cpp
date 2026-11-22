#include "converters.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "dvv/base/error.h"
#include "dvv/base/executor.h"
#include "dvv/base/task.h"
#include "dvv/gpu/command_recorder.h"
#include "dvv/gpu/compute_pipeline.h"
#include "dvv/gpu/context_vulkan.h"
#include "dvv/gpu/execution_context.h"
#include "dvv/gpu/image.h"
#include "dvv/gpu/resources.h"
#include "dvv/gpu/submission.h"
#include "dvv/media/display.h"
#include "dvv/media/frame.h"
#include "dvv/media/frame_vulkan.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/reference.h"
#include "dvv/media/types.h"
#include "dvv/platform/aligned_buffer.h"
#include "frames_vulkan.h"
#include "gpu_unpack.h"
#include "store_state.h"
#include "to_display.h"

namespace dvv::media::detail {

namespace {

// The graph states of the M2 converters (§5.3).
constexpr StateKey HOST_ORIGINAL = PRODUCED;
constexpr StateKey HOST_PLANES{
    .unpacked = true, .binding = media::HOST_PLANES, .kind = MemKind::Host};
constexpr StateKey STAGING_BUFFER{
    .unpacked = false, .binding = media::vulkan::BUFFER, .kind = MemKind::HostVisible};
constexpr StateKey STAGING_ORIGINAL{
    .unpacked = false, .binding = media::HOST_PLANES, .kind = MemKind::HostVisible};
constexpr StateKey STAGING_PLANES{
    .unpacked = true, .binding = media::HOST_PLANES, .kind = MemKind::HostVisible};
constexpr StateKey DEVICE_BUFFER{
    .unpacked = false, .binding = media::vulkan::BUFFER, .kind = MemKind::DeviceLocal};
constexpr StateKey DEVICE_PLANES{
    .unpacked = true, .binding = media::vulkan::IMAGE, .kind = MemKind::DeviceLocal};

// One edge of the graph. The cost is `factor` times the bytes of the input view.
struct Edge {
    std::string_view name;
    StateKey from;
    StateKey to;
    LayoutPolicy layout = LayoutPolicy::Tight;
    Fidelity fidelity = Fidelity::Exact;
    double factor = 1;
};

// The forms of a frame a converter reads or makes.
enum class Form : std::uint8_t { Original, Unpacked, Display };

// The tight bytes of the frame in `form` at its size; 0 when unknown.
[[nodiscard]] std::uint64_t tightBytes(const FrameDesc& desc, Form form) noexcept {
    try {
        FormatId format = desc.format;
        if (form == Form::Unpacked) {
            format = unpackedFormat(desc.format);
        } else if (form == Form::Display) {
            format = displayFormat();
        }
        const Result<FrameGeometry> layout = geometry(format, desc.width, desc.height);
        return layout ? layout->bytes : 0;
    } catch (...) {  // interning the unpacked format or the error message could not allocate
        return 0;
    }
}

// The tight bytes of the original (or, with `unpacked`, of its unpacked form); 0 when unknown.
[[nodiscard]] double viewBytes(const FrameDesc& desc, bool unpacked) noexcept {
    return static_cast<double>(tightBytes(desc, unpacked ? Form::Unpacked : Form::Original));
}

[[nodiscard]] std::string frameName(std::string_view what, const FrameKey& key) {
    return std::format("{} {}:{}", what, key.origin, key.index);
}

// The converters' common part: the edge. Members are set at creation and never change.
class EdgeConverter : public Converter {
public:
    explicit EdgeConverter(Edge edge) noexcept : edge_(edge) {}
    [[nodiscard]] std::string_view name() const noexcept final { return edge_.name; }
    [[nodiscard]] StateKey from() const noexcept final { return edge_.from; }
    [[nodiscard]] StateKey to() const noexcept final { return edge_.to; }
    [[nodiscard]] LayoutPolicy outputLayout() const noexcept final { return edge_.layout; }
    [[nodiscard]] Fidelity fidelity() const noexcept final { return edge_.fidelity; }
    [[nodiscard]] double cost(const FrameDesc& desc) const noexcept final {
        return edge_.factor * viewBytes(desc, edge_.from.unpacked);
    }

private:
    Edge edge_;
};

// Every original host binding in M2 is tight and contiguous: plane k starts at
// planes[0].data() + geometry.planes[k].offset. The whole original as one span.
[[nodiscard]] Result<std::span<const std::byte>> contiguousOriginal(const ConvertInput& input) {
    const FrameDesc& desc = input.desc;
    DVV_TRY(const FrameGeometry layout, geometry(desc.format, desc.width, desc.height));
    const HostPlanesB* planes = input.binding.planes();
    bool contiguous = planes != nullptr && planes->planeCount == layout.planeCount &&
                      layout.planeCount > 0 && planes->planes[0].data() != nullptr;
    for (std::size_t p = 0; contiguous && p < layout.planeCount; ++p) {
        const PlaneGeometry& plane = layout.planes[p];
        contiguous =
            planes->planes[p].data() ==
                planes->planes[0].data() + static_cast<std::size_t>(plane.offset) &&
            planes->planes[p].size() == static_cast<std::size_t>(plane.stride) * plane.rows;
    }
    if (!contiguous) {
        return makeError(Errc::Internal,
                         std::format("frame {} of origin {}: host original is not contiguous",
                                     input.key.index, input.key.origin));
    }
    return std::span<const std::byte>(planes->planes[0].data(),
                                      static_cast<std::size_t>(layout.bytes));
}

// The buffer of the input's binding; Internal without one.
[[nodiscard]] Result<const gpu::GpuBuffer*> bufferOf(const ConvertInput& input,
                                                     std::string_view converter) {
    const gpu::GpuBuffer* buffer = vulkan::buffer(input.binding);
    if (buffer == nullptr) {
        return makeError(Errc::Internal, std::format("{}: frame {} of origin {} has no buffer",
                                                     converter, input.key.index, input.key.origin));
    }
    return buffer;
}

// (original, HostPlanes, Host) -> (unpacked, HostPlanes, Host), and the same from HostVisible
// (a Readback output): the CPU reference unpack into a new host frame.
class CpuUnpack final : public EdgeConverter {
public:
    explicit CpuUnpack(StateKey from) noexcept
        : EdgeConverter({.name = "CpuUnpack",
                         .from = from,
                         .to = HOST_PLANES,
                         .layout = LayoutPolicy::Tight,
                         .fidelity = Fidelity::Lossless,
                         .factor = 4}) {}

    // The HostFrame's capacity: the unpacked form, tight.
    [[nodiscard]] std::size_t outputBytes(const FrameDesc& desc) const noexcept override {
        return static_cast<std::size_t>(tightBytes(desc, Form::Unpacked));
    }

    [[nodiscard]] Task<Result<std::shared_ptr<IFrame>>> run(ConvertInput input,
                                                            ConvertContext context) const override {
        co_await resumeOn(context.cpu);
        const FrameDesc& desc = input.desc;
        const std::span<const std::byte> original = co_try(contiguousOriginal(input));
        const FrameGeometry layout =
            co_try(geometry(unpackedFormat(desc.format), desc.width, desc.height));
        const auto bytes = static_cast<std::size_t>(layout.bytes);
        platform::AlignedBuffer buffer = co_try(platform::allocateAligned(bytes, 64));
        std::shared_ptr<HostFrame> frame = StoreAccess::makeFrame<HostFrame>(
            context.store, desc, context.output, std::move(buffer), bytes, 0);
        co_try(unpackToPlanes(desc.format, desc.width, desc.height, original, frame->data()));
        co_return std::shared_ptr<IFrame>(std::move(frame));
    }
};

// (original, HostPlanes, Host) -> (original, VkBuffer, HostVisible): a HostUpload buffer padded
// for the unpack shader.
class HostToStaging final : public EdgeConverter {
public:
    explicit HostToStaging(std::shared_ptr<gpu::ExecutionContext> context) noexcept
        : EdgeConverter({.name = "HostToStaging",
                         .from = HOST_ORIGINAL,
                         .to = STAGING_BUFFER,
                         .layout = LayoutPolicy::Tight,
                         .fidelity = Fidelity::Exact,
                         .factor = 1}),
          context_(std::move(context)) {}

    // The staging buffer: the original padded for the unpack shader.
    [[nodiscard]] std::size_t outputBytes(const FrameDesc& desc) const noexcept override {
        return static_cast<std::size_t>(unpackBufferBytes(0, tightBytes(desc, Form::Original)));
    }

    [[nodiscard]] Task<Result<std::shared_ptr<IFrame>>> run(ConvertInput input,
                                                            ConvertContext context) const override {
        co_await resumeOn(context.cpu);
        const FrameDesc& desc = input.desc;
        const std::span<const std::byte> original = co_try(contiguousOriginal(input));
        const FrameGeometry layout = co_try(geometry(desc.format, desc.width, desc.height));
        const std::uint64_t size = unpackBufferBytes(0, layout.bytes);
        std::shared_ptr<gpu::GpuBuffer> buffer =
            co_try(gpu::createBuffer(*context_, {.size = size,
                                                 .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                 .memory = gpu::MemoryUsage::HostUpload,
                                                 .name = frameName("staging", input.key)}));
        {
            gpu::MappedWrite w = buffer->mapWrite();
            const std::span<std::byte> mapped = w.bytes();
            if (mapped.size() < size) {
                co_return makeError(
                    Errc::Internal,
                    std::format("HostToStaging: buffer '{}' is not mapped", buffer->desc().name));
            }
            // Sequential writes only (write-combined memory): the original, then the zero padding.
            std::memcpy(mapped.data(), original.data(), original.size());
            std::memset(mapped.data() + original.size(), 0,
                        static_cast<std::size_t>(size) - original.size());
            co_try(w.flush());
        }
        co_return std::shared_ptr<IFrame>(StoreAccess::makeFrame<StagingFrame>(
            context.store, desc, desc.format, context.output.domain, std::move(buffer), layout,
            nullptr));
    }

private:
    std::shared_ptr<gpu::ExecutionContext> context_;
};

// (original, VkBuffer, HostVisible) -> (original, VkBuffer, DeviceLocal): a DeviceOnly buffer
// padded for the unpack shader.
class Upload final : public EdgeConverter {
public:
    Upload(std::shared_ptr<gpu::ExecutionContext> context, std::shared_ptr<IExecutor> cpu) noexcept
        : EdgeConverter({.name = "Upload",
                         .from = STAGING_BUFFER,
                         .to = DEVICE_BUFFER,
                         .layout = LayoutPolicy::Tight,
                         .fidelity = Fidelity::Exact,
                         .factor = 2}),
          context_(std::move(context)),
          cpu_(std::move(cpu)) {}

    // The device buffer: the original padded for the unpack shader.
    [[nodiscard]] std::size_t outputBytes(const FrameDesc& desc) const noexcept override {
        return static_cast<std::size_t>(unpackBufferBytes(0, tightBytes(desc, Form::Original)));
    }

    [[nodiscard]] Task<Result<std::shared_ptr<IFrame>>> run(ConvertInput input,
                                                            ConvertContext context) const override {
        const FrameDesc& desc = input.desc;
        co_try(bufferOf(input, name()));
        const FrameGeometry layout = co_try(geometry(desc.format, desc.width, desc.height));
        const std::uint64_t bytes = layout.bytes;
        std::shared_ptr<gpu::GpuBuffer> device =
            co_try(gpu::createBuffer(*context_, {.size = unpackBufferBytes(0, bytes),
                                                 .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                 .memory = gpu::MemoryUsage::DeviceOnly,
                                                 .name = frameName("original", input.key)}));
        // The record function holds the input binding (its buffer) and the device buffer until the
        // GPU is done (D10).
        co_try(co_await gpu::submit(
            *context_, *cpu_,
            [binding = input.binding, device, bytes](gpu::CommandRecorder& recorder) {
                recorder.copyBuffer(
                    *vulkan::buffer(binding), *device,
                    gpu::BufferCopy{.sourceOffset = 0, .destinationOffset = 0, .size = bytes});
            }));
        co_return std::shared_ptr<IFrame>(StoreAccess::makeFrame<VkBufferFrame>(
            context.store, desc, context.output.domain, std::move(device)));
    }

private:
    std::shared_ptr<gpu::ExecutionContext> context_;
    std::shared_ptr<IExecutor> cpu_;
};

// (original, VkBuffer, DeviceLocal) -> (unpacked, VkImage, DeviceLocal): the unpack shader, one
// R8_UINT/R16_UINT image per component, left in GENERAL.
class GpuUnpack final : public EdgeConverter {
public:
    GpuUnpack(std::shared_ptr<gpu::ExecutionContext> context, std::shared_ptr<IExecutor> cpu,
              std::shared_ptr<gpu::ComputePipeline> pipeline) noexcept
        : EdgeConverter({.name = "GpuUnpack",
                         .from = DEVICE_BUFFER,
                         .to = DEVICE_PLANES,
                         .layout = LayoutPolicy::Opaque,
                         .fidelity = Fidelity::Lossless,
                         .factor = 0.5}),
          context_(std::move(context)),
          cpu_(std::move(cpu)),
          pipeline_(std::move(pipeline)) {}

    // What its VkPlanesFrame reports: the unpacked form, tight.
    [[nodiscard]] std::size_t outputBytes(const FrameDesc& desc) const noexcept override {
        return static_cast<std::size_t>(tightBytes(desc, Form::Unpacked));
    }

    [[nodiscard]] Task<Result<std::shared_ptr<IFrame>>> run(ConvertInput given,
                                                            ConvertContext context) const override {
        const FrameDesc desc = given.desc;
        const FormatId unpacked = unpackedFormat(desc.format);
        co_try(bufferOf(given, name()));
        const PixelFormatDesc& d = formatDesc(desc.format);
        std::array<std::shared_ptr<gpu::GpuImage>, MAX_COMPONENTS> planes{};
        for (std::size_t c = 0; c < d.componentCount; ++c) {
            const auto [w, h] = componentSize(desc.format, c, desc.width, desc.height);
            planes[c] = co_try(gpu::createImage(
                *context_,
                {.width = w,
                 .height = h,
                 .format = unpackedImageFormat(d.components[c]),
                 .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                 .name = std::format("planes {}:{} {}", given.key.origin, given.key.index, c)}));
        }
        const std::uint8_t count = d.componentCount;
        // recordUnpack's checks fail inside the record function (nothing recorded); the result
        // is read after the submission completed. The record function holds the input binding (its
        // buffer), the images and the pipeline until the GPU is done (D10).
        auto checked = std::make_shared<Result<void>>();
        co_try(co_await gpu::submit(*context_, *cpu_,
                                    [pipeline = pipeline_, binding = given.binding, planes, desc,
                                     count, checked](gpu::CommandRecorder& recorder) {
                                        std::array<gpu::GpuImage*, MAX_COMPONENTS> targets{};
                                        for (std::size_t c = 0; c < count; ++c) {
                                            targets[c] = planes[c].get();
                                        }
                                        *checked = recordUnpack(
                                            recorder, *pipeline, *vulkan::buffer(binding), 0, desc,
                                            std::span<gpu::GpuImage* const>(targets).first(count));
                                    }));
        co_try(std::move(*checked));
        co_return std::shared_ptr<IFrame>(StoreAccess::makeFrame<VkPlanesFrame>(
            context.store, desc, unpacked, desc.colour, context.output.domain, planes, count));
    }

private:
    std::shared_ptr<gpu::ExecutionContext> context_;
    std::shared_ptr<IExecutor> cpu_;
    std::shared_ptr<gpu::ComputePipeline> pipeline_;
};

// (original, VkBuffer, DeviceLocal) -> (original, HostPlanes, HostVisible): a HostReadback copy,
// which also offers the VkBuffer view.
class ReadbackBuffer final : public EdgeConverter {
public:
    ReadbackBuffer(std::shared_ptr<gpu::ExecutionContext> context,
                   std::shared_ptr<IExecutor> cpu) noexcept
        : EdgeConverter({.name = "Readback",
                         .from = DEVICE_BUFFER,
                         .to = STAGING_ORIGINAL,
                         .layout = LayoutPolicy::Tight,
                         .fidelity = Fidelity::Exact,
                         .factor = 2}),
          context_(std::move(context)),
          cpu_(std::move(cpu)) {}

    // The readback buffer: the original, tight.
    [[nodiscard]] std::size_t outputBytes(const FrameDesc& desc) const noexcept override {
        return static_cast<std::size_t>(tightBytes(desc, Form::Original));
    }

    [[nodiscard]] Task<Result<std::shared_ptr<IFrame>>> run(ConvertInput input,
                                                            ConvertContext context) const override {
        const FrameDesc& desc = input.desc;
        co_try(bufferOf(input, name()));
        const FrameGeometry layout = co_try(geometry(desc.format, desc.width, desc.height));
        const std::uint64_t bytes = layout.bytes;
        // TRANSFER_SRC too: the frame's VkBuffer view is STAGING_BUFFER, an Upload source.
        std::shared_ptr<gpu::GpuBuffer> host =
            co_try(gpu::createBuffer(*context_, {.size = bytes,
                                                 .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                 .memory = gpu::MemoryUsage::HostReadback,
                                                 .name = frameName("readback", input.key)}));
        // The record function holds the input binding (its buffer) and the host buffer until the
        // GPU is done (D10).
        co_try(co_await gpu::submit(
            *context_, *cpu_,
            [binding = input.binding, host, bytes](gpu::CommandRecorder& recorder) {
                recorder.copyBuffer(
                    *vulkan::buffer(binding), *host,
                    gpu::BufferCopy{.sourceOffset = 0, .destinationOffset = 0, .size = bytes});
            }));
        gpu::MappedRead read = co_try(host->mapRead());
        co_return std::shared_ptr<IFrame>(StoreAccess::makeFrame<StagingFrame>(
            context.store, desc, desc.format, context.output.domain, std::move(host), layout,
            std::make_unique<gpu::MappedRead>(std::move(read))));
    }

private:
    std::shared_ptr<gpu::ExecutionContext> context_;
    std::shared_ptr<IExecutor> cpu_;
};

// (unpacked, VkImage, DeviceLocal) -> (unpacked, HostPlanes, HostVisible): the planes copied from
// GENERAL into one HostReadback buffer in the unpacked format's tight layout.
class ReadbackImage final : public EdgeConverter {
public:
    ReadbackImage(std::shared_ptr<gpu::ExecutionContext> context,
                  std::shared_ptr<IExecutor> cpu) noexcept
        : EdgeConverter({.name = "Readback",
                         .from = DEVICE_PLANES,
                         .to = STAGING_PLANES,
                         .layout = LayoutPolicy::Tight,
                         .fidelity = Fidelity::Exact,
                         .factor = 2}),
          context_(std::move(context)),
          cpu_(std::move(cpu)) {}

    // The readback buffer: the unpacked form, tight.
    [[nodiscard]] std::size_t outputBytes(const FrameDesc& desc) const noexcept override {
        return static_cast<std::size_t>(tightBytes(desc, Form::Unpacked));
    }

    [[nodiscard]] Task<Result<std::shared_ptr<IFrame>>> run(ConvertInput input,
                                                            ConvertContext context) const override {
        const FrameDesc& desc = input.desc;
        const FormatId unpacked = unpackedFormat(desc.format);
        const FrameGeometry layout = co_try(geometry(unpacked, desc.width, desc.height));
        bool matches = vulkan::imageCount(input.binding) == layout.planeCount;
        for (std::size_t c = 0; matches && c < layout.planeCount; ++c) {
            const gpu::GpuImage* image = vulkan::image(input.binding, c);
            const std::uint32_t texel =
                image != nullptr && gpu::vulkanFormat(*image) == VK_FORMAT_R8_UINT ? 1 : 2;
            matches = image != nullptr && layout.planes[c].stride == image->width() * texel &&
                      layout.planes[c].rows == image->height();
        }
        if (!matches) {
            co_return makeError(
                Errc::Internal,
                std::format("Readback: the planes of frame {} of origin {} do not "
                            "match {}",
                            input.key.index, input.key.origin, formatDesc(unpacked).name));
        }
        // TRANSFER_SRC too, like ReadbackBuffer: the frame also offers a VkBuffer view.
        std::shared_ptr<gpu::GpuBuffer> host =
            co_try(gpu::createBuffer(*context_, {.size = layout.bytes,
                                                 .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                 .memory = gpu::MemoryUsage::HostReadback,
                                                 .name = frameName("readback planes", input.key)}));
        // The record function holds the input binding (its images) and the host buffer until the
        // GPU is done (D10).
        co_try(co_await gpu::submit(
            *context_, *cpu_,
            [binding = input.binding, host, layout](gpu::CommandRecorder& recorder) {
                // The images are in GENERAL after the unpack: copied without a transition.
                for (std::size_t c = 0; c < vulkan::imageCount(binding); ++c) {
                    recorder.copyImageToBuffer(*vulkan::image(binding, c), *host,
                                               layout.planes[c].offset);
                }
            }));
        gpu::MappedRead read = co_try(host->mapRead());
        co_return std::shared_ptr<IFrame>(StoreAccess::makeFrame<StagingFrame>(
            context.store, desc, unpacked, context.output.domain, std::move(host), layout,
            std::make_unique<gpu::MappedRead>(std::move(read))));
    }

private:
    std::shared_ptr<gpu::ExecutionContext> context_;
    std::shared_ptr<IExecutor> cpu_;
};

// (unpacked, VkImage, DeviceLocal) -> DISPLAY_STATE (M3 D3-D6): the ToDisplay shader reads the
// planes and writes one RGBA16F linear-light image at frame size, left in GENERAL. The store's one
// colour-changing edge (Derived).
class ToDisplay final : public EdgeConverter {
public:
    ToDisplay(std::shared_ptr<gpu::ExecutionContext> context, std::shared_ptr<IExecutor> cpu,
              std::shared_ptr<gpu::ComputePipeline> pipeline,
              std::shared_ptr<const ToDisplayDummies> dummies) noexcept
        : EdgeConverter({.name = "ToDisplay",
                         .from = DEVICE_PLANES,
                         .to = DISPLAY_STATE,
                         .layout = LayoutPolicy::Opaque,
                         .fidelity = Fidelity::Derived,
                         .factor = 1}),
          context_(std::move(context)),
          cpu_(std::move(cpu)),
          pipeline_(std::move(pipeline)),
          dummies_(std::move(dummies)) {}

    // What its VkPlanesFrame reports: the display format at frame size, tight.
    [[nodiscard]] std::size_t outputBytes(const FrameDesc& desc) const noexcept override {
        return static_cast<std::size_t>(tightBytes(desc, Form::Display));
    }

    [[nodiscard]] Task<Result<std::shared_ptr<IFrame>>> run(ConvertInput given,
                                                            ConvertContext context) const override {
        const FrameDesc desc = given.desc;
        if (vulkan::imageCount(given.binding) == 0) {
            co_return makeError(Errc::Internal,
                                std::format("ToDisplay: frame {} of origin {} has no planes",
                                            given.key.index, given.key.origin));
        }
        const ColourParams resolved = resolveColour(desc).params;
        const ToDisplayPush push =
            co_try(toDisplayPush(desc.format, resolved, desc.width, desc.height));
        std::shared_ptr<gpu::GpuImage> output = co_try(gpu::createImage(
            *context_, {.width = desc.width,
                        .height = desc.height,
                        .format = DISPLAY_IMAGE_FORMAT,
                        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        .name = frameName("display", given.key)}));
        // recordToDisplay's checks fail inside the record function (nothing recorded); the result
        // is read after the submission completed. The record function holds the input binding (the
        // planes), the output, the dummies and the pipeline until the GPU is done (D10).
        auto checked = std::make_shared<Result<void>>();
        co_try(co_await gpu::submit(
            *context_, *cpu_,
            [pipeline = pipeline_, binding = given.binding, output, dummies = dummies_, push, desc,
             checked](gpu::CommandRecorder& recorder) {
                std::array<gpu::GpuImage*, MAX_COMPONENTS> planes{};
                const std::size_t count = vulkan::imageCount(binding);
                for (std::size_t c = 0; c < count; ++c) {
                    planes[c] = vulkan::image(binding, c);
                }
                *checked = recordToDisplay(recorder, *pipeline, push, desc.format,
                                           std::span<gpu::GpuImage* const>(planes).first(count),
                                           *output, *dummies);
            }));
        co_try(std::move(*checked));
        std::array<std::shared_ptr<gpu::GpuImage>, MAX_COMPONENTS> display{};
        display[0] = std::move(output);
        co_return std::shared_ptr<IFrame>(StoreAccess::makeFrame<VkPlanesFrame>(
            context.store, desc, displayFormat(), displayColour(), context.output.domain,
            std::move(display), std::uint8_t{1}));
    }

private:
    std::shared_ptr<gpu::ExecutionContext> context_;
    std::shared_ptr<IExecutor> cpu_;
    std::shared_ptr<gpu::ComputePipeline> pipeline_;
    std::shared_ptr<const ToDisplayDummies> dummies_;
};

}  // namespace

std::vector<std::unique_ptr<Converter>> makeHostConverters() {
    std::vector<std::unique_ptr<Converter>> converters;
    converters.push_back(std::make_unique<CpuUnpack>(HOST_ORIGINAL));
    converters.push_back(std::make_unique<CpuUnpack>(STAGING_ORIGINAL));
    return converters;
}

Result<std::vector<std::unique_ptr<Converter>>> makeGpuConverters(
    std::shared_ptr<gpu::ExecutionContext> context, std::shared_ptr<IExecutor> cpu) {
    if (!context || !cpu) {
        return makeError(Errc::Internal, "makeGpuConverters needs a context and a cpu executor");
    }
    DVV_TRY(std::shared_ptr<gpu::ComputePipeline> pipeline, createUnpackPipeline(*context));
    DVV_TRY(std::shared_ptr<gpu::ComputePipeline> display, createToDisplayPipeline(*context));
    DVV_TRY(ToDisplayDummies dummies, createToDisplayDummies(*context));
    std::vector<std::unique_ptr<Converter>> converters;
    converters.push_back(std::make_unique<HostToStaging>(context));
    converters.push_back(std::make_unique<Upload>(context, cpu));
    converters.push_back(std::make_unique<GpuUnpack>(context, cpu, std::move(pipeline)));
    converters.push_back(std::make_unique<ReadbackBuffer>(context, cpu));
    converters.push_back(std::make_unique<ReadbackImage>(context, cpu));
    for (std::unique_ptr<Converter>& converter : makeHostConverters()) {
        converters.push_back(std::move(converter));
    }
    // After the M2 converters, so their tie-breaks stay as they were.
    converters.push_back(
        std::make_unique<ToDisplay>(std::move(context), std::move(cpu), std::move(display),
                                    std::make_shared<const ToDisplayDummies>(std::move(dummies))));
    return converters;
}

}  // namespace dvv::media::detail
