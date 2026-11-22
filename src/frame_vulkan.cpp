#include "dvv/media/frame_vulkan.h"

#include <cstddef>
#include <memory>
#include <span>
#include <utility>

#include "dvv/gpu/gpu_fwd.h"
#include "dvv/media/frame.h"
#include "dvv/media/frame_store.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"

namespace dvv::media::detail {

// The one writer and reader of a GPU binding's resources: they are stored type-erased and read
// back by kind, so frame.h needs no dvv-gpu type.
struct BindingAccess {
    // An empty binding of `kind`; add() gives it its resources.
    [[nodiscard]] static Binding make(BindingKind kind) noexcept {
        Binding binding;
        binding.kind_ = kind;
        return binding;
    }
    // The caller keeps the count at most MAX_COMPONENTS.
    static void add(Binding& binding, std::shared_ptr<void> resource) noexcept {
        binding.resources_[binding.resource_count_] = std::move(resource);
        ++binding.resource_count_;
    }
    // The resources of a binding of `kind`; 0 for another kind.
    [[nodiscard]] static std::size_t count(const Binding& binding, BindingKind kind) noexcept {
        return binding.kind_ == kind ? binding.resource_count_ : 0;
    }
    // Resource `index` of a binding of `kind` as T; null for another kind or index >= count.
    template <class T>
    [[nodiscard]] static T* resource(const Binding& binding, BindingKind kind,
                                     std::size_t index) noexcept {
        if (index >= count(binding, kind)) {
            return nullptr;
        }
        return static_cast<T*>(binding.resources_[index].get());
    }
};

}  // namespace dvv::media::detail

namespace dvv::media::vulkan {

using detail::BindingAccess;

Binding bufferBinding(std::shared_ptr<gpu::GpuBuffer> buffer) noexcept {
    if (!buffer) {
        return Binding{};
    }
    Binding binding = BindingAccess::make(BUFFER);
    BindingAccess::add(binding, std::move(buffer));
    return binding;
}

Binding imageBinding(std::span<const std::shared_ptr<gpu::GpuImage>> planes) noexcept {
    if (planes.empty() || planes.size() > MAX_COMPONENTS) {
        return Binding{};
    }
    for (const std::shared_ptr<gpu::GpuImage>& plane : planes) {
        if (!plane) {
            return Binding{};
        }
    }
    Binding binding = BindingAccess::make(IMAGE);
    for (const std::shared_ptr<gpu::GpuImage>& plane : planes) {
        BindingAccess::add(binding, plane);
    }
    return binding;
}

const gpu::GpuBuffer* buffer(const Binding& binding) noexcept {
    return BindingAccess::resource<gpu::GpuBuffer>(binding, BUFFER, 0);
}

gpu::GpuImage* image(const Binding& binding, std::size_t plane) noexcept {
    return BindingAccess::resource<gpu::GpuImage>(binding, IMAGE, plane);
}

std::size_t imageCount(const Binding& binding) noexcept {
    return BindingAccess::count(binding, IMAGE);
}

// No lock and no allocation: the lease's binding owns the resources.
template <LeaseKind K>
gpu::GpuImage* image(const Lease<K>& lease, std::size_t plane) noexcept {
    return image(lease.binding(), plane);
}

template <LeaseKind K>
const gpu::GpuBuffer* buffer(const Lease<K>& lease) noexcept {
    return buffer(lease.binding());
}

template gpu::GpuImage* image<LeaseKind::Read>(const ReadLease&, std::size_t) noexcept;
template gpu::GpuImage* image<LeaseKind::Pin>(const PinLease&, std::size_t) noexcept;
template const gpu::GpuBuffer* buffer<LeaseKind::Read>(const ReadLease&) noexcept;
template const gpu::GpuBuffer* buffer<LeaseKind::Pin>(const PinLease&) noexcept;

}  // namespace dvv::media::vulkan
