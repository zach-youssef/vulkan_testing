#pragma once

#include "core/VkUtil.h"

#include <algorithm>
#include <limits>

struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
    
    bool adequate() {
        return !formats.empty() && !presentModes.empty();
    }
    
    VkSurfaceFormatKHR chooseSwapSurfaceFormat(){
        // Prefer an 8-bit sRGB format, so shader output is gamma encoded on presentation
        for (const auto& availableFormat : formats) {
            if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB
                && availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return availableFormat;
            }
        }
        
        return formats[0];
    }
    
    VkPresentModeKHR chooseSwapPresentMode() {
        for (const auto& availableMode : presentModes) {
            if (availableMode == VK_PRESENT_MODE_MAILBOX_KHR) {
                return availableMode;
            }
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    
    // framebufferSize is the window's size in pixels, used when the surface leaves the extent
    // up to the application
    VkExtent2D chooseSwapExtent(VkExtent2D framebufferSize) {
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
           return capabilities.currentExtent;
       } else {
           VkExtent2D actualExtent = framebufferSize;

           actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
           actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

           return actualExtent;
       }
    }
};

Result<SwapChainSupportDetails> querySwapChainSupport(VkPhysicalDevice device, VkSurfaceKHR surface);

#ifdef VK_WRAP_UTIL_IMPL
Result<SwapChainSupportDetails> querySwapChainSupport(VkPhysicalDevice device, VkSurfaceKHR surface) {
    SwapChainSupportDetails details{};
    
    VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities),
           "Failed to query surface capabilities");
    
    TRY_ASSIGN(details.formats, (readVkVector<VkSurfaceFormatKHR, VkPhysicalDevice, VkSurfaceKHR>(device, surface, vkGetPhysicalDeviceSurfaceFormatsKHR)));
    TRY_ASSIGN(details.presentModes, (readVkVector<VkPresentModeKHR, VkPhysicalDevice, VkSurfaceKHR>(device, surface, vkGetPhysicalDeviceSurfacePresentModesKHR)));

    return details;
}
#endif
