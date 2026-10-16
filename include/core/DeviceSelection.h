#pragma once

#include "core/VkUtil.h"
#include "core/QueueFamilyIndices.h"
#include "core/SwapChainSupportDetails.h"

bool checkDeviceExtensionSupport(VkPhysicalDevice device);
VkPhysicalDeviceFeatures getSupportedFeatures(VkPhysicalDevice device);
bool isDeviceSuitable(VkPhysicalDevice device, VkSurfaceKHR surface);
VkPhysicalDevice pickPhysicalDevice(VkInstance instance, VkSurfaceKHR surface);

#ifdef VK_WRAP_UTIL_IMPL
bool checkDeviceExtensionSupport(VkPhysicalDevice device) {
    auto availableExtensions = readVkVector<VkExtensionProperties, VkPhysicalDevice, const char*>(device, vkEnumerateDeviceExtensionProperties);
    if (!availableExtensions) {
        return false;   // Treated as unsuitable
    }
    std::set<std::string> requiredExtensions(deviceExtensions.begin(), deviceExtensions.end());
    for (const auto& extension : *availableExtensions) {
        requiredExtensions.erase(extension.extensionName);
    }
    return requiredExtensions.empty();
}

VkPhysicalDeviceFeatures getSupportedFeatures(VkPhysicalDevice device) {
    VkPhysicalDeviceFeatures supportedFeatures;
    vkGetPhysicalDeviceFeatures(device, &supportedFeatures);
    return supportedFeatures;
}

// Devices whose support can't be queried are unsuitable
bool isDeviceSuitable(VkPhysicalDevice device, VkSurfaceKHR surface) {
    if (!findQueueFamilies(device, surface).complete() || !checkDeviceExtensionSupport(device)) {
        return false;
    }
    auto support = querySwapChainSupport(device, surface);
    return support && support->adequate() && getSupportedFeatures(device).samplerAnisotropy;
}

// Returns VK_NULL_HANDLE if no suitable device found
VkPhysicalDevice pickPhysicalDevice(VkInstance instance, VkSurfaceKHR surface) {
    auto devices = readVkVector<VkPhysicalDevice, VkInstance>(instance, vkEnumeratePhysicalDevices);
    if (!devices) {
        return VK_NULL_HANDLE;
    }
    
    for (const auto& device : *devices) {
        if (isDeviceSuitable(device, surface)) {
            return device;
        }
    }

    return VK_NULL_HANDLE;
}
#endif
