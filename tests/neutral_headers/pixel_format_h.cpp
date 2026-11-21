// Compile-only check (spec §6.10): pixel_format.h includes no Vulkan header.
#include "dvv/media/pixel_format.h"

#if defined(VULKAN_CORE_H_)
#error "pixel_format.h includes Vulkan"
#endif

static_assert(sizeof(dvv::media::PixelFormatDesc) != 0);
