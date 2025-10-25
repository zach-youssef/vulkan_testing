#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

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

class VulkanInstance final : public VkWrapper<VulkanInstance, VkInstance, const VkInstanceCreateInfo&> {
public:
    VulkanInstance(VkResult& outResult, const VkInstanceCreateInfo& createInfo) {
        outResult = vkCreateInstance(&createInfo, nullptr, &value_);
    }
    
    ~VulkanInstance() {
        vkDestroyInstance(value_, nullptr);
    }
};

class VulkanDevice final : public VkWrapper<VulkanDevice, VkDevice, VkPhysicalDevice, const VkDeviceCreateInfo&> {
public:
    VulkanDevice(VkResult& outResult, VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo& createInfo) {
        outResult = vkCreateDevice(physicalDevice, &createInfo, nullptr, &value_);
    }
    
    ~VulkanDevice() {
        vkDestroyDevice(value_, nullptr);
    }
};

class VulkanSurface final : public VkWrapper<VulkanSurface, VkSurfaceKHR, VkInstance, GLFWwindow*> {
public:
    VulkanSurface(VkResult& outResult, VkInstance instance, GLFWwindow* window): instance_(instance) {
        outResult = glfwCreateWindowSurface(instance, window, nullptr, &value_);
    }
    
    ~VulkanSurface() {
        vkDestroySurfaceKHR(instance_, value_, nullptr);
    }
    
private:
    VkInstance instance_;
};

#define VULKAN_DEVICE_CLASS(ClassName, VkType, CreateInfo, creator, deletor)                            \
class ClassName final : public VkWrapper<ClassName, VkType, VkDevice, const CreateInfo&> {              \
public:                                                                                                 \
    ClassName(VkResult& outResult, VkDevice device, const CreateInfo& createInfo) : device_(device) {   \
        outResult = creator(device, &createInfo, nullptr, &value_);                                     \
    }                                                                                                   \
    ~ClassName() {                                                                                      \
        deletor(device_, value_, nullptr);                                                              \
    }                                                                                                   \
private:                                                                                                \
    VkDevice device_;
// (class intentionally left unclosed to allow additional definitions)

VULKAN_DEVICE_CLASS(VulkanSwapchain, VkSwapchainKHR, VkSwapchainCreateInfoKHR, vkCreateSwapchainKHR, vkDestroySwapchainKHR)
};

VULKAN_DEVICE_CLASS(VulkanImageView, VkImageView, VkImageViewCreateInfo, vkCreateImageView, vkDestroyImageView)
};

VULKAN_DEVICE_CLASS(VulkanRenderPass, VkRenderPass, VkRenderPassCreateInfo, vkCreateRenderPass, vkDestroyRenderPass)
};

VULKAN_DEVICE_CLASS(VulkanPipelineLayout, VkPipelineLayout, VkPipelineLayoutCreateInfo, vkCreatePipelineLayout, vkDestroyPipelineLayout)
};

class VulkanGraphicsPipeline final : public VkWrapper<VulkanGraphicsPipeline, VkPipeline, VkDevice, const VkGraphicsPipelineCreateInfo&> {
public:
    VulkanGraphicsPipeline(VkResult outResult, VkDevice device, const VkGraphicsPipelineCreateInfo& createInfo) : device_(device) {
        outResult = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &createInfo, nullptr, &value_);
    }
    ~VulkanGraphicsPipeline() {
        vkDestroyPipeline(device_, value_, nullptr);
    }
private:
    VkDevice device_;
};

VULKAN_DEVICE_CLASS(VulkanFramebuffer, VkFramebuffer, VkFramebufferCreateInfo, vkCreateFramebuffer, vkDestroyFramebuffer)
};

VULKAN_DEVICE_CLASS(VulkanCommandPool, VkCommandPool, VkCommandPoolCreateInfo, vkCreateCommandPool, vkDestroyCommandPool)
};

VULKAN_DEVICE_CLASS(VulkanSemaphore, VkSemaphore, VkSemaphoreCreateInfo, vkCreateSemaphore, vkDestroySemaphore)
};

VULKAN_DEVICE_CLASS(VulkanFence, VkFence, VkFenceCreateInfo, vkCreateFence, vkDestroyFence)
};

VULKAN_DEVICE_CLASS(VulkanShaderModule, VkShaderModule, VkShaderModuleCreateInfo, vkCreateShaderModule, vkDestroyShaderModule)
};
