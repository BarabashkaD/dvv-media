// Compile-only check: view_lines.h includes no Vulkan header.
#include "dvv/media/view_lines.h"

#if defined(VULKAN_CORE_H_)
#error "view_lines.h includes Vulkan"
#endif

static_assert(sizeof(dvv::media::AxisView) != 0);
