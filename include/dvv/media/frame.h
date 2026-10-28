#pragma once

// Frames (M2 design §4.4, spec §6.2): the IFrame interface, its bindings, the host memory frame,
// the GPU frames and the frame handle.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <variant>

#include "dvv/base/lifetime.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"
#include "dvv/platform/file_reader.h"

namespace dvv::gpu {
class GpuBuffer;
class GpuImage;
}  // namespace dvv::gpu

namespace dvv::media {

class FrameStore;

// Read-only host bytes of one view.
struct HostPlanesB {
    std::array<std::span<const std::byte>, MAX_PLANES> planes{};
    std::array<std::uint32_t, MAX_PLANES> strides{};
    std::uint8_t planeCount = 0;
};
// The whole original in one buffer, tight.
struct VkBufferB {
    std::shared_ptr<gpu::GpuBuffer> buffer;
    std::uint64_t offset = 0;
    FrameGeometry geometry{};
};
// One image per unpacked component, layout GENERAL.
struct VkImageB {
    std::array<std::shared_ptr<gpu::GpuImage>, MAX_COMPONENTS> planes{};
    std::uint8_t planeCount = 0;
};
using Binding = std::variant<HostPlanesB, VkBufferB, VkImageB>;  // + AvFrameB in M4

// Spec §6.2.
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
    // Before publishing only; after publishing: ignored, like memory() and data().
    void setDataOffset(std::size_t offset) noexcept;
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

// A frame in one host-visible GpuBuffer holding the tight layout of `format` (desc.format or
// unpackedFormat(desc.format)) at offset 0. Native views (format, desc.colour, Tight, domain):
// - HostReadback memory (made by Readback): HostPlanes and VkBuffer, the same bytes (one store
//   entry, counted once);
// - HostUpload memory (made by HostToStaging): VkBuffer only (the CPU never reads write-combined
//   memory).
class StagingFrame final : public IFrame, public Managed<StagingFrame> {
public:
    StagingFrame(CreationKey<FrameStore> key, FrameDesc desc, FormatId format, Domain domain,
                 std::shared_ptr<gpu::GpuBuffer> buffer, FrameGeometry geometry) noexcept;
    [[nodiscard]] const std::shared_ptr<gpu::GpuBuffer>& buffer() const noexcept { return buffer_; }
    [[nodiscard]] const FrameDesc& desc() const noexcept override { return desc_; }
    [[nodiscard]] std::span<const ViewSpec> nativeViews() const noexcept override;
    [[nodiscard]] Binding bind(const ViewSpec& native) const override;  // HostPlanesB or VkBufferB
    [[nodiscard]] std::size_t bytes() const noexcept override;          // buffer->size()

private:
    FrameDesc desc_;
    std::shared_ptr<gpu::GpuBuffer> buffer_;
    FrameGeometry geometry_;
    std::array<ViewSpec, 2> views_{};
    std::uint8_t view_count_ = 0;
};

// The original in a DeviceOnly buffer, padded for the GPU unpack (M2 design §5.4). One native view:
// (desc.format, desc.colour, Tight, VkBuffer, domain DeviceLocal).
class VkBufferFrame final : public IFrame, public Managed<VkBufferFrame> {
public:
    VkBufferFrame(CreationKey<FrameStore> key, FrameDesc desc, Domain domain,
                  std::shared_ptr<gpu::GpuBuffer> buffer, FrameGeometry geometry) noexcept;
    [[nodiscard]] const FrameDesc& desc() const noexcept override { return desc_; }
    [[nodiscard]] std::span<const ViewSpec> nativeViews() const noexcept override;
    [[nodiscard]] Binding bind(const ViewSpec& native) const override;  // VkBufferB{buffer, 0, ...}
    [[nodiscard]] std::size_t bytes() const noexcept override;          // buffer->size()

private:
    FrameDesc desc_;
    ViewSpec view_;
    std::shared_ptr<gpu::GpuBuffer> buffer_;
    FrameGeometry geometry_;
};

// Device images, layout GENERAL: the unpacked planes (D5, one R8_UINT/R16_UINT image per
// component, colour desc.colour) or the display image (M3 D3, one RGBA16F image, format
// displayFormat(), colour displayColour()). One native view: (format, colour, VkOptimal, VkImage,
// domain DeviceLocal).
class VkPlanesFrame final : public IFrame, public Managed<VkPlanesFrame> {
public:
    VkPlanesFrame(CreationKey<FrameStore> key, FrameDesc desc, FormatId format, ColourId colour,
                  Domain domain, std::array<std::shared_ptr<gpu::GpuImage>, MAX_COMPONENTS> planes,
                  std::uint8_t plane_count) noexcept;
    [[nodiscard]] const FrameDesc& desc() const noexcept override { return desc_; }
    [[nodiscard]] std::span<const ViewSpec> nativeViews() const noexcept override;
    [[nodiscard]] Binding bind(const ViewSpec& native) const override;  // VkImageB
    // geometry(format, desc.width, desc.height)->bytes (0 when unknown).
    [[nodiscard]] std::size_t bytes() const noexcept override { return bytes_; }

private:
    FrameDesc desc_;
    ViewSpec view_;
    VkImageB planes_;
    std::size_t bytes_ = 0;
};

// Handle: key + desc + store (§6.2).
struct FrameRef {
    FrameKey key;
    FrameDesc desc;
    std::shared_ptr<FrameStore> store;
};

}  // namespace dvv::media
