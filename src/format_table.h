#pragma once

// Private declarations of the process-wide format table (pixel_format.cpp, catalogue.cpp).

#include <cstddef>
#include <span>

#include "dvv/base/error.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"

namespace dvv::media::detail {

// The number of built-in formats; their ids are 0..CATALOGUE_SIZE - 1.
inline constexpr std::size_t CATALOGUE_SIZE = 37;

// The 37 built-in descriptors in the order of D3 (catalogue.cpp). Each one passes validate().
[[nodiscard]] std::span<const PixelFormatDescriptor> builtInFormats();

// Interns a validated custom descriptor. Under the table's unique lock: a catalogue name → Format
// "'<name>' is a built-in format"; an interned non-catalogue entry with the same name → its id when
// the descriptors are equal, else Format "a different format named '<name>' is already loaded";
// otherwise appended (unpacked = false).
[[nodiscard]] Result<FormatId> internCustomFormat(const PixelFormatDescriptor& descriptor);

}  // namespace dvv::media::detail
