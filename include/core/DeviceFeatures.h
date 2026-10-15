#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

/*
 The device features the wrapper requires, as a VkPhysicalDeviceFeatures2 chain.

 Vulkan 1.3 is the minimum version. Beyond it, the wrapper relies on:
    - samplerAnisotropy: samplers are always created with anisotropic filtering
    - timelineSemaphore: per-queue timelines synchronize submissions and frames
    - synchronization2: barriers and submissions use the synchronization2 structures
    - dynamicRendering: render targets are begun with vkCmdBeginRendering, without render passes
 */
class DeviceFeatures {
public:
    DeviceFeatures() {
        link();
    }

    DeviceFeatures(const DeviceFeatures& other)
    : core(other.core), vulkan12(other.vulkan12), vulkan13(other.vulkan13) {
        link();
    }

    DeviceFeatures& operator=(const DeviceFeatures& other) {
        core = other.core;
        vulkan12 = other.vulkan12;
        vulkan13 = other.vulkan13;
        link();
        return *this;
    }

    // The features a device supports
    static DeviceFeatures supported(VkPhysicalDevice device) {
        DeviceFeatures features;
        vkGetPhysicalDeviceFeatures2(device, &features.core);
        return features;
    }

    // The features the wrapper enables (all of which are required)
    static DeviceFeatures required() {
        DeviceFeatures features;
        features.core.features.samplerAnisotropy = VK_TRUE;
        features.vulkan12.timelineSemaphore = VK_TRUE;
        features.vulkan13.synchronization2 = VK_TRUE;
        features.vulkan13.dynamicRendering = VK_TRUE;
        return features;
    }

    // Names of the required features this set lacks; empty if it has them all
    std::vector<std::string> missingRequired() const {
        std::vector<std::string> missing;
        const DeviceFeatures needed = required();
        auto check = [&missing](VkBool32 need, VkBool32 have, const char* name) {
            if (need && !have) {
                missing.emplace_back(name);
            }
        };
        check(needed.core.features.samplerAnisotropy, core.features.samplerAnisotropy, "samplerAnisotropy");
        check(needed.vulkan12.timelineSemaphore, vulkan12.timelineSemaphore, "timelineSemaphore");
        check(needed.vulkan13.synchronization2, vulkan13.synchronization2, "synchronization2");
        check(needed.vulkan13.dynamicRendering, vulkan13.dynamicRendering, "dynamicRendering");
        return missing;
    }

    // The head of the chain, e.g. for VkDeviceCreateInfo::pNext
    const VkPhysicalDeviceFeatures2* chain() const {
        return &core;
    }

    VkPhysicalDeviceFeatures2 core{};
    VkPhysicalDeviceVulkan12Features vulkan12{};
    VkPhysicalDeviceVulkan13Features vulkan13{};

private:
    void link() {
        core.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        vulkan13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        core.pNext = &vulkan12;
        vulkan12.pNext = &vulkan13;
        vulkan13.pNext = nullptr;
    }
};

// Why a device can't be used by the wrapper (an API version or missing features); empty if it can
inline std::vector<std::string> deviceShortcomings(VkPhysicalDevice device) {
    std::vector<std::string> shortcomings;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_3) {
        shortcomings.emplace_back("Vulkan 1.3");
        return shortcomings; // Its features can't be queried through the 1.3 structures
    }
    for (auto& feature : DeviceFeatures::supported(device).missingRequired()) {
        shortcomings.push_back(feature);
    }
    return shortcomings;
}
