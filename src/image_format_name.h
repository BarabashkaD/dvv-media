#pragma once

// The name of an image format, for messages.

#include <vulkan/vulkan_core.h>

namespace dvv::media::detail {

inline const char* imageFormatName(VkFormat format) noexcept {
    switch (format) {
        case VK_FORMAT_R8_UINT: return "R8_UINT";
        case VK_FORMAT_R16_UINT: return "R16_UINT";
        case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
        default: return "another format";
    }
}

}  // namespace dvv::media::detail
