// Compile-only check: sample_grid.h includes no Vulkan header.
#include "dvv/media/sample_grid.h"

#if defined(VULKAN_CORE_H_)
#error "sample_grid.h includes Vulkan"
#endif

static_assert(sizeof(dvv::media::SamplePosition) != 0);
