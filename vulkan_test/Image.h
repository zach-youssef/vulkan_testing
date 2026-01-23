#pragma once

#include "VkTypes.h"
#include "CommandUtil.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

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
    
    uint32_t getWidth() {
        return width_;
    }
    
    uint32_t getHeight() {
        return height_;
    }
    
    Image(uint32_t width,
          uint32_t height,
          VkFormat format,
          VkImageTiling tiling,
          VkImageUsageFlags usage,
          VkMemoryPropertyFlags properties,
          VkDevice device,
          VkPhysicalDevice physicalDevice,
          VkResult& outResult) : device_(device), width_(width), height_(height) {
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
    
    static void createFromFile(std::unique_ptr<Image>& outImage,
                               const std::string& filePath,
                               VkQueue graphicsQueue,
                               VkCommandPool commandPool,
                               VkDevice device,
                               VkPhysicalDevice physicalDevice) {
        int width, height, channels;
        
        stbi_uc* pixels = stbi_load((filePath).c_str(), &width, &height, &channels, STBI_rgb_alpha);
        
        if (!pixels) {
            throw std::runtime_error("Failed to load texture image.");
        }
        
        VkDeviceSize imageSize = width * height * 4;
        
        std::unique_ptr<Buffer<uint8_t>> stagingBuffer;
        VK_SUCCESS_OR_THROW(Buffer<uint8_t>::create(stagingBuffer, imageSize,
                                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                                    device, physicalDevice),
                            "Failed to create image staging buffer");
        
        stagingBuffer->mapAndExecute(0, imageSize, [pixels, imageSize] (void* data) {
            memcpy(data, pixels, static_cast<size_t>(imageSize));
        });
        
        stbi_image_free(pixels);
        
        VK_SUCCESS_OR_THROW(Image::create(outImage,
                                          width, height,
                                          VK_FORMAT_R8G8B8A8_SRGB,
                                          VK_IMAGE_TILING_OPTIMAL,
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                          device, physicalDevice),
                            "Failed to create image.");
        
        transitionImageLayout(outImage->getImage(),
                              VK_FORMAT_R8G8B8A8_SRGB,
                              VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              graphicsQueue, commandPool, device);

        copyBufferToImage(stagingBuffer->getBuffer(),
                          outImage->getImage(),
                          width, height,
                          graphicsQueue, commandPool, device);

        transitionImageLayout(outImage->getImage(),
                              VK_FORMAT_R8G8B8A8_SRGB,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              graphicsQueue, commandPool, device);
    }
    
    // TODO: Could probably abstract this to make it more useful
    static void createEmptyRGBA(std::unique_ptr<Image>& outImage,
                                uint32_t width,
                                uint32_t height,
                                VkQueue queue,
                                VkCommandPool commandPool,
                                VkDevice device,
                                VkPhysicalDevice physicalDevice) {
        VK_SUCCESS_OR_THROW(Image::create(outImage,
                                          width, height,
                                          VK_FORMAT_R8G8B8A8_SRGB,
                                          VK_IMAGE_TILING_OPTIMAL,
                                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                          device, physicalDevice),
                            "Failed to create image.");
        
        transitionImageLayout(outImage->getImage(),
                              VK_FORMAT_R8G8B8A8_SRGB,
                              VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              queue, commandPool, device);
    }
    
    static void transitionImageLayout(VkImage image,
                                      VkFormat format,
                                      VkImageLayout oldLayout,
                                      VkImageLayout newLayout,
                                      VkQueue graphicsQueue,
                                      VkCommandPool commandPool,
                                      VkDevice device) {
        issueSingleTimeCommand([=](VkCommandBuffer commandBuffer){
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            
            // Must be set. Barrier can be used to transfer queue ownership.
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            
            barrier.image = image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;
            
            VkPipelineStageFlags sourceStage;
            VkPipelineStageFlags destinationStage;

            if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
                barrier.srcAccessMask = 0; // Nothing to wait on
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

                sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; // Earliest possible stage
                destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT; // Not a "real" stage, but is where transfers happen?
            } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                
                sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            } else if(oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                barrier.srcAccessMask = 0; // Nothing to wait on
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                
                sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                destinationStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            } else {
                // TODO better handling
                throw std::invalid_argument("unsupported layout transition!");
            }

            
            vkCmdPipelineBarrier(
                commandBuffer,
                sourceStage,
                destinationStage,
                0,
                0, nullptr,
                0, nullptr,
                1, &barrier
            );
        }, graphicsQueue, device, commandPool);
    }
    
    static void copyBufferToImage(VkBuffer buffer,
                                  VkImage image,
                                  uint32_t width,
                                  uint32_t height,
                                  VkQueue graphicsQueue,
                                  VkCommandPool commandPool,
                                  VkDevice device) {
        issueSingleTimeCommand([=](VkCommandBuffer commandBuffer){
            VkBufferImageCopy region{};
            region.bufferOffset = 0;
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;

            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;

            region.imageOffset = {0, 0, 0};
            region.imageExtent = {
                width,
                height,
                1
            };
            
            vkCmdCopyBufferToImage(
                commandBuffer,
                buffer,
                image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1,
                &region
            );
        }, graphicsQueue, device, commandPool);
    }
private:
    VkDevice device_;
    std::unique_ptr<VulkanImage> image_;
    std::unique_ptr<VulkanMemory> memory_;
    
    std::unique_ptr<VulkanImageView> imageView_;
    std::unique_ptr<VulkanSampler> sampler_;
    
    uint32_t width_;
    uint32_t height_;
};
