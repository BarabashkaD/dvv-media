#pragma once

// Frame value types of the media layer (spec §6, M2 design §4.3).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "dvv/base/error.h"
#include "dvv/gpu/device_id.h"

namespace dvv::media {

struct Rational {
    std::int64_t num = 0;
    std::int64_t den = 1;
    bool operator==(const Rational&) const = default;  // plain member equality, no normalisation
};

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

enum class Api : std::uint8_t { HostPlanes, VkBuffer, VkImage /*, AvFrame (M4) */ };
enum class MemKind : std::uint8_t { Host, HostVisible, DeviceLocal };
struct Domain {
    MemKind kind = MemKind::Host;
    gpu::DeviceId device{0};
    gpu::DeviceGeneration generation{};
    bool operator==(const Domain&) const = default;
};
enum class LayoutPolicy : std::uint8_t { Any, Tight, RowAlign256, VkOptimal };
struct ViewSpec {
    FormatId format = 0;
    ColourId colour = 0;
    LayoutPolicy layout = LayoutPolicy::Any;
    Api api = Api::HostPlanes;
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

// UTF-8 text to a path through std::u8string, never through the ANSI code page on Windows (D19).
// Invalid UTF-8 is replaced first (each byte that does not start a well-formed sequence becomes
// U+FFFD), so the conversion never throws.
[[nodiscard]] std::filesystem::path pathFromUtf8(std::string_view utf8);
// The opposite direction for messages and resolved URIs: path.u8string(), never path.string().
// Returns "(path not representable as UTF-8)" if the platform conversion fails.
[[nodiscard]] std::string utf8FromPath(const std::filesystem::path& path);

}  // namespace dvv::media
