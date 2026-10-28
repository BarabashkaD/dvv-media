#include "gpu_unpack.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "dvv/base/json.h"
#include "dvv_media_shaders/unpack.h"

namespace dvv::media::detail {

namespace {

// The name of an unpacked image format, for messages.
const char* imageFormatName(VkFormat format) noexcept {
    return format == VK_FORMAT_R8_UINT ? "R8_UINT" : "R16_UINT";
}

}  // namespace

Result<std::uint64_t> reflectedPushSize(std::string_view reflection_json, std::string_view shader) {
    DVV_TRY(const json::Value reflection, json::parse(reflection_json));
    try {
        const json::Value::json_pointer kind("/binding/kind");
        const json::Value::json_pointer size("/type/elementVarLayout/binding/size");
        const auto parameters = reflection.find("parameters");
        if (parameters != reflection.end()) {
            for (const json::Value& parameter : *parameters) {
                if (parameter.contains(kind) && parameter.at(kind) == "pushConstantBuffer") {
                    return parameter.at(size).get<std::uint64_t>();
                }
            }
        }
    } catch (const json::Value::exception& error) {
        return makeError(Errc::Internal, std::format("{}: cannot read the shader's reflection: {}",
                                                     shader, error.what()));
    }
    return makeError(Errc::Internal,
                     std::format("{}: the shader's reflection has no push-constant block", shader));
}

VkFormat unpackedImageFormat(const ComponentDesc& component) noexcept {
    return component.bits <= 8 ? VK_FORMAT_R8_UINT : VK_FORMAT_R16_UINT;
}

Result<UnpackPush> unpackPush(FormatId format, std::uint32_t width, std::uint32_t height,
                              std::size_t component, std::uint64_t base) {
    const PixelFormatDescriptor& d = descriptor(format);
    // The plane that holds the component's samples, and those samples sorted by index.
    std::optional<std::size_t> plane;
    std::array<SampleSlot, MAX_SLOTS> slots{};
    std::size_t n = 0;
    for (std::size_t p = 0; p < d.planeCount; ++p) {
        const PlaneDesc& candidate = d.planes[p];
        for (std::size_t s = 0; s < candidate.slotCount; ++s) {
            if (candidate.slots[s].component != component) {
                continue;
            }
            if (plane.has_value() && *plane != p) {
                return makeError(Errc::Internal,
                                 std::format("unpack: component {} of {} spans planes {} and {}",
                                             component, d.name, *plane, p));
            }
            plane = p;
            slots[n++] = candidate.slots[s];
        }
    }
    if (!plane.has_value() || component >= d.componentCount) {
        return makeError(Errc::Internal, std::format("unpack: component {} of {} has no samples",
                                                     component, d.name));
    }
    std::ranges::sort(std::span(slots).first(n), {}, &SampleSlot::index);
    const PlaneDesc& plane_desc = d.planes[*plane];
    const ComponentDesc& component_desc = d.components[component];
    bool indexed = true;
    for (std::size_t k = 0; k < n; ++k) {
        indexed = indexed && slots[k].index == k;
    }
    if (n > MAX_GPU_SLOTS || !indexed ||
        n != static_cast<std::size_t>(plane_desc.groupPixels >> component_desc.log2SubX)) {
        return makeError(Errc::Internal,
                         std::format("unpack: component {} of {} has {} samples per group, which "
                                     "the GPU unpack cannot read (at most {}, indices 0..n-1, "
                                     "groupPixels >> log2SubX)",
                                     component, d.name, n, MAX_GPU_SLOTS));
    }
    DVV_TRY(const FrameGeometry frame, geometry(format, width, height));

    UnpackPush push;
    push.base = base;
    push.planeOffset = frame.planes[*plane].offset;
    push.stride = frame.planes[*plane].stride;
    push.groupBytes = plane_desc.groupBytes;
    push.wordBytes = plane_desc.wordBytes;
    push.bigEndian = plane_desc.order == ByteOrder::Big ? 1U : 0U;
    push.bits = component_desc.bits;
    const auto [grid_width, grid_height] = componentSize(format, component, width, height);
    push.width = grid_width;
    push.height = grid_height;
    push.samplesPerGroup = static_cast<std::uint32_t>(n);
    for (std::size_t k = 0; k < n; ++k) {
        const std::uint32_t slot =
            (std::uint32_t{slots[k].word} << 8U) | std::uint32_t{slots[k].shift};
        push.slots[k / 2] |= slot << (16U * (k % 2));
    }
    return push;
}

Result<std::shared_ptr<gpu::ComputePipeline>> createUnpackPipeline(gpu::ExecutionContext& context) {
    constexpr VkFormatFeatureFlags2 NEEDED = VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT |
                                             VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
    for (const VkFormat format : {VK_FORMAT_R8_UINT, VK_FORMAT_R16_UINT}) {
        if ((context.formatFeatures(format) & NEEDED) != NEEDED) {
            return makeError(Errc::Unsupported,
                             std::format("device {} cannot write {} storage images without a "
                                         "format (M2 R1)",
                                         context.deviceId(), imageFormatName(format)));
        }
    }
    const gpu::EmbeddedShader& shader = dvv_media_shaders::unpack();
    DVV_TRY(const std::uint64_t push_size, reflectedPushSize(shader.reflectionJson, "unpack"));
    if (push_size != sizeof(UnpackPush)) {
        return makeError(Errc::Internal,
                         std::format("unpack: the shader's push constants are {} bytes, "
                                     "UnpackPush is {}",
                                     push_size, sizeof(UnpackPush)));
    }
    return context.createComputePipeline(shader, sizeof(UnpackPush), {.storageImages = 1});
}

std::uint64_t unpackBufferBytes(std::uint64_t offset, std::uint64_t frame_bytes) noexcept {
    return offset + ((frame_bytes + 3) & ~std::uint64_t{3}) + 4;
}

Result<void> recordUnpack(gpu::CommandRecorder& recorder, const gpu::ComputePipeline& pipeline,
                          const gpu::GpuBuffer& buffer, std::uint64_t offset, const FrameDesc& desc,
                          std::span<gpu::GpuImage* const> images) {
    DVV_TRY(const FrameGeometry frame, geometry(desc.format, desc.width, desc.height));
    const PixelFormatDescriptor& format = descriptor(desc.format);
    const std::uint64_t base = buffer.deviceAddress() + offset;
    if (base % 4 != 0) {
        return makeError(Errc::Internal,
                         std::format("unpack: the frame in buffer '{}' starts at offset {}, which "
                                     "is not 4-aligned",
                                     buffer.desc().name, offset));
    }
    const std::uint64_t needed = unpackBufferBytes(offset, frame.bytes);
    if (buffer.size() < needed || needed > (std::uint64_t{1} << 34)) {
        return makeError(Errc::Internal,
                         std::format("unpack: buffer '{}' holds {} bytes; a {}x{} {} frame at "
                                     "offset {} needs {} (frame bytes rounded up to 4, plus 4)",
                                     buffer.desc().name, buffer.size(), desc.width, desc.height,
                                     format.name, offset, needed));
    }
    if (images.size() != format.componentCount) {
        return makeError(Errc::Internal,
                         std::format("unpack: {} has {} components, {} images given", format.name,
                                     format.componentCount, images.size()));
    }
    std::array<UnpackPush, MAX_COMPONENTS> pushes{};
    for (std::size_t c = 0; c < images.size(); ++c) {
        DVV_TRY(const UnpackPush push, unpackPush(desc.format, desc.width, desc.height, c, base));
        pushes[c] = push;
        const gpu::GpuImage* image = images[c];
        const VkFormat image_format = unpackedImageFormat(format.components[c]);
        if (image == nullptr || image->width() != push.width || image->height() != push.height ||
            image->format() != image_format) {
            return makeError(
                Errc::Internal,
                std::format("unpack: the image of component {} of {} must be {}x{} {}", c,
                            format.name, push.width, push.height, imageFormatName(image_format)));
        }
    }
    for (std::size_t c = 0; c < images.size(); ++c) {
        recorder.dispatch(pipeline, std::as_bytes(std::span(&pushes[c], 1)),
                          std::span<gpu::GpuImage* const>(&images[c], 1),
                          (pushes[c].width + UNPACK_GROUP - 1) / UNPACK_GROUP,
                          (pushes[c].height + UNPACK_GROUP - 1) / UNPACK_GROUP);
    }
    return {};
}

}  // namespace dvv::media::detail
