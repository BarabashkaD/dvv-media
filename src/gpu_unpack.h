#pragma once

// The GPU unpack (M2 design D5, D7, §5.4): shaders/unpack.slang reads the original bytes of a frame
// by buffer device address and writes one R8_UINT or R16_UINT storage image per component, the
// same code values as the CPU reference unpack().

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include <vulkan/vulkan_core.h>

#include "dvv/base/error.h"
#include "dvv/gpu/command_recorder.h"
#include "dvv/gpu/compute_pipeline.h"
#include "dvv/gpu/execution_context.h"
#include "dvv/gpu/resources.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"

namespace dvv::media::detail {

// Push constants of shaders/unpack.slang (M2 design §5.4). The layout is std430/scalar-safe: two
// 8-byte members first, then 4-byte members, slots as a uint3 at offset 48, explicit padding.
struct UnpackPush {
    std::uint64_t base = 0;         // device address of the frame's first byte; 4-aligned
    std::uint64_t planeOffset = 0;  // byte offset of the component's plane inside the frame
    std::uint32_t stride = 0;       // bytes per plane row
    std::uint32_t groupBytes = 0;
    std::uint32_t wordBytes = 0;  // 1, 2 or 4
    std::uint32_t bigEndian = 0;  // 1: words are big-endian
    std::uint32_t bits = 0;       // significant bits of the component
    std::uint32_t width = 0;      // the component's grid size
    std::uint32_t height = 0;
    std::uint32_t samplesPerGroup = 0;
    std::array<std::uint32_t, 3> slots{};  // slot k: (word << 8 | shift) in bits 16*(k%2) of [k/2]
    std::uint32_t pad = 0;
};
static_assert(sizeof(UnpackPush) == 64);
static_assert(offsetof(UnpackPush, planeOffset) == 8 && offsetof(UnpackPush, stride) == 16 &&
              offsetof(UnpackPush, samplesPerGroup) == 44 && offsetof(UnpackPush, slots) == 48 &&
              offsetof(UnpackPush, pad) == 60);

inline constexpr std::size_t MAX_GPU_SLOTS = 6;   // per component and group (validate() checks it)
inline constexpr std::uint32_t UNPACK_GROUP = 8;  // [numthreads(8, 8, 1)]
static_assert(MAX_GPU_SLOTS == MAX_COMPONENT_SAMPLES);

// R8_UINT for <= 8 bits, else R16_UINT (D5).
[[nodiscard]] VkFormat unpackedImageFormat(const ComponentDesc& component) noexcept;

// The push constants for one component. Errc::Internal when the component has no slots, more than
// MAX_GPU_SLOTS, slot indices that are not 0..n-1, or n != groupPixels >> log2SubX; Errc::Format
// from geometry().
[[nodiscard]] Result<UnpackPush> unpackPush(FormatId format, std::uint32_t width,
                                            std::uint32_t height, std::size_t component,
                                            std::uint64_t base);

// The unpack pipeline (bindings {1}, 64 bytes of push constants). Errc::Unsupported when R8_UINT
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
