#pragma once

// The canonical form of a hidden component set: a store identity rule (one display entry per
// distinct picture), defined in to_display.cpp next to the pass that applies the set.

#include "dvv/media/types.h"

namespace dvv::media::detail {

// The canonical form of `hidden` for a frame of `desc`: components the format lacks are cleared
// (a format with fewer than three colour components keeps only components[0]), and `chroma` is
// Mid unless hasChroma() holds under resolveColour(desc) and components[1] or [2] is hidden.
// Equal pictures then have equal sets.
[[nodiscard]] HiddenComponents canonicalHidden(const FrameDesc& desc, HiddenComponents hidden);

}  // namespace dvv::media::detail
