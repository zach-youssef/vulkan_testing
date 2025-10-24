#pragma once

#include <vulkan/vulkan.h>

template<typename VkType>
class VkWrapper {
public:
    VkType operator * () {
        return value_;
    }
    
    VkType* get() {
        return &value_;
    }
    
protected:
    VkType value_;
};

// Test impl
class VulkanInstance final : public VkWrapper<VkInstance> {
public:
    static VkResult createVulkanInstance(const VkInstanceCreateInfo& createInfo, std::unique_ptr<VulkanInstance>& outPtr) {
        VkResult result;
        outPtr = std::make_unique<VulkanInstance>(createInfo, result);
        if (result != VK_SUCCESS) {
            outPtr = nullptr;
        }
        return result;
    }
    ~VulkanInstance() {
        vkDestroyInstance(value_, nullptr);
    }
    VulkanInstance(const VkInstanceCreateInfo& createInfo, VkResult& outResult) {
        outResult = vkCreateInstance(&createInfo, nullptr, &value_);
    }
};

