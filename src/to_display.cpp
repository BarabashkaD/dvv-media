#include "to_display.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <utility>

#include <vulkan/vulkan_core.h>

#include "dvv/base/error.h"
#include "dvv/media/display.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"
#include "dvv_media_shaders/to_display.h"
#include "gpu_unpack.h"

namespace dvv::media::detail {

namespace {

const char* imageFormatName(VkFormat format) noexcept {
    switch (format) {
        case VK_FORMAT_R8_UINT: return "R8_UINT";
        case VK_FORMAT_R16_UINT: return "R16_UINT";
        case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
        default: return "another format";
    }
}

std::uint32_t transferCode(TransferFn transfer) noexcept {
    switch (transfer) {
        case TransferFn::Unspecified:
        case TransferFn::Srgb: return 0;
        case TransferFn::Bt1886: return 1;
        case TransferFn::Linear: return 2;
        case TransferFn::Pq: return 3;
        case TransferFn::Hlg: return 4;
    }
    return 0;
}

}  // namespace

Result<ToDisplayPush> toDisplayPush(FormatId format, const ColourParams& resolved,
                                    std::uint32_t width, std::uint32_t height) {
    const PixelFormatDescriptor& d = descriptor(format);
    if (d.componentCount == 0 || d.componentCount > MAX_COMPONENTS) {
        return makeError(Errc::Internal, std::format("ToDisplay: unknown format {}", format));
    }
    if (width == 0 || height == 0) {
        return makeError(Errc::Internal,
                         std::format("ToDisplay: an empty {}x{} {} frame", width, height, d.name));
    }
    const DisplayAffine affine = displayAffine(format, resolved);
    ToDisplayPush push;
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t i = 0; i < 3; ++i) {
            push.affine[(4 * r) + i] = affine.matrix[(3 * r) + i];
        }
        push.affine[(4 * r) + 3] = affine.offset[r];
    }
    for (std::size_t k = 0; k < d.componentCount; ++k) {
        const ComponentDesc& component = d.components[k];
        // Unpacked frames hold one plane per component in component order: binding k.
        push.planeLayout[k] = std::uint32_t{component.log2SubX} |
                              (std::uint32_t{component.log2SubY} << 4U) |
                              (static_cast<std::uint32_t>(k) << 8U);
    }
    push.width = width;
    push.height = height;
    push.componentCount = d.componentCount;
    push.transfer = transferCode(resolved.transfer);
    return push;
}

Result<std::shared_ptr<gpu::ComputePipeline>> createToDisplayPipeline(
    gpu::ExecutionContext& context) {
    constexpr VkFormatFeatureFlags2 READ =
        VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT;
    for (const VkFormat format : {VK_FORMAT_R8_UINT, VK_FORMAT_R16_UINT}) {
        if ((context.formatFeatures(format) & READ) != READ) {
            return makeError(Errc::Unsupported,
                             std::format("device {} cannot read {} storage images without a "
                                         "format (M3 R1)",
                                         context.deviceId(), imageFormatName(format)));
        }
    }
    if ((context.formatFeatures(DISPLAY_IMAGE_FORMAT) & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT) ==
        0) {
        return makeError(Errc::Unsupported,
                         std::format("device {} cannot write {} storage images (M3 R1)",
                                     context.deviceId(), imageFormatName(DISPLAY_IMAGE_FORMAT)));
    }
    const gpu::EmbeddedShader& shader = dvv_media_shaders::toDisplay();
    DVV_TRY(const std::uint64_t push_size, reflectedPushSize(shader.reflectionJson, "ToDisplay"));
    if (push_size != sizeof(ToDisplayPush)) {
        return makeError(Errc::Internal,
                         std::format("ToDisplay: the shader's push constants are {} bytes, "
                                     "ToDisplayPush is {}",
                                     push_size, sizeof(ToDisplayPush)));
    }
    return context.createComputePipeline(shader, sizeof(ToDisplayPush),
                                         {.storageImages = TO_DISPLAY_BINDINGS});
}

Result<ToDisplayDummies> createToDisplayDummies(gpu::ExecutionContext& context) {
    ToDisplayDummies dummies;
    for (std::size_t i = 0; i < dummies.images.size(); ++i) {
        DVV_TRY(dummies.images[i],
                context.createImage({.width = 1,
                                     .height = 1,
                                     .format = VK_FORMAT_R16_UINT,
                                     .usage = VK_IMAGE_USAGE_STORAGE_BIT,
                                     .name = std::format("to-display dummy {}", i)}));
    }
    return dummies;
}

Result<void> recordToDisplay(gpu::CommandRecorder& recorder, const gpu::ComputePipeline& pipeline,
                             const ToDisplayPush& push, FormatId format,
                             std::span<gpu::GpuImage* const> planes, gpu::GpuImage& output,
                             const ToDisplayDummies& dummies) {
    const PixelFormatDescriptor& d = descriptor(format);
    if (d.componentCount == 0 || planes.size() != d.componentCount) {
        return makeError(Errc::Internal,
                         std::format("ToDisplay: {} has {} components, {} planes given", d.name,
                                     d.componentCount, planes.size()));
    }
    for (std::size_t k = 0; k < planes.size(); ++k) {
        const gpu::GpuImage* plane = planes[k];
        const auto [w, h] = componentSize(format, k, push.width, push.height);
        const VkFormat plane_format = unpackedImageFormat(d.components[k]);
        if (plane == nullptr || plane->width() != w || plane->height() != h ||
            plane->format() != plane_format) {
            return makeError(Errc::Internal,
                             std::format("ToDisplay: the plane of component {} of {} must be {}x{} "
                                         "{}",
                                         k, d.name, w, h, imageFormatName(plane_format)));
        }
    }
    if (output.width() != push.width || output.height() != push.height ||
        output.format() != DISPLAY_IMAGE_FORMAT) {
        return makeError(Errc::Internal,
                         std::format("ToDisplay: the output must be {}x{} {}", push.width,
                                     push.height, imageFormatName(DISPLAY_IMAGE_FORMAT)));
    }
    std::array<gpu::GpuImage*, TO_DISPLAY_BINDINGS> bindings{};
    std::size_t dummy = 0;
    for (std::size_t k = 0; k + 1 < TO_DISPLAY_BINDINGS; ++k) {
        if (k < planes.size()) {
            bindings[k] = planes[k];
        } else {
            bindings[k] = dummies.images[dummy++].get();
        }
        if (bindings[k] == nullptr) {
            return makeError(Errc::Internal, "ToDisplay: a dummy image is missing");
        }
    }
    bindings[TO_DISPLAY_BINDINGS - 1] = &output;
    recorder.dispatch(pipeline, std::as_bytes(std::span(&push, 1)), bindings,
                      (push.width + TO_DISPLAY_GROUP - 1) / TO_DISPLAY_GROUP,
                      (push.height + TO_DISPLAY_GROUP - 1) / TO_DISPLAY_GROUP);
    return {};
}

}  // namespace dvv::media::detail
