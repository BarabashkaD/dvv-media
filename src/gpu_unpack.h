#pragma once

// The GPU unpack (M2 design D5, D7, §5.4): shaders/unpack.slang reads the original bytes of a frame
// by buffer device address and writes one R8_UINT or R16_UINT storage image per component, the
// same code values as the CPU reference unpack().

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include <vulkan/vulkan_core.h>

#include "dvv/base/error.h"
#include "dvv/gpu/command_recorder.h"
#include "dvv/gpu/compute_pipeline.h"
#include "dvv/gpu/context_vulkan.h"
#include "dvv/gpu/execution_context.h"
#include "dvv/gpu/image.h"
#include "dvv/gpu/resources.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/sample_grid.h"
#include "dvv/media/types.h"

namespace dvv::media::detail {

// Push constants of shaders/unpack.slang (M2 design §5.4): the frame's device address, the
// component's stored-word rule (dvv/media/sample_grid.h) and its grid size. std430/scalar-safe:
// the layout is 8-aligned and holds only 4-byte arrays.
struct UnpackPush {
    std::uint64_t base = 0;  // device address of the frame's first byte; 4-aligned
    StoredWordLayout layout;
    std::uint32_t width = 0;  // the component's grid size
    std::uint32_t height = 0;
};
static_assert(sizeof(UnpackPush) == 88);
static_assert(offsetof(UnpackPush, layout) == 8 && offsetof(UnpackPush, width) == 80 &&
              offsetof(UnpackPush, height) == 84);

inline constexpr std::uint32_t UNPACK_GROUP = 8;  // [numthreads(8, 8, 1)]

// R8_UINT for <= 8 bits, else R16_UINT (D5).
[[nodiscard]] VkFormat unpackedImageFormat(const ComponentDesc& component) noexcept;

// The push constants for one component: storedWordLayout() (and its errors) with the grid size.
[[nodiscard]] Result<UnpackPush> unpackPush(FormatId format, std::uint32_t width,
                                            std::uint32_t height, std::size_t component,
                                            std::uint64_t base);

// The unpack pipeline (bindings {1}, 88 bytes of push constants). Errc::Unsupported when R8_UINT
// or R16_UINT lacks STORAGE_IMAGE or STORAGE_WRITE_WITHOUT_FORMAT; Errc::Internal when the
// shader's reflected push-constant block is not sizeof(UnpackPush).
[[nodiscard]] Result<std::shared_ptr<gpu::ComputePipeline>> createUnpackPipeline(
    gpu::ExecutionContext& context);

// Records one dispatch per component, reading the frame at buffer + offset and writing
// images[c] for component c. Errc::Internal (nothing recorded) when (deviceAddress + offset) is
// not 4-aligned, when the buffer is smaller than offset + alignUp(bytes, 4) + 4 (the size rule of
// §5.4) or that exceeds 16 GiB (the shader's 32-bit dword index), or when the images do not match
// (count = componentCount, each componentSize() and unpackedImageFormat()). Recorder misuse
// (missing STORAGE usage, another device) fails the submission as usual.
[[nodiscard]] Result<void> recordUnpack(gpu::CommandRecorder& recorder,
                                        const gpu::ComputePipeline& pipeline,
                                        const gpu::GpuBuffer& buffer, std::uint64_t offset,
                                        const FrameDesc& desc,
                                        std::span<gpu::GpuImage* const> images);

// The size of a shader's push-constant block in its reflection JSON; Errc::Internal ("<shader>:
// ...") when the JSON cannot be read or has no push-constant block.
[[nodiscard]] Result<std::uint64_t> reflectedPushSize(std::string_view reflection_json,
                                                      std::string_view shader);

// Bytes a buffer needs to hold a frame at `offset` for the unpack: offset + alignUp(bytes, 4) + 4.
[[nodiscard]] std::uint64_t unpackBufferBytes(std::uint64_t offset,
                                              std::uint64_t frame_bytes) noexcept;

}  // namespace dvv::media::detail
