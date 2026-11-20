#pragma once

// The Vulkan interop header of the frame model: a lease's GPU resources and the
// Vulkan binding kinds.

#include <cstddef>

#include "dvv/gpu/gpu_fwd.h"
#include "dvv/media/frame_store.h"
#include "dvv/media/types.h"

namespace dvv::media::vulkan {
inline constexpr BindingKind BUFFER = Api::VkBuffer;  // the whole original in one GPU buffer
inline constexpr BindingKind IMAGE = Api::VkImage;    // one GPU image per unpacked component

// The image of plane `plane` of an image binding (VkImageB). Null for an empty lease, another
// binding, or plane >= planeCount. The image is valid while `lease` (or a share of it) lives: a
// holder that keeps the pointer keeps a share beside it.
template <LeaseKind K>
[[nodiscard]] gpu::GpuImage* image(const Lease<K>& lease, std::size_t plane) noexcept;
// The buffer of a buffer binding (VkBufferB); null otherwise. Same lifetime rule.
template <LeaseKind K>
[[nodiscard]] const gpu::GpuBuffer* buffer(const Lease<K>& lease) noexcept;
}  // namespace dvv::media::vulkan
