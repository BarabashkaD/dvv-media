#pragma once

// Frame value types of the media layer.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "dvv/base/error.h"
#include "dvv/gpu/device_id.h"

namespace dvv::media {

struct Rational {
    std::int64_t num = 0;
    std::int64_t den = 1;
    bool operator==(const Rational&) const = default;  // plain member equality, no normalisation
};

// The frame rate a source reports when its input states none (a raw file without fps, a Y4M
// header without F, a container without a rate).
inline constexpr Rational FALLBACK_FPS{25, 1};

using FormatId = std::uint32_t;  // index into the process-wide format table (catalogue first)
using ColourId = std::uint32_t;  // index into the process-wide colour table (0 = all Unspecified)
using OriginId = std::uint64_t;  // 0 = none

struct FrameKey {
    OriginId origin = 0;
    std::int64_t index = 0;
    bool operator==(const FrameKey&) const = default;
};
struct FrameKeyHash {
    [[nodiscard]] std::size_t operator()(const FrameKey& key) const noexcept;
};
struct FrameDesc {
    FormatId format = 0;
    ColourId colour = 0;
    std::uint32_t width = 0, height = 0;
    bool operator==(const FrameDesc&) const = default;
};

// The kind of a view's binding. The kinds dvv-media defines: HOST_PLANES here, vulkan::BUFFER and
// vulkan::IMAGE in dvv/media/frame_vulkan.h. Two kinds are equal when their names are equal.
class BindingKind {
public:
    explicit consteval BindingKind(std::string_view name) noexcept : name_(name) {}
    [[nodiscard]] constexpr std::string_view name() const noexcept { return name_; }  // messages
    friend constexpr bool operator==(const BindingKind&, const BindingKind&) noexcept = default;

private:
    std::string_view name_;
};
// Read-only host bytes per plane (HostPlanesB).
inline constexpr BindingKind HOST_PLANES{"HostPlanes"};

enum class MemKind : std::uint8_t { Host, HostVisible, DeviceLocal };
struct Domain {
    MemKind kind = MemKind::Host;
    gpu::DeviceId device{0};
    gpu::DeviceGeneration generation{};
    bool operator==(const Domain&) const = default;
};
enum class LayoutPolicy : std::uint8_t {
    Any,
    Tight,
    Opaque,  // a backend-defined layout the host does not address
};
struct ViewSpec {
    FormatId format = 0;
    ColourId colour = 0;
    LayoutPolicy layout = LayoutPolicy::Any;
    BindingKind binding = HOST_PLANES;
    Domain domain{};
    std::uint8_t downscaleLog2 = 0;
    bool operator==(const ViewSpec&) const = default;
};
enum class Fidelity : std::uint8_t { Exact, Lossless, Derived };
struct SyncPoint {
    gpu::DeviceId device{0};
    std::uint8_t timeline = 0;
    std::uint64_t value = 0;
};

enum class ColourMatrix : std::uint8_t { Unspecified, Bt601, Bt709, Bt2020Ncl, Rgb, Custom };
enum class ColourRange : std::uint8_t { Unspecified, Limited, Full };
enum class TransferFn : std::uint8_t { Unspecified, Srgb, Bt1886, Pq, Hlg, Linear };
// NOLINTNEXTLINE(readability-identifier-naming): BT.601 525-line and 625-line, as in the design
enum class Primaries : std::uint8_t { Unspecified, Bt601_525, Bt601_625, Bt709, Bt2020 };
struct ColourParams {
    ColourMatrix matrix{};
    ColourRange range{};
    TransferFn transfer{};
    Primaries primaries{};
    // Luma weights of the custom matrix (ColourMatrix::Custom only); interned by value.
    float kr = 0;
    float kb = 0;
    bool operator==(const ColourParams&) const = default;
};

// Errc::Format unless: with ColourMatrix::Custom, kr and kb are finite, kr > 0, kb > 0 and
// kr + kb < 1 ("custom matrix needs kr > 0, kb > 0 and kr + kb < 1 (kr=<kr>, kb=<kb>)"); with any
// other matrix, kr == kb == 0 ("kr and kb apply only to the custom matrix").
[[nodiscard]] Result<void> validateColour(const ColourParams& params);

// Thread-safe. Equal parameters always give the same id; ColourParams{} is id 0.
[[nodiscard]] ColourId internColour(const ColourParams& params);
// A stable reference (entries are never moved or removed). An unknown id gives the parameters of
// id 0 (all Unspecified).
[[nodiscard]] const ColourParams& colourParams(ColourId id) noexcept;

}  // namespace dvv::media
