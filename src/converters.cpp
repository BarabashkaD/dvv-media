#include "converters.h"

#include <algorithm>
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
#include <variant>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "dvv/base/error.h"
#include "dvv/base/executor.h"
#include "dvv/base/task.h"
#include "dvv/gpu/command_recorder.h"
#include "dvv/gpu/compute_pipeline.h"
#include "dvv/gpu/execution_context.h"
#include "dvv/gpu/resources.h"
#include "dvv/media/display.h"
#include "dvv/media/frame.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/reference.h"
#include "dvv/media/types.h"
#include "dvv/platform/file_reader.h"
#include "gpu_unpack.h"
#include "store_state.h"
#include "to_display.h"

namespace dvv::media::detail {

namespace {

// The graph states of the M2 converters (§5.3).
constexpr StateKey HOST_ORIGINAL = PRODUCED;
constexpr StateKey HOST_PLANES{.unpacked = true, .api = Api::HostPlanes, .kind = MemKind::Host};
constexpr StateKey STAGING_BUFFER{
    .unpacked = false, .api = Api::VkBuffer, .kind = MemKind::HostVisible};
constexpr StateKey STAGING_ORIGINAL{
    .unpacked = false, .api = Api::HostPlanes, .kind = MemKind::HostVisible};
constexpr StateKey STAGING_PLANES{
    .unpacked = true, .api = Api::HostPlanes, .kind = MemKind::HostVisible};
constexpr StateKey DEVICE_BUFFER{
    .unpacked = false, .api = Api::VkBuffer, .kind = MemKind::DeviceLocal};
constexpr StateKey DEVICE_PLANES{
    .unpacked = true, .api = Api::VkImage, .kind = MemKind::DeviceLocal};

// One edge of the graph. The cost is `factor` times the bytes of the input view.
struct Edge {
    std::string_view name;
    StateKey from;
    StateKey to;
    LayoutPolicy layout = LayoutPolicy::Tight;
    Fidelity fidelity = Fidelity::Exact;
    double factor = 1;
};

// The tight bytes of the original (or, with `unpacked`, of its unpacked form); 0 when unknown.
[[nodiscard]] double viewBytes(const FrameDesc& desc, bool unpacked) noexcept {
    try {
        const FormatId format = unpacked ? unpackedFormat(desc.format) : desc.format;
        const Result<FrameGeometry> layout = geometry(format, desc.width, desc.height);
        return layout ? static_cast<double>(layout->bytes) : 0.0;
    } catch (...) {  // interning the unpacked format or the error message could not allocate
        return 0.0;
    }
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
    const auto* planes = std::get_if<HostPlanesB>(&input.binding);
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

// The buffer binding of the input; Internal without one.
[[nodiscard]] Result<VkBufferB> bufferOf(const ConvertInput& input, std::string_view converter) {
    const auto* binding = std::get_if<VkBufferB>(&input.binding);
    if (binding == nullptr || !binding->buffer) {
        return makeError(Errc::Internal, std::format("{}: frame {} of origin {} has no buffer",
                                                     converter, input.key.index, input.key.origin));
    }
    return *binding;
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

    [[nodiscard]] Task<Result<std::shared_ptr<IFrame>>> run(ConvertInput input,
                                                            ConvertContext context) const override {
        co_await resumeOn(context.cpu);
        const FrameDesc& desc = input.desc;
        const std::span<const std::byte> original = co_try(contiguousOriginal(input));
        const FrameGeometry layout = co_try(geometry(desc.format, desc.width, desc.height));
        const std::uint64_t size = unpackBufferBytes(0, layout.bytes);
        std::shared_ptr<gpu::GpuBuffer> buffer =
            co_try(context_->createBuffer({.size = size,
                                           .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                           .memory = gpu::MemoryUsage::HostUpload,
                                           .name = frameName("staging", input.key)}));
        const std::span<std::byte> mapped = buffer->mapped();
        if (mapped.size() < size) {
            co_return makeError(
                Errc::Internal,
                std::format("HostToStaging: buffer '{}' is not mapped", buffer->desc().name));
        }
        // Sequential writes only (write-combined memory): the original, then the zero padding.
        std::memcpy(mapped.data(), original.data(), original.size());
        std::memset(mapped.data() + original.size(), 0,
                    static_cast<std::size_t>(size) - original.size());
        co_try(buffer->flush());
        co_return std::shared_ptr<IFrame>(StoreAccess::makeFrame<StagingFrame>(
            context.store, desc, desc.format, context.output.domain, std::move(buffer), layout));
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

    [[nodiscard]] Task<Result<std::shared_ptr<IFrame>>> run(ConvertInput input,
                                                            ConvertContext context) const override {
        const FrameDesc& desc = input.desc;
        const VkBufferB source = co_try(bufferOf(input, name()));
        const FrameGeometry layout = co_try(geometry(desc.format, desc.width, desc.height));
        const std::uint64_t bytes = layout.bytes;
        std::shared_ptr<gpu::GpuBuffer> device = co_try(context_->createBuffer(
            {.size = unpackBufferBytes(0, bytes),
             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
             .memory = gpu::MemoryUsage::DeviceOnly,
             .name = frameName("original", input.key)}));
        // The record function holds both buffers until the GPU is done (D10).
        co_try(co_await context_->submit(
            *cpu_, [source, device, bytes](gpu::CommandRecorder& recorder) {
                recorder.copyBuffer(*source.buffer, source.offset, *device, 0, bytes);
            }));
        co_return std::shared_ptr<IFrame>(StoreAccess::makeFrame<VkBufferFrame>(
            context.store, desc, context.output.domain, std::move(device), layout));
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
                         .layout = LayoutPolicy::VkOptimal,
                         .fidelity = Fidelity::Lossless,
                         .factor = 0.5}),
          context_(std::move(context)),
          cpu_(std::move(cpu)),
          pipeline_(std::move(pipeline)) {}

    [[nodiscard]] Task<Result<std::shared_ptr<IFrame>>> run(ConvertInput given,
                                                            ConvertContext context) const override {
        const FrameDesc desc = given.desc;
        const FormatId unpacked = unpackedFormat(desc.format);
        const VkBufferB input = co_try(bufferOf(given, name()));
        const PixelFormatDescriptor& d = descriptor(desc.format);
        std::array<std::shared_ptr<gpu::GpuImage>, MAX_COMPONENTS> planes{};
        for (std::size_t c = 0; c < d.componentCount; ++c) {
            const auto [w, h] = componentSize(desc.format, c, desc.width, desc.height);
            planes[c] = co_try(context_->createImage(
                {.width = w,
                 .height = h,
                 .format = unpackedImageFormat(d.components[c]),
                 .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                 .name = std::format("planes {}:{} {}", given.key.origin, given.key.index, c)}));
        }
        const std::uint8_t count = d.componentCount;
        // recordUnpack's checks fail inside the record function (nothing recorded); the result
        // is read after the submission completed. The record function holds the input buffer, the
        // images and the pipeline until the GPU is done (D10).
        auto checked = std::make_shared<Result<void>>();
        co_try(co_await context_->submit(*cpu_, [pipeline = pipeline_, input, planes, desc, count,
                                                 checked](gpu::CommandRecorder& recorder) {
            std::array<gpu::GpuImage*, MAX_COMPONENTS> targets{};
            for (std::size_t c = 0; c < count; ++c) {
                targets[c] = planes[c].get();
            }
            *checked = recordUnpack(recorder, *pipeline, *input.buffer, input.offset, desc,
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

    [[nodiscard]] Task<Result<std::shared_ptr<IFrame>>> run(ConvertInput input,
                                                            ConvertContext context) const override {
        const FrameDesc& desc = input.desc;
        const VkBufferB source = co_try(bufferOf(input, name()));
        const FrameGeometry layout = co_try(geometry(desc.format, desc.width, desc.height));
        const std::uint64_t bytes = layout.bytes;
        // TRANSFER_SRC too: the frame's VkBuffer view is STAGING_BUFFER, an Upload source.
        std::shared_ptr<gpu::GpuBuffer> host = co_try(context_->createBuffer(
            {.size = bytes,
             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
             .memory = gpu::MemoryUsage::HostReadback,
             .name = frameName("readback", input.key)}));
        co_try(
            co_await context_->submit(*cpu_, [source, host, bytes](gpu::CommandRecorder& recorder) {
                recorder.copyBuffer(*source.buffer, source.offset, *host, 0, bytes);
            }));
        co_try(host->invalidate());
        co_return std::shared_ptr<IFrame>(StoreAccess::makeFrame<StagingFrame>(
            context.store, desc, desc.format, context.output.domain, std::move(host), layout));
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

    [[nodiscard]] Task<Result<std::shared_ptr<IFrame>>> run(ConvertInput input,
                                                            ConvertContext context) const override {
        const FrameDesc& desc = input.desc;
        const FormatId unpacked = unpackedFormat(desc.format);
        const FrameGeometry layout = co_try(geometry(unpacked, desc.width, desc.height));
        const auto* images = std::get_if<VkImageB>(&input.binding);
        bool matches = images != nullptr && images->planeCount == layout.planeCount;
        for (std::size_t c = 0; matches && c < layout.planeCount; ++c) {
            const gpu::GpuImage* image = images->planes[c].get();
            const std::uint32_t texel =
                image != nullptr && image->format() == VK_FORMAT_R8_UINT ? 1 : 2;
            matches = image != nullptr && layout.planes[c].stride == image->width() * texel &&
                      layout.planes[c].rows == image->height();
        }
        if (!matches) {
            co_return makeError(
                Errc::Internal,
                std::format("Readback: the planes of frame {} of origin {} do not "
                            "match {}",
                            input.key.index, input.key.origin, descriptor(unpacked).name));
        }
        // TRANSFER_SRC too, like ReadbackBuffer: the frame also offers a VkBuffer view.
        std::shared_ptr<gpu::GpuBuffer> host = co_try(context_->createBuffer(
            {.size = layout.bytes,
             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
             .memory = gpu::MemoryUsage::HostReadback,
             .name = frameName("readback planes", input.key)}));
        co_try(co_await context_->submit(
            *cpu_, [planes = *images, host, layout](gpu::CommandRecorder& recorder) {
                // The images are in GENERAL after the unpack: copied without a transition.
                for (std::size_t c = 0; c < planes.planeCount; ++c) {
                    recorder.copyImageToBuffer(*planes.planes[c], *host, layout.planes[c].offset);
                }
            }));
        co_try(host->invalidate());
        co_return std::shared_ptr<IFrame>(StoreAccess::makeFrame<StagingFrame>(
            context.store, desc, unpacked, context.output.domain, std::move(host), layout));
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
                         .layout = LayoutPolicy::VkOptimal,
                         .fidelity = Fidelity::Derived,
                         .factor = 1}),
          context_(std::move(context)),
          cpu_(std::move(cpu)),
          pipeline_(std::move(pipeline)),
          dummies_(std::move(dummies)) {}

    [[nodiscard]] Task<Result<std::shared_ptr<IFrame>>> run(ConvertInput given,
                                                            ConvertContext context) const override {
        const FrameDesc desc = given.desc;
        const auto* images = std::get_if<VkImageB>(&given.binding);
        if (images == nullptr) {
            co_return makeError(Errc::Internal,
                                std::format("ToDisplay: frame {} of origin {} has no planes",
                                            given.key.index, given.key.origin));
        }
        const VkImageB input = *images;
        const ColourParams resolved = resolveColour(desc).params;
        const ToDisplayPush push =
            co_try(toDisplayPush(desc.format, resolved, desc.width, desc.height));
        std::shared_ptr<gpu::GpuImage> output = co_try(context_->createImage(
            {.width = desc.width,
             .height = desc.height,
             .format = DISPLAY_IMAGE_FORMAT,
             .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
             .name = frameName("display", given.key)}));
        // recordToDisplay's checks fail inside the record function (nothing recorded); the result
        // is read after the submission completed. The record function holds the planes, the
        // output, the dummies and the pipeline until the GPU is done (D10).
        auto checked = std::make_shared<Result<void>>();
        co_try(co_await context_->submit(
            *cpu_, [pipeline = pipeline_, input, output, dummies = dummies_, push, desc,
                    checked](gpu::CommandRecorder& recorder) {
                std::array<gpu::GpuImage*, MAX_COMPONENTS> planes{};
                const std::size_t count = std::min<std::size_t>(input.planeCount, MAX_COMPONENTS);
                for (std::size_t c = 0; c < count; ++c) {
                    planes[c] = input.planes[c].get();
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
