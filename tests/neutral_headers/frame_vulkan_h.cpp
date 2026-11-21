// Compile-only check (spec §6.5): frame_vulkan.h is self-contained.
#include <cstddef>

#include "dvv/media/frame_vulkan.h"

static_assert(sizeof(dvv::media::ReadLease) != 0);
