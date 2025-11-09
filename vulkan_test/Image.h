#pragma once

#include "VkTypes.h"

#define RETURN_IF_ERROR(expr)   \
outResult = expr;               \
if (outResult != VK_SUCCESS) {  \
    return;                     \
}

class Image {
public:
    static VkResult create(std::unique_ptr<Image>& outImage,
                           uint32_t width,
                           uint32_t height,
                           VkFormat format,
                           VkImageTiling tiling,
                           VkImageUsageFlags usage,
                           VkMemoryPropertyFlags properties,
                           VkDevice device,
                           VkPhysicalDevice physicalDevice) {
        VkResult result;
        
        outImage = std::make_unique<Image>(width, height,
                                           format, tiling,
                                           usage, properties,
                                           device, physicalDevice,
                                           result);
        
        return result;
    }

    VkImage getImage() {
        return **image_;
    }
    
    VkImageView getImageView() {
        return **imageView_;
    }
    
    Image(uint32_t width,
          uint32_t height,
          VkFormat format,
          VkImageTiling tiling,
          VkImageUsageFlags usage,
          VkMemoryPropertyFlags properties,
          VkDevice device,
          VkPhysicalDevice physicalDevice,
          VkResult& outResult) : device_(device) {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = VkExtent3D{width, height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        
        imageInfo.format = format;
        
        imageInfo.tiling = tiling;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; // first transition discards
        
        imageInfo.usage = usage;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE; // Only used by graphics queue
        
        // Multisampling
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        
        RETURN_IF_ERROR(VulkanImage::create(image_, device_, imageInfo))
        
        VkMemoryRequirements memoryRequirements;
        vkGetImageMemoryRequirements(device_, **image_, &memoryRequirements);
        
        RETURN_IF_ERROR(VulkanMemory::createFromRequirements(memory_,
                                                             device_,
                                                             physicalDevice,
                                                             properties,
                                                             memoryRequirements))
        
        vkBindImageMemory(device_, **image_, **memory_, 0);
        
        RETURN_IF_ERROR(VulkanImageView::createForImageWithFormat(imageView_, device_, **image_, format));
    }
private:
    VkDevice device_;
    std::unique_ptr<VulkanImage> image_;
    std::unique_ptr<VulkanMemory> memory_;
    
    std::unique_ptr<VulkanImageView> imageView_;
    std::unique_ptr<VulkanSampler> sampler_;
};
