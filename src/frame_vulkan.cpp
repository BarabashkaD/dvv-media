#include "dvv/media/frame_vulkan.h"

#include <cstddef>
#include <variant>

#include "dvv/media/frame.h"
#include "dvv/media/frame_store.h"

namespace dvv::media::vulkan {

// No lock and no allocation: the lease's binding owns the resources in 0.2.x.
template <LeaseKind K>
gpu::GpuImage* image(const Lease<K>& lease, std::size_t plane) noexcept {
    const auto* images = std::get_if<VkImageB>(&lease.binding());
    if (images == nullptr || plane >= images->planeCount || plane >= images->planes.size()) {
        return nullptr;
    }
    return images->planes[plane].get();
}

template <LeaseKind K>
const gpu::GpuBuffer* buffer(const Lease<K>& lease) noexcept {
    const auto* bound = std::get_if<VkBufferB>(&lease.binding());
    return bound == nullptr ? nullptr : bound->buffer.get();
}

template gpu::GpuImage* image<LeaseKind::Read>(const ReadLease&, std::size_t) noexcept;
template gpu::GpuImage* image<LeaseKind::Pin>(const PinLease&, std::size_t) noexcept;
template const gpu::GpuBuffer* buffer<LeaseKind::Read>(const ReadLease&) noexcept;
template const gpu::GpuBuffer* buffer<LeaseKind::Pin>(const PinLease&) noexcept;

}  // namespace dvv::media::vulkan
