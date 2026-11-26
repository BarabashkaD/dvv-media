#include "gpu_unpack.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string_view>

#include "dvv/base/error.h"
#include "dvv/base/json.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/sample_grid.h"
#include "dvv_media_shaders/unpack.h"
#include "image_format_name.h"

namespace dvv::media::detail {

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
    DVV_TRY(const StoredWordLayout layout, storedWordLayout(format, width, height, component));
    const auto [grid_width, grid_height] = componentSize(format, component, width, height);
    return UnpackPush{.base = base, .layout = layout, .width = grid_width, .height = grid_height};
}

Result<std::shared_ptr<gpu::ComputePipeline>> createUnpackPipeline(gpu::ExecutionContext& context) {
    constexpr VkFormatFeatureFlags2 NEEDED = VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT |
                                             VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
    for (const VkFormat format : {VK_FORMAT_R8_UINT, VK_FORMAT_R16_UINT}) {
        if ((gpu::formatFeatures(context, format) & NEEDED) != NEEDED) {
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
    return gpu::createComputePipeline(context, shader, sizeof(UnpackPush), {.storageImages = 1});
}

std::uint64_t unpackBufferBytes(std::uint64_t offset, std::uint64_t frame_bytes) noexcept {
    return offset + ((frame_bytes + 3) & ~std::uint64_t{3}) + 4;
}

Result<void> recordUnpack(gpu::CommandRecorder& recorder, const gpu::ComputePipeline& pipeline,
                          const gpu::GpuBuffer& buffer, std::uint64_t offset, const FrameDesc& desc,
                          std::span<gpu::GpuImage* const> images) {
    DVV_TRY(const FrameGeometry frame, geometry(desc.format, desc.width, desc.height));
    const PixelFormatDesc& format = formatDesc(desc.format);
    const std::uint64_t base = buffer.deviceAddress() + offset;
    if (base % 4 != 0) {
        return makeError(Errc::Internal,
                         std::format("unpack: the frame in buffer '{}' starts at offset {}, which "
                                     "is not 4-aligned",
                                     buffer.desc().name, offset));
    }
    const std::uint64_t needed = unpackBufferBytes(offset, frame.bytes);
    if (buffer.size() < needed || needed > (std::uint64_t{1} << 34U)) {
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
            gpu::vulkanFormat(*image) != image_format) {
            return makeError(
                Errc::Internal,
                std::format("unpack: the image of component {} of {} must be {}x{} {}", c,
                            format.name, push.width, push.height, imageFormatName(image_format)));
        }
    }
    for (std::size_t c = 0; c < images.size(); ++c) {
        recorder.dispatch(
            pipeline, std::as_bytes(std::span(&pushes[c], 1)),
            std::span<gpu::GpuImage* const>(&images[c], 1),
            gpu::GroupCount{.x = (pushes[c].width + UNPACK_GROUP - 1) / UNPACK_GROUP,
                            .y = (pushes[c].height + UNPACK_GROUP - 1) / UNPACK_GROUP});
    }
    return {};
}

}  // namespace dvv::media::detail
