// Compile-only check (spec §6.10): types.h includes no Vulkan header.
#include "dvv/media/types.h"

#if defined(VULKAN_CORE_H_)
#error "types.h includes Vulkan"
#endif

static_assert(sizeof(dvv::media::FrameKey) != 0);
