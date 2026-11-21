#pragma once
#include <vulkan/vulkan_core.h>

namespace dvv::media {
// The GPU format of the display view's image (displayView(), display.h): RGBA16F, linear light,
// layout GENERAL. A shader that binds the image declares format "rgba16f" and static_asserts
// against this constant.
inline constexpr VkFormat DISPLAY_IMAGE_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
}  // namespace dvv::media
