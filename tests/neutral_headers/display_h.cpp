// Compile-only check (spec §6.10): display.h includes no Vulkan header.
#include "dvv/media/display.h"

#if defined(VULKAN_CORE_H_)
#error "display.h includes Vulkan"
#endif

static_assert(sizeof(dvv::media::ResolvedColour) != 0);
