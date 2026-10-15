#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "core/VkUtil.h"

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
    
    VkExtent2D chooseSwapExtent(GLFWwindow* window) {
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
           return capabilities.currentExtent;
       } else {
           int width, height;
           glfwGetFramebufferSize(window, &width, &height);

           VkExtent2D actualExtent = {
               static_cast<uint32_t>(width),
               static_cast<uint32_t>(height)
           };

           actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
           actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

           return actualExtent;
       }
    }
};

SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device, VkSurfaceKHR surface);

#ifdef VK_WRAP_UTIL_IMPL
SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device, VkSurfaceKHR surface) {
    SwapChainSupportDetails details{};
    
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);
    
    details.formats = readVkVector<VkSurfaceFormatKHR, VkPhysicalDevice, VkSurfaceKHR>(device, surface, vkGetPhysicalDeviceSurfaceFormatsKHR);
    details.presentModes = readVkVector<VkPresentModeKHR, VkPhysicalDevice, VkSurfaceKHR>(device, surface, vkGetPhysicalDeviceSurfacePresentModesKHR);

    return details;
}
#endif
