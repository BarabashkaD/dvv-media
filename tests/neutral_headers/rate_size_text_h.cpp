// Compile-only check (spec §6.10): rate_size_text.h includes no Vulkan header.
#include "dvv/media/rate_size_text.h"

#if defined(VULKAN_CORE_H_)
#error "rate_size_text.h includes Vulkan"
#endif

static_assert(sizeof(dvv::media::Rational) != 0);
