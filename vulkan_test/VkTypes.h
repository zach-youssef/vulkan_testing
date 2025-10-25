#pragma once

#include <vulkan/vulkan.h>

template<typename WrapperType, typename VkType, class... Args>
class VkWrapper {
public:
    VkType operator * () {
        return value_;
    }
    
    VkType* get() {
        return &value_;
    }
    
    static VkResult create(std::unique_ptr<WrapperType>& outPtr, Args... args) {
        VkResult result;
        outPtr = std::make_unique<WrapperType>(result, args...);
        if (result != VK_SUCCESS) {
            outPtr = nullptr;
        }
        return result;
    }
protected:
    VkType value_;
};

// Test impl
class VulkanInstance final : public VkWrapper<VulkanInstance, VkInstance, const VkInstanceCreateInfo&> {
public:
    VulkanInstance(VkResult& outResult, const VkInstanceCreateInfo& createInfo) {
        outResult = vkCreateInstance(&createInfo, nullptr, &value_);
    }
    
    ~VulkanInstance() {
        vkDestroyInstance(value_, nullptr);
    }
};
