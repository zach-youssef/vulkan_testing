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

QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface) {
    QueueFamilyIndices indices;
    
    std::vector<VkQueueFamilyProperties> queueFamilies = readVkVectorVoid<VkQueueFamilyProperties, VkPhysicalDevice>(device, vkGetPhysicalDeviceQueueFamilyProperties);
    for (int i = 0; i < queueFamilies.size(); ++i) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
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
