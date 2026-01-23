#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <memory>
#include <optional>
#include <iostream>

#include <vulkan/vk_enum_string_helper.h>

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
            std::cerr << string_VkResult(result) << std::endl;
            //outPtr = nullptr;
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
public:
    static VkResult createForImageWithFormat(std::unique_ptr<VulkanImageView>& outPtr,
                                             VkDevice device,
                                             VkImage image,
                                             VkFormat format) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        
        return VulkanImageView::create(outPtr, device, viewInfo);
    }
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

class VulkanComputePipeline final : public VkWrapper<VulkanComputePipeline, VkPipeline, VkDevice, const VkComputePipelineCreateInfo&> {
public:
    VulkanComputePipeline(VkResult outResult, VkDevice device, const VkComputePipelineCreateInfo& createInfo): device_(device) {
        outResult = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &createInfo, nullptr, &value_);
    }
    ~VulkanComputePipeline() {
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

VULKAN_DEVICE_CLASS(VulkanBuffer, VkBuffer, VkBufferCreateInfo, vkCreateBuffer, vkDestroyBuffer)
};

VULKAN_DEVICE_CLASS(VulkanMemory, VkDeviceMemory, VkMemoryAllocateInfo, vkAllocateMemory, vkFreeMemory)
public:
    static VkResult createFromRequirements(std::unique_ptr<VulkanMemory>& outPtr,
                                           VkDevice device,
                                           VkPhysicalDevice physicalDevice,
                                           VkMemoryPropertyFlags properties,
                                           const VkMemoryRequirements& memoryRequirements) {
        auto memoryTypeIndex = findMemoryType(physicalDevice, memoryRequirements.memoryTypeBits, properties);
        
        if (!memoryTypeIndex.has_value()) {
            return VK_ERROR_UNKNOWN; // TODO better handling
        }
        
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memoryRequirements.size;
        allocInfo.memoryTypeIndex = memoryTypeIndex.value();
        
        return VulkanMemory::create(outPtr, device, allocInfo);
    }
private:
    static std::optional<uint32_t> findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties){
        VkPhysicalDeviceMemoryProperties memoryProperties;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
        
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            if (typeFilter & (1 << i)
                && (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        
        return {};
    }
};

VULKAN_DEVICE_CLASS(VulkanDescriptorSetLayout, VkDescriptorSetLayout, VkDescriptorSetLayoutCreateInfo, vkCreateDescriptorSetLayout, vkDestroyDescriptorSetLayout)
};

VULKAN_DEVICE_CLASS(VulkanDescriptorPool, VkDescriptorPool, VkDescriptorPoolCreateInfo, vkCreateDescriptorPool, vkDestroyDescriptorPool)
};

VULKAN_DEVICE_CLASS(VulkanImage, VkImage, VkImageCreateInfo, vkCreateImage, vkDestroyImage)
};

VULKAN_DEVICE_CLASS(VulkanSampler, VkSampler, VkSamplerCreateInfo, vkCreateSampler, vkDestroySampler)
public:
    static VkResult createWithAddressMode(std::unique_ptr<VulkanSampler>& outSampler,
                                          VkSamplerAddressMode addressMode,
                                          VkDevice device,
                                          VkPhysicalDevice physicalDevice) {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        
        // Alternative is VK_FILTER_NEAREST
        // Mag is for oversampling, min is for undersampling
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        
        // Alternatives are
        // - MIRRORED_REPEAT
        // - CLAMP_TO_EDGE (or MIRRORED_)
        // CLAMP_TO_BORDER
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        
        // Can be set to false if device doesn't support it
        samplerInfo.anisotropyEnable = VK_TRUE;
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice, &properties);
        samplerInfo.maxAnisotropy = properties.limits.maxSamplerAnisotropy;
        
        // Only relevant if using CLAMP_TO_BORDER adress mode
        samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        
        // Use real coordinates instead of UVs
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        
        // I don't understand this but it has some use with shadow maps?
        samplerInfo.compareEnable = VK_FALSE;
        samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
        
        // To be revisted when we implement mip maps
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.mipLodBias = 0.0f;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = 0.0f;
        
        return VulkanSampler::create(outSampler, device, samplerInfo);
    }
};
