#pragma once

// The display alpha of a format, shared by ToDisplay's push constants (to_display.cpp) and the CPU
// reference (display.cpp, toDisplayReference).

#include "dvv/media/types.h"

namespace dvv::media::detail {

// 1 / (2^bits - 1) of the format's A component (the last one, present when componentCount is 2 or
// 4), so that code * scale is the straight, full-range alpha in [0, 1]; 0 for a format without A
// or an unknown id.
[[nodiscard]] float alphaScaleOf(FormatId format) noexcept;

}  // namespace dvv::media::detail
