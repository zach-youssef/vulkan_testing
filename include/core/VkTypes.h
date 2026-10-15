#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <memory>
#include <vector>
#include <optional>
#include <iostream>

#include <vulkan/vk_enum_string_helper.h>

#include "core/ResourceManager.h"

class VulkanContext;

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

class VulkanSurface final : public VkWrapper<VulkanSurface, VkSurfaceKHR> {
public:
    // Takes ownership of an already created surface
    VulkanSurface(VkInstance instance, VkSurfaceKHR surface): instance_(instance) {
        value_ = surface;
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
        if (value_ != VK_NULL_HANDLE) {                                                                 \
            deletor(device_, value_, nullptr);                                                          \
        }                                                                                               \
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
                                             VkFormat format,
                                            VkImageAspectFlagBits aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = aspect;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        
        return VulkanImageView::create(outPtr, device, viewInfo);
    }
};

VULKAN_DEVICE_CLASS(VulkanPipelineLayout, VkPipelineLayout, VkPipelineLayoutCreateInfo, vkCreatePipelineLayout, vkDestroyPipelineLayout)
};

class VulkanGraphicsPipeline final : public VkWrapper<VulkanGraphicsPipeline, VkPipeline, VkDevice, const VkGraphicsPipelineCreateInfo&> {
public:
    VulkanGraphicsPipeline(VkResult& outResult, VkDevice device, const VkGraphicsPipelineCreateInfo& createInfo) : device_(device) {
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
    VulkanComputePipeline(VkResult& outResult, VkDevice device, const VkComputePipelineCreateInfo& createInfo): device_(device) {
        outResult = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &createInfo, nullptr, &value_);
    }
    ~VulkanComputePipeline() {
        vkDestroyPipeline(device_, value_, nullptr);
    }
private:
    VkDevice device_;
};

VULKAN_DEVICE_CLASS(VulkanCommandPool, VkCommandPool, VkCommandPoolCreateInfo, vkCreateCommandPool, vkDestroyCommandPool)
};

VULKAN_DEVICE_CLASS(VulkanSemaphore, VkSemaphore, VkSemaphoreCreateInfo, vkCreateSemaphore, vkDestroySemaphore)
public:
    VulkanSemaphore() {
        value_ = VK_NULL_HANDLE;
    }
};

VULKAN_DEVICE_CLASS(VulkanFence, VkFence, VkFenceCreateInfo, vkCreateFence, vkDestroyFence)
public:
    VulkanFence() {
        value_ = VK_NULL_HANDLE;
    }
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

// How a sampler filters and addresses images
struct SamplerDesc {
    VkFilter filter = VK_FILTER_LINEAR;   // Magnification and minification
    VkSamplerMipmapMode mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    VkSamplerAddressMode addressMode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkBorderColor borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;   // For CLAMP_TO_BORDER
    // Depth comparison (e.g. VK_COMPARE_OP_LESS_OR_EQUAL for shadow maps, sampled with
    // sampler2DShadow): sampling returns the fraction of texels passing instead of their values
    std::optional<VkCompareOp> compareOp;
    // The device's maximum anisotropy, or none
    bool anisotropy = true;
    float maxLod = 0.0f;
};

VULKAN_DEVICE_CLASS(VulkanSampler, VkSampler, VkSamplerCreateInfo, vkCreateSampler, vkDestroySampler)
public:
    // From a VkSamplerCreateInfo
    using VkWrapper<VulkanSampler, VkSampler, VkDevice, const VkSamplerCreateInfo&>::create;

    static VkResult create(std::unique_ptr<VulkanSampler>& outSampler,
                           const SamplerDesc& desc,
                           VkDevice device,
                           VkPhysicalDevice physicalDevice) {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = desc.filter;
        samplerInfo.minFilter = desc.filter;
        samplerInfo.mipmapMode = desc.mipmapMode;
        samplerInfo.addressModeU = desc.addressMode;
        samplerInfo.addressModeV = desc.addressMode;
        samplerInfo.addressModeW = desc.addressMode;
        samplerInfo.borderColor = desc.borderColor;
        if (desc.anisotropy) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(physicalDevice, &properties);
            samplerInfo.anisotropyEnable = VK_TRUE;
            samplerInfo.maxAnisotropy = properties.limits.maxSamplerAnisotropy;
        }
        samplerInfo.compareEnable = desc.compareOp.has_value();
        samplerInfo.compareOp = desc.compareOp.value_or(VK_COMPARE_OP_ALWAYS);
        samplerInfo.maxLod = desc.maxLod;
        return VulkanSampler::create(outSampler, device, samplerInfo);
    }

    // Managed samplers, owned by the context's ResourceManager. Defined in VulkanContext.h.
    static VkResult create(Handle<VulkanSampler>& outSampler, VulkanContext& context, const SamplerDesc& desc);

    static VkResult createWithAddressMode(std::unique_ptr<VulkanSampler>& outSampler,
                                          VkSamplerAddressMode addressMode,
                                          VkDevice device,
                                          VkPhysicalDevice physicalDevice) {
        return createWithModeAndFilter(outSampler, addressMode, VK_FILTER_LINEAR, device, physicalDevice);
    }
    
    // Shorthands for SamplerDesc with an address mode (and filter)
    static VkResult createWithAddressMode(Handle<VulkanSampler>& outSampler,
                                          VulkanContext& context,
                                          VkSamplerAddressMode addressMode);
    
    static VkResult createWithModeAndFilter(Handle<VulkanSampler>& outSampler,
                                            VulkanContext& context,
                                            VkSamplerAddressMode addressMode,
                                            VkFilter filter);

    static VkResult createWithModeAndFilter(std::unique_ptr<VulkanSampler>& outSampler,
                                            VkSamplerAddressMode addressMode,
                                            VkFilter filter,
                                            VkDevice device,
                                            VkPhysicalDevice physicalDevice) {
        return create(outSampler, SamplerDesc{.filter = filter, .addressMode = addressMode}, device, physicalDevice);
    }
};
