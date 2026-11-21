// Compile-only check (spec §6.10): frame_store.h includes no Vulkan header.
#include "dvv/media/frame_store.h"

#if defined(VULKAN_CORE_H_)
#error "frame_store.h includes Vulkan"
#endif

static_assert(sizeof(dvv::media::StoreStats) != 0);
