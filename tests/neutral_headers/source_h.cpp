// Compile-only check (spec §6.10): source.h includes no Vulkan header.
#include "dvv/media/source.h"

#if defined(VULKAN_CORE_H_)
#error "source.h includes Vulkan"
#endif

static_assert(sizeof(dvv::media::SourceInfo) != 0);
