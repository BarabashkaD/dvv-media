#pragma once

// The Vulkan interop header of the frame model: the Vulkan binding kinds, the GPU bindings (made
// and read here) and a lease's GPU resources.

#include <cstddef>
#include <memory>
#include <span>

#include "dvv/gpu/gpu_fwd.h"
#include "dvv/media/frame.h"
#include "dvv/media/frame_store.h"
#include "dvv/media/types.h"

namespace dvv::media::vulkan {
inline constexpr BindingKind BUFFER{"VkBuffer"};  // the whole original in one GPU buffer
inline constexpr BindingKind IMAGE{"VkImage"};    // one GPU image per unpacked component

// A BUFFER binding: the whole original in `buffer`, tight, from offset 0. An IFrame's bind()
// returns it (e.g. a frame given to FrameStore::adopt). A null buffer gives Binding{} (HOST_PLANES,
// planeCount 0).
[[nodiscard]] Binding bufferBinding(std::shared_ptr<gpu::GpuBuffer> buffer) noexcept;
// An IMAGE binding: one image per unpacked component in component order, layout GENERAL. An empty
// span, a null image or more than MAX_COMPONENTS images give Binding{} (nothing is cut silently).
[[nodiscard]] Binding imageBinding(std::span<const std::shared_ptr<gpu::GpuImage>> planes) noexcept;

// The buffer of a BUFFER binding; null for another kind. Valid while `binding` or a copy lives.
[[nodiscard]] const gpu::GpuBuffer* buffer(const Binding& binding) noexcept;
// The image of plane `plane` of an IMAGE binding; null for another kind or plane >= imageCount.
// Same lifetime rule.
[[nodiscard]] gpu::GpuImage* image(const Binding& binding, std::size_t plane) noexcept;
// The image count of an IMAGE binding; 0 for another kind.
[[nodiscard]] std::size_t imageCount(const Binding& binding) noexcept;

// The same for a lease's binding: null for an empty lease. Valid while `lease` (or a share of it)
// lives: a holder that keeps the pointer keeps a share beside it.
template <LeaseKind K>
[[nodiscard]] gpu::GpuImage* image(const Lease<K>& lease, std::size_t plane) noexcept;
template <LeaseKind K>
[[nodiscard]] const gpu::GpuBuffer* buffer(const Lease<K>& lease) noexcept;
}  // namespace dvv::media::vulkan
