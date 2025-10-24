#pragma once

#include "VkUtil.h"
#include "QueueFamilyIndices.h"
#include "SwapChainSupportDetails.h"

bool checkDeviceExtensionSupport(VkPhysicalDevice device) {
    std::vector<VkExtensionProperties> availableExtensions = readVkVector<VkExtensionProperties, VkPhysicalDevice, const char*>(device, vkEnumerateDeviceExtensionProperties);
    std::set<std::string> requiredExtensions(deviceExtensions.begin(), deviceExtensions.end());
    for (const auto& extension : availableExtensions) {
        requiredExtensions.erase(extension.extensionName);
    }
    return requiredExtensions.empty();
}

bool isDeviceSuitable(VkPhysicalDevice device, Handle<VkSurfaceKHR>& surface) {
    return findQueueFamilies(device, surface).complete() && checkDeviceExtensionSupport(device) && querySwapChainSupport(device, surface).adequate();
}

// Returns VK_NULL_HANDLE if no suitable device found
VkPhysicalDevice pickPhysicalDevice(VkInstance instance, Handle<VkSurfaceKHR>& surface) {
    std::vector<VkPhysicalDevice> devices = readVkVector<VkPhysicalDevice, VkInstance>(instance, vkEnumeratePhysicalDevices);
    if (devices.size() == 0) {
        //throw std::runtime_error("Failed to find GPU with Vulkan support.");
        return VK_NULL_HANDLE;
    }
    
    for (const auto& device : devices) {
        if (isDeviceSuitable(device, surface)) {
            return device;
        }
    }

    return VK_NULL_HANDLE;
}
