#pragma once

#include "VkUtil.h"

#include <set>

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;
    
    bool complete() {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
    
    std::set<uint32_t> uniqueQueueFamilies() {
        if (!complete()){
            return {};
        } else {
            return {graphicsFamily.value(), presentFamily.value()};
        }
    }
};

QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface);

#ifdef VK_WRAP_UTIL_IMPL
QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface) {
    QueueFamilyIndices indices;
    
    std::vector<VkQueueFamilyProperties> queueFamilies = readVkVectorVoid<VkQueueFamilyProperties, VkPhysicalDevice>(device, vkGetPhysicalDeviceQueueFamilyProperties);
    for (int i = 0; i < queueFamilies.size(); ++i) {
        // Also requiring compute queue to avoid having to synchronize dedicated async compute
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT
            && queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            indices.graphicsFamily = i;
        }
        
        VkBool32 presentSupport = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
        if (presentSupport) {
            indices.presentFamily = i;
        }

        // Stop looking if we've found all the families we need
        if (indices.complete()) {
            break;
        }
    }
    
    return indices;
}
#endif
