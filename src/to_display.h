#pragma once

// ToDisplay (design M3 D3-D5): unpacked planes (R8_UINT/R16_UINT storage images read without a
// format) -> RGBA16F linear-light display image at frame size. shaders/to_display.slang implements
// the formulas of display.cpp (toDisplayReference) in float32.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include <vulkan/vulkan_core.h>

#include "dvv/base/error.h"
#include "dvv/gpu/command_recorder.h"
#include "dvv/gpu/compute_pipeline.h"
#include "dvv/gpu/execution_context.h"
#include "dvv/gpu/resources.h"
#include "dvv/media/types.h"

namespace dvv::media::detail {

// Push constants of shaders/to_display.slang. The design's "float matrix[9]; float offset[3]" is
// stored as three float4 rows (matrix row r, then offset r), and planeLayout as a uint4, so every
// member is 16-byte aligned in the shader's push-constant block (arrays would get a 16-byte
// stride).
struct ToDisplayPush {
    std::array<float, 12> affine{};  // rows: m[3r], m[3r+1], m[3r+2], offset[r]
    // Component k: log2SubX | log2SubY << 4 | binding << 8.
    std::array<std::uint32_t, 4> planeLayout{};
    std::uint32_t width = 0;  // output = frame size
    std::uint32_t height = 0;
    std::uint32_t componentCount = 0;    // 1..4 (alpha ignored)
    std::uint32_t transfer = 0;          // 0 sRGB/unspecified, 1 BT.1886, 2 linear, 3 PQ, 4 HLG
    std::uint32_t pad0 = 0;              // was the BT.2020 -> BT.709 flag (D5R §2.1); always 0
    std::array<std::uint32_t, 3> pad{};  // explicit, to 96 bytes
};
static_assert(sizeof(ToDisplayPush) == 96);
static_assert(offsetof(ToDisplayPush, planeLayout) == 48 && offsetof(ToDisplayPush, width) == 64 &&
              offsetof(ToDisplayPush, componentCount) == 72 &&
              offsetof(ToDisplayPush, pad0) == 80 && offsetof(ToDisplayPush, pad) == 84);

inline constexpr std::uint32_t TO_DISPLAY_GROUP = 8;     // [numthreads(8, 8, 1)]
inline constexpr std::uint32_t TO_DISPLAY_BINDINGS = 5;  // planes 0..3, output 4
inline constexpr VkFormat DISPLAY_IMAGE_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

// For a frame of `format` (original or unpacked: the components are the same) and a resolved
// colour. Errc::Internal for an unknown format or an empty frame.
[[nodiscard]] Result<ToDisplayPush> toDisplayPush(FormatId format, const ColourParams& resolved,
                                                  std::uint32_t width, std::uint32_t height);

// The ToDisplay pipeline (5 storage images, 96 bytes of push constants). Errc::Unsupported when
// R8_UINT or R16_UINT lacks STORAGE_IMAGE or STORAGE_READ_WITHOUT_FORMAT, or R16G16B16A16_SFLOAT
// lacks STORAGE_IMAGE ("device <id> cannot read <format> storage images without a format (M3 R1)");
// Errc::Internal when the shader's reflected push block is not 96 bytes.
[[nodiscard]] Result<std::shared_ptr<gpu::ComputePipeline>> createToDisplayPipeline(
    gpu::ExecutionContext& context);

// Three distinct 1x1 R16_UINT storage images for unused plane bindings (an image may appear only
// once per dispatch). Made once per store.
struct ToDisplayDummies {
    std::array<std::shared_ptr<gpu::GpuImage>, 3> images;
};
[[nodiscard]] Result<ToDisplayDummies> createToDisplayDummies(gpu::ExecutionContext& context);

// Records one dispatch. Checks first (Errc::Internal, nothing recorded): planes.size() equals the
// format's componentCount; plane k has componentSize(format, k, w, h) and unpackedImageFormat of
// component k; output is w x h DISPLAY_IMAGE_FORMAT. Bindings: k < planes.size() -> planes[k],
// otherwise the next dummy; binding 4 -> output.
[[nodiscard]] Result<void> recordToDisplay(gpu::CommandRecorder& recorder,
                                           const gpu::ComputePipeline& pipeline,
                                           const ToDisplayPush& push, FormatId format,
                                           std::span<gpu::GpuImage* const> planes,
                                           gpu::GpuImage& output, const ToDisplayDummies& dummies);

}  // namespace dvv::media::detail
