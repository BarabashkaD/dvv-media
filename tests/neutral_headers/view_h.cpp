// Compile-only check: view.h includes no Vulkan header.
#include "dvv/media/view.h"

#if defined(VULKAN_CORE_H_)
#error "view.h includes Vulkan"
#endif

static_assert(sizeof(dvv::media::ViewTransform) != 0);
