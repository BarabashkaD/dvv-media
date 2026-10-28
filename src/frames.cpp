#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include <vulkan/vulkan_core.h>

#include "dvv/gpu/resources.h"
#include "dvv/media/frame.h"
#include "dvv/media/pixel_format.h"

namespace dvv::media {

namespace {

// The frame's geometry when it fits into `capacity` bytes from `offset`; nullopt otherwise.
std::optional<FrameGeometry> fittingGeometry(const ViewSpec& view, const FrameDesc& desc,
                                             std::size_t capacity, std::size_t offset) noexcept {
    try {
        const Result<FrameGeometry> layout = geometry(view.format, desc.width, desc.height);
        if (!layout || offset > capacity || layout->bytes > capacity - offset) {
            return std::nullopt;
        }
        return *layout;
    } catch (...) {  // the error message could not be allocated
        return std::nullopt;
    }
}

}  // namespace

HostFrame::HostFrame(CreationKey<FrameStore> key, FrameDesc desc, ViewSpec view,
                     platform::AlignedBuffer memory, std::size_t capacity,
                     std::size_t data_offset) noexcept
    : Managed<HostFrame>(std::move(key)),
      desc_(desc),
      view_(view),
      memory_(std::move(memory)),
      capacity_(capacity),
      data_offset_(data_offset) {}

std::span<std::byte> HostFrame::memory() noexcept {
    if (sealed_.load(std::memory_order_acquire) || !memory_) {
        return {};
    }
    return {memory_.get(), capacity_};
}

std::span<std::byte> HostFrame::data() noexcept {
    const std::span<std::byte> whole = memory();
    if (whole.empty()) {
        return {};
    }
    const std::optional<FrameGeometry> layout =
        fittingGeometry(view_, desc_, capacity_, data_offset_);
    if (!layout) {
        return {};
    }
    return whole.subspan(data_offset_, static_cast<std::size_t>(layout->bytes));
}

void HostFrame::setDataOffset(std::size_t offset) noexcept {
    if (sealed_.load(std::memory_order_acquire)) {
        return;  // published: leaseholders read the offset without synchronisation
    }
    data_offset_ = offset;
}

std::span<const ViewSpec> HostFrame::nativeViews() const noexcept {
    return {&view_, 1};
}

Binding HostFrame::bind(const ViewSpec& native) const {
    HostPlanesB planes;
    if (native != view_ || !memory_) {
        return planes;
    }
    const std::optional<FrameGeometry> layout =
        fittingGeometry(view_, desc_, capacity_, data_offset_);
    if (!layout) {
        return planes;
    }
    const std::byte* const base = memory_.get() + data_offset_;
    for (std::size_t p = 0; p < layout->planeCount; ++p) {
        const PlaneGeometry& plane = layout->planes[p];
        planes.planes[p] = std::span<const std::byte>(
            base + plane.offset, static_cast<std::size_t>(plane.stride) * plane.rows);
        planes.strides[p] = plane.stride;
    }
    planes.planeCount = layout->planeCount;
    return planes;
}

void HostFrame::seal() noexcept {
    sealed_.store(true, std::memory_order_release);
}

// ---- StagingFrame ----

StagingFrame::StagingFrame(CreationKey<FrameStore> key, FrameDesc desc, FormatId format,
                           Domain domain, std::shared_ptr<gpu::GpuBuffer> buffer,
                           FrameGeometry geometry) noexcept
    : Managed<StagingFrame>(std::move(key)),
      desc_(desc),
      buffer_(std::move(buffer)),
      geometry_(geometry) {
    const ViewSpec buffer_view{format, desc.colour, LayoutPolicy::Tight, Api::VkBuffer, domain, 0};
    if (buffer_ && buffer_->desc().memory == gpu::MemoryUsage::HostReadback) {
        ViewSpec host_view = buffer_view;
        host_view.api = Api::HostPlanes;
        views_[view_count_++] = host_view;
    }
    views_[view_count_++] = buffer_view;
}

std::span<const ViewSpec> StagingFrame::nativeViews() const noexcept {
    return std::span<const ViewSpec>(views_).first(view_count_);
}

Binding StagingFrame::bind(const ViewSpec& native) const {
    if (std::ranges::find(nativeViews(), native) == nativeViews().end()) {
        return HostPlanesB{};
    }
    if (native.api == Api::VkBuffer) {
        return VkBufferB{.buffer = buffer_, .offset = 0, .geometry = geometry_};
    }
    HostPlanesB planes;
    const std::span<const std::byte> mapped = buffer_->mapped();
    if (mapped.size() < geometry_.bytes) {
        return planes;
    }
    for (std::size_t p = 0; p < geometry_.planeCount; ++p) {
        const PlaneGeometry& plane = geometry_.planes[p];
        planes.planes[p] = mapped.subspan(static_cast<std::size_t>(plane.offset),
                                          static_cast<std::size_t>(plane.stride) * plane.rows);
        planes.strides[p] = plane.stride;
    }
    planes.planeCount = geometry_.planeCount;
    return planes;
}

std::size_t StagingFrame::bytes() const noexcept {
    return buffer_ ? static_cast<std::size_t>(buffer_->size()) : 0;
}

// ---- VkBufferFrame ----

VkBufferFrame::VkBufferFrame(CreationKey<FrameStore> key, FrameDesc desc, Domain domain,
                             std::shared_ptr<gpu::GpuBuffer> buffer,
                             FrameGeometry geometry) noexcept
    : Managed<VkBufferFrame>(std::move(key)),
      desc_(desc),
      view_{desc.format, desc.colour, LayoutPolicy::Tight, Api::VkBuffer, domain, 0},
      buffer_(std::move(buffer)),
      geometry_(geometry) {}

std::span<const ViewSpec> VkBufferFrame::nativeViews() const noexcept {
    return {&view_, 1};
}

Binding VkBufferFrame::bind(const ViewSpec& native) const {
    if (native != view_) {
        return HostPlanesB{};
    }
    return VkBufferB{.buffer = buffer_, .offset = 0, .geometry = geometry_};
}

std::size_t VkBufferFrame::bytes() const noexcept {
    return buffer_ ? static_cast<std::size_t>(buffer_->size()) : 0;
}

// ---- VkPlanesFrame ----

VkPlanesFrame::VkPlanesFrame(CreationKey<FrameStore> key, FrameDesc desc, FormatId format,
                             ColourId colour, Domain domain,
                             std::array<std::shared_ptr<gpu::GpuImage>, MAX_COMPONENTS> planes,
                             std::uint8_t plane_count) noexcept
    : Managed<VkPlanesFrame>(std::move(key)),
      desc_(desc),
      view_{format, colour, LayoutPolicy::VkOptimal, Api::VkImage, domain, 0},
      planes_{.planes = std::move(planes), .planeCount = plane_count} {
    try {
        if (const Result<FrameGeometry> layout = geometry(format, desc.width, desc.height)) {
            bytes_ = static_cast<std::size_t>(layout->bytes);
        }
    } catch (...) {  // the error message could not be allocated: bytes stay 0
        bytes_ = 0;
    }
}

std::span<const ViewSpec> VkPlanesFrame::nativeViews() const noexcept {
    return {&view_, 1};
}

Binding VkPlanesFrame::bind(const ViewSpec& native) const {
    if (native != view_) {
        return HostPlanesB{};
    }
    return planes_;
}

}  // namespace dvv::media
