#pragma once

// The GPU frames the store makes (private: only the store makes them, and the lease readers give
// everything a user reads from them).

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "dvv/base/lifetime.h"
#include "dvv/gpu/gpu_fwd.h"
#include "dvv/media/frame.h"
#include "dvv/media/frame_vulkan.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"

namespace dvv::media {

// A frame in one host-visible GpuBuffer holding the tight layout of `format` (desc.format or
// unpackedFormat(desc.format)) at offset 0. Native views (format, desc.colour, Tight, domain):
// - HostReadback memory (made by Readback) with its read scope: HostPlanes and VkBuffer, the same
//   bytes (one store entry, counted once); without a scope: VkBuffer only (no empty planes);
// - HostUpload memory (made by HostToStaging): VkBuffer only (the CPU never reads write-combined
//   memory).
class StagingFrame final : public IFrame, public Managed<StagingFrame> {
public:
    // `host_read`: the read scope of `buffer` for HostReadback memory (made by mapRead() after
    // the GPU copy completed); null for HostUpload memory. The HostPlanes view is offered only
    // when it is set.
    StagingFrame(CreationKey<FrameStore> key, FrameDesc desc, FormatId format, Domain domain,
                 std::shared_ptr<gpu::GpuBuffer> buffer, FrameGeometry geometry,
                 std::unique_ptr<gpu::MappedRead> host_read) noexcept;  // frames.cpp
    ~StagingFrame() override;                                           // frames.cpp
    // Both are defined in frames.cpp, where MappedRead is complete: destroying the by-value
    // parameter and the member needs the complete type.
    [[nodiscard]] const FrameDesc& desc() const noexcept override { return desc_; }
    [[nodiscard]] std::span<const ViewSpec> nativeViews() const noexcept override;
    // The host planes, or the buffer binding.
    [[nodiscard]] Binding bind(const ViewSpec& native) const override;
    [[nodiscard]] std::size_t bytes() const noexcept override;  // buffer->size()

private:
    FrameDesc desc_;
    std::shared_ptr<gpu::GpuBuffer> buffer_;
    // Made from buffer_ in the constructor's init list: declared after it.
    Binding buffer_binding_;
    FrameGeometry geometry_;
    std::array<ViewSpec, 2> views_{};
    std::uint8_t view_count_ = 0;
    // A scope of buffer_: declared after it, so it is destroyed first.
    std::unique_ptr<gpu::MappedRead> host_read_;
};

// The original in a DeviceOnly buffer, padded for the GPU unpack. One native view:
// (desc.format, desc.colour, Tight, vulkan::BUFFER, domain DeviceLocal).
class VkBufferFrame final : public IFrame, public Managed<VkBufferFrame> {
public:
    VkBufferFrame(CreationKey<FrameStore> key, FrameDesc desc, Domain domain,
                  std::shared_ptr<gpu::GpuBuffer> buffer) noexcept;
    [[nodiscard]] const FrameDesc& desc() const noexcept override { return desc_; }
    [[nodiscard]] std::span<const ViewSpec> nativeViews() const noexcept override;
    [[nodiscard]] Binding bind(const ViewSpec& native) const override;  // the buffer binding
    [[nodiscard]] std::size_t bytes() const noexcept override;          // buffer->size()

private:
    FrameDesc desc_;
    ViewSpec view_;
    std::shared_ptr<gpu::GpuBuffer> buffer_;
    // Made from buffer_ in the constructor's init list: declared after it.
    Binding buffer_binding_;
};

// Device images, layout GENERAL: the unpacked planes (D5, one R8_UINT/R16_UINT image per
// component, colour desc.colour) or the display image (M3 D3, one RGBA16F image, format
// displayFormat(), colour displayColour()). One native view: (format, colour, Opaque,
// vulkan::IMAGE, domain DeviceLocal).
class VkPlanesFrame final : public IFrame, public Managed<VkPlanesFrame> {
public:
    VkPlanesFrame(CreationKey<FrameStore> key, FrameDesc desc, FormatId format, ColourId colour,
                  Domain domain, std::array<std::shared_ptr<gpu::GpuImage>, MAX_COMPONENTS> planes,
                  std::uint8_t plane_count) noexcept;
    [[nodiscard]] const FrameDesc& desc() const noexcept override { return desc_; }
    [[nodiscard]] std::span<const ViewSpec> nativeViews() const noexcept override;
    [[nodiscard]] Binding bind(const ViewSpec& native) const override;  // the image binding
    // geometry(format, desc.width, desc.height)->bytes (0 when unknown).
    [[nodiscard]] std::size_t bytes() const noexcept override { return bytes_; }

private:
    FrameDesc desc_;
    ViewSpec view_;
    Binding binding_;
    std::size_t bytes_ = 0;
};

}  // namespace dvv::media
