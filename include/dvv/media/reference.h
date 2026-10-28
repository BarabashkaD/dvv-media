#pragma once

// The scalar CPU reference (slow, obviously correct), spec §7.3: the ground truth for tests and the
// CPU fallback (CpuUnpack, Task 8).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"

namespace dvv::media {

// One plane of code values per component, row-major, tight, value range [0, 2^bits).
struct CodeValues {
    FormatId format = 0;
    std::uint32_t width = 0, height = 0;
    std::array<std::vector<std::uint16_t>, MAX_COMPONENTS> components{};
    bool operator==(const CodeValues&) const = default;
};

// Packs code values into the format's tight layout (geometry()); padding bits, row padding and
// samples beyond the width are 0. Errc::Internal for a value out of range or sizes that do not
// match; geometry() errors are passed through.
[[nodiscard]] Result<std::vector<std::byte>> pack(const CodeValues& values);
// Unpacks original bytes (at least geometry().bytes, tight layout) into code values.
[[nodiscard]] Result<CodeValues> unpack(FormatId format, std::uint32_t width, std::uint32_t height,
                                        std::span<const std::byte> original);
// The host form of the unpacked view (D5): planes in component order, LE 8/16-bit, tight, i.e. the
// layout of unpackedFormat(format). `planes.size()` must equal
// geometry(unpackedFormat(format)).bytes.
[[nodiscard]] Result<void> unpackToPlanes(FormatId format, std::uint32_t width,
                                          std::uint32_t height, std::span<const std::byte> original,
                                          std::span<std::byte> planes);

}  // namespace dvv::media
