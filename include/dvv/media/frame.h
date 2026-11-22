#pragma once

// Frames: the IFrame interface, its bindings, the host memory frame and the frame handle.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "dvv/base/lifetime.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"
#include "dvv/platform/aligned_buffer.h"

namespace dvv::media {

class FrameStore;

// Read-only host bytes of one view.
struct HostPlanesB {
    std::array<std::span<const std::byte>, MAX_PLANES> planes{};
    std::array<std::uint32_t, MAX_PLANES> strides{};
    std::uint8_t planeCount = 0;
};

namespace detail {
struct BindingAccess;  // src/frame_vulkan.cpp: makes and reads the resources of a GPU binding
}  // namespace detail

// What one view of a frame gives its reader: the host planes (kind HOST_PLANES), or the GPU
// resources of a backend kind (vulkan::BUFFER, vulkan::IMAGE), made and read through that
// backend's header (dvv/media/frame_vulkan.h). A copy shares the resources and keeps them alive.
class Binding {
public:
    // Empty host planes (planeCount 0): what bind() gives for a view the frame does not offer.
    Binding() noexcept = default;
    // Implicit, so an IFrame's bind() can return its HostPlanesB.
    Binding(const HostPlanesB& planes) noexcept : planes_(planes) {}
    [[nodiscard]] BindingKind kind() const noexcept { return kind_; }
    // The host planes; null for another kind.
    [[nodiscard]] const HostPlanesB* planes() const noexcept {
        return kind_ == HOST_PLANES ? &planes_ : nullptr;
    }

private:
    friend struct detail::BindingAccess;
    BindingKind kind_ = HOST_PLANES;
    HostPlanesB planes_{};
    // A GPU kind: its buffer, or one image per unpacked component, type-erased; the backend that
    // made the binding reads them back by kind.
    std::array<std::shared_ptr<void>, MAX_COMPONENTS> resources_{};
    std::uint8_t resource_count_ = 0;
};

// A frame: its description, its native views and the binding of each view.
class IFrame {
public:
    virtual ~IFrame() = default;
    [[nodiscard]] virtual const FrameDesc& desc() const noexcept = 0;
    [[nodiscard]] virtual std::span<const ViewSpec> nativeViews() const noexcept = 0;
    // No I/O, no GPU work. A view that is not native gives a default Binding (HostPlanesB with
    // planeCount 0); the store binds native views only.
    [[nodiscard]] virtual Binding bind(const ViewSpec& native) const = 0;
    [[nodiscard]] virtual std::size_t bytes() const noexcept = 0;
};

// Host memory frame: aligned buffer; the frame data starts at data_offset inside it. Writable
// until the store publishes or adopts it; then memory() and data() are empty (read through bind()).
class HostFrame final : public IFrame, public Managed<HostFrame> {
public:
    HostFrame(CreationKey<FrameStore> key, FrameDesc desc, ViewSpec view,
              platform::AlignedBuffer memory, std::size_t capacity,
              std::size_t data_offset) noexcept;
    [[nodiscard]] std::span<std::byte> memory() noexcept;  // the whole buffer
    [[nodiscard]] std::span<std::byte> data() noexcept;    // geometry().bytes at dataOffset()
    [[nodiscard]] std::size_t dataOffset() const noexcept { return data_offset_; }
    [[nodiscard]] const FrameDesc& desc() const noexcept override { return desc_; }
    [[nodiscard]] std::span<const ViewSpec> nativeViews() const noexcept override;  // {view}
    [[nodiscard]] Binding bind(const ViewSpec& native) const override;  // planes per geometry()
    [[nodiscard]] std::size_t bytes() const noexcept override { return capacity_; }

private:
    friend class FrameStore;
    void seal() noexcept;  // published: memory() and data() become empty
    FrameDesc desc_;
    ViewSpec view_;
    platform::AlignedBuffer memory_;
    std::size_t capacity_;
    std::size_t data_offset_;
    std::atomic<bool> sealed_{false};
};

// Handle: key + desc + store.
struct FrameRef {
    FrameKey key;
    FrameDesc desc;
    std::shared_ptr<FrameStore> store;
};

}  // namespace dvv::media
