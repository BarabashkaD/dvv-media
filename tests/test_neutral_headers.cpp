// Compile-only check (spec §6.10): frame.h includes no Vulkan header.
#include "dvv/media/frame.h"

#if defined(VULKAN_CORE_H_)
#error "frame.h includes Vulkan"
#endif

static_assert(sizeof(dvv::media::HostFrame) != 0 && sizeof(dvv::media::HostPlanesB) != 0);
