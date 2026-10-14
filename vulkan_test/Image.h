#pragma once

#include "VkTypes.h"
#include "VkUtil.h"
#include "CommandUtil.h"
#include "Buffer.h"

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
                           VkPhysicalDevice physicalDevice,
                           VkImageAspectFlagBits aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        VkResult result;
        
        outImage = std::make_unique<Image>(width, height,
                                           format, tiling,
                                           usage, properties,
                                           device, physicalDevice,
                                           result, aspect);
        
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
    
    VkFormat getFormat() {
        return format_;
    }
    
    // Size in bytes of a single texel for the formats this wrapper supports reading back
    static VkDeviceSize bytesPerPixel(VkFormat format) {
        switch (format) {
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_SRGB:
            case VK_FORMAT_D32_SFLOAT:
                return 4;
            case VK_FORMAT_R32G32B32A32_SFLOAT:
                return 16;
            default:
                throw std::invalid_argument("Unsupported format for bytesPerPixel");
        }
    }
    
    Image(uint32_t width,
          uint32_t height,
          VkFormat format,
          VkImageTiling tiling,
          VkImageUsageFlags usage,
          VkMemoryPropertyFlags properties,
          VkDevice device,
          VkPhysicalDevice physicalDevice,
          VkResult& outResult,
          VkImageAspectFlagBits aspect = VK_IMAGE_ASPECT_COLOR_BIT) : device_(device), format_(format), width_(width), height_(height) {
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
        
        RETURN_IF_ERROR(VulkanImageView::createForImageWithFormat(imageView_, device_, **image_, format, aspect));
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
        
        createFromUcharBuffer(outImage,
                              pixels,
                              width,
                              height,
                              imageSize,
                              graphicsQueue,
                              commandPool,
                              device,
                              physicalDevice);
        
        stbi_image_free(pixels);
    }
    
    static void createFromUcharBuffer(std::unique_ptr<Image>& outImage,
                                      unsigned char* pixels,
                                      uint32_t width,
                                      uint32_t height,
                                      VkDeviceSize imageSize,
                                      VkQueue graphicsQueue,
                                      VkCommandPool commandPool,
                                      VkDevice device,
                                      VkPhysicalDevice physicalDevice) {
        VK_SUCCESS_OR_THROW(Image::create(outImage,
                                          width, height,
                                          VK_FORMAT_R8G8B8A8_SRGB,
                                          VK_IMAGE_TILING_OPTIMAL,
                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                          device, physicalDevice),
                            "Failed to create image.");

        outImage->uploadUcharBufferToImage(pixels, imageSize, graphicsQueue, commandPool, physicalDevice);
    }
    
    static void createEmptyRGBA(std::unique_ptr<Image>& outImage,
                                uint32_t width,
                                uint32_t height,
                                VkQueue queue,
                                VkCommandPool commandPool,
                                VkDevice device,
                                VkPhysicalDevice physicalDevice) {
        createEmpty(outImage, VK_FORMAT_R8G8B8A8_SRGB, width, height, queue, commandPool, device, physicalDevice);
    }

    static void createEmpty(std::unique_ptr<Image>& outImage,
                            VkFormat format,
                            uint32_t width,
                            uint32_t height,
                            VkQueue queue,
                            VkCommandPool commandPool,
                            VkDevice device,
                            VkPhysicalDevice physicalDevice) {
        VK_SUCCESS_OR_THROW(Image::create(outImage,
                                          width, height,
                                          format,
                                          VK_IMAGE_TILING_OPTIMAL,
                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                          device, physicalDevice),
                            "Failed to create image.");
        
        transitionImageLayout(outImage->getImage(),
                              format,
                              VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              queue, commandPool, device);
    }

    // Managed variants of the above factories; resources are owned by the ResourceManager
    static VkResult create(Handle<Image>& outImage,
                           ResourceManager& resourceManager,
                           uint32_t width,
                           uint32_t height,
                           VkFormat format,
                           VkImageTiling tiling,
                           VkImageUsageFlags usage,
                           VkMemoryPropertyFlags properties,
                           VkDevice device,
                           VkPhysicalDevice physicalDevice,
                           VkImageAspectFlagBits aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        std::unique_ptr<Image> image;
        VkResult result = create(image, width, height, format, tiling, usage, properties, device, physicalDevice, aspect);
        outImage = resourceManager.adopt(std::move(image));
        return result;
    }

    static void createFromFile(Handle<Image>& outImage,
                               ResourceManager& resourceManager,
                               const std::string& filePath,
                               VkQueue graphicsQueue,
                               VkCommandPool commandPool,
                               VkDevice device,
                               VkPhysicalDevice physicalDevice) {
        std::unique_ptr<Image> image;
        createFromFile(image, filePath, graphicsQueue, commandPool, device, physicalDevice);
        outImage = resourceManager.adopt(std::move(image));
    }

    static void createFromUcharBuffer(Handle<Image>& outImage,
                                      ResourceManager& resourceManager,
                                      unsigned char* pixels,
                                      uint32_t width,
                                      uint32_t height,
                                      VkDeviceSize imageSize,
                                      VkQueue graphicsQueue,
                                      VkCommandPool commandPool,
                                      VkDevice device,
                                      VkPhysicalDevice physicalDevice) {
        std::unique_ptr<Image> image;
        createFromUcharBuffer(image, pixels, width, height, imageSize, graphicsQueue, commandPool, device, physicalDevice);
        outImage = resourceManager.adopt(std::move(image));
    }

    static void createEmptyRGBA(Handle<Image>& outImage,
                                ResourceManager& resourceManager,
                                uint32_t width,
                                uint32_t height,
                                VkQueue queue,
                                VkCommandPool commandPool,
                                VkDevice device,
                                VkPhysicalDevice physicalDevice) {
        createEmpty(outImage, resourceManager, VK_FORMAT_R8G8B8A8_SRGB, width, height, queue, commandPool, device, physicalDevice);
    }

    static void createEmpty(Handle<Image>& outImage,
                            ResourceManager& resourceManager,
                            VkFormat format,
                            uint32_t width,
                            uint32_t height,
                            VkQueue queue,
                            VkCommandPool commandPool,
                            VkDevice device,
                            VkPhysicalDevice physicalDevice) {
        std::unique_ptr<Image> image;
        createEmpty(image, format, width, height, queue, commandPool, device, physicalDevice);
        outImage = resourceManager.adopt(std::move(image));
    }

    void uploadUcharBufferToImage(unsigned char* pixels, 
                                  VkDeviceSize imageSize,
                                  VkQueue graphicsQueue,
                                  VkCommandPool commandPool,
                                  VkPhysicalDevice physicalDevice) {
        std::unique_ptr<Buffer<uint8_t>> stagingBuffer;
        VK_SUCCESS_OR_THROW(Buffer<uint8_t>::create(stagingBuffer, imageSize,
                                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                                    device_, physicalDevice),
                            "Failed to create image staging buffer");
        
        stagingBuffer->mapAndExecute(0, imageSize, [pixels, imageSize] (void* data) {
            memcpy(data, pixels, static_cast<size_t>(imageSize));
        });

        transitionImageLayout(getImage(),
                              VK_FORMAT_R8G8B8A8_SRGB,
                              VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              graphicsQueue, commandPool, device_);

        copyBufferToImage(stagingBuffer->getBuffer(),
                          getImage(),
                          width_, height_,
                          graphicsQueue, commandPool, device_);

        transitionImageLayout(getImage(),
                              VK_FORMAT_R8G8B8A8_SRGB,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              graphicsQueue, commandPool, device_);
    }
    
    // Access mask & pipeline stages to synchronize on for an image in a given layout
    struct LayoutSync {
        VkAccessFlags access;
        VkPipelineStageFlags stages;
    };
    
    static LayoutSync layoutSync(VkImageLayout layout) {
        constexpr VkPipelineStageFlags shaderStages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        switch (layout) {
            case VK_IMAGE_LAYOUT_UNDEFINED:
                // Nothing to wait on; the earliest possible stage
                return {0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT};
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                return {VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
            case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                return {VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                return {VK_ACCESS_SHADER_READ_BIT, shaderStages};
            case VK_IMAGE_LAYOUT_GENERAL:
                // Used for storage images, which shaders may read and write
                return {VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, shaderStages};
            default:
                // TODO better handling
                throw std::invalid_argument("unsupported layout transition!");
        }
    }
    
    static void transitionImageLayout(VkImage image,
                                      VkFormat format,
                                      VkImageLayout oldLayout,
                                      VkImageLayout newLayout,
                                      VkQueue graphicsQueue,
                                      VkCommandPool commandPool,
                                      VkDevice device,
                                      VkImageAspectFlagBits aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        // Resolve synchronization before recording, so unsupported layouts throw
        // without leaving a command buffer mid-recording
        const LayoutSync source = layoutSync(oldLayout);
        const LayoutSync destination = layoutSync(newLayout);
        if (newLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            throw std::invalid_argument("Cannot transition an image to VK_IMAGE_LAYOUT_UNDEFINED");
        }
        
        issueSingleTimeCommand([=](VkCommandBuffer commandBuffer){
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            
            // Must be set. Barrier can be used to transfer queue ownership.
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            
            barrier.image = image;
            barrier.subresourceRange.aspectMask = aspect;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;
            
            barrier.srcAccessMask = source.access;
            barrier.dstAccessMask = destination.access;
            
            vkCmdPipelineBarrier(
                commandBuffer,
                source.stages,
                destination.stages,
                0,
                0, nullptr,
                0, nullptr,
                1, &barrier
            );
        }, graphicsQueue, device, commandPool);
    }
    
    // Convenience wrapper around transitionImageLayout for this image
    void transitionLayout(VkImageLayout oldLayout,
                          VkImageLayout newLayout,
                          VkQueue queue,
                          VkCommandPool commandPool,
                          VkImageAspectFlagBits aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        transitionImageLayout(getImage(), format_, oldLayout, newLayout, queue, commandPool, device_, aspect);
    }
    
    // Copies the image's contents into host memory. The image must be in currentLayout
    // (any layout supported by transitionImageLayout) and is returned to it afterwards.
    std::vector<uint8_t> readback(VkImageLayout currentLayout,
                                  VkQueue queue,
                                  VkCommandPool commandPool,
                                  VkPhysicalDevice physicalDevice,
                                  VkImageAspectFlagBits aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        const VkDeviceSize size = bytesPerPixel(format_) * width_ * height_;
        
        std::unique_ptr<Buffer<uint8_t>> stagingBuffer;
        VK_SUCCESS_OR_THROW(Buffer<uint8_t>::create(stagingBuffer, size,
                                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                                    device_, physicalDevice),
                            "Failed to create image readback buffer");
        
        if (currentLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            transitionImageLayout(getImage(), format_, currentLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  queue, commandPool, device_, aspect);
        }
        
        copyImageToBuffer(getImage(), stagingBuffer->getBuffer(), width_, height_,
                          queue, commandPool, device_, aspect);
        
        if (currentLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            transitionImageLayout(getImage(), format_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, currentLayout,
                                  queue, commandPool, device_, aspect);
        }
        
        std::vector<uint8_t> pixels(size);
        stagingBuffer->mapAndExecute(0, size, [&pixels, size](void* data) {
            memcpy(pixels.data(), data, static_cast<size_t>(size));
        });
        return pixels;
    }
    
    static void copyImageToBuffer(VkImage image,
                                  VkBuffer buffer,
                                  uint32_t width,
                                  uint32_t height,
                                  VkQueue queue,
                                  VkCommandPool commandPool,
                                  VkDevice device,
                                  VkImageAspectFlagBits aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        issueSingleTimeCommand([=](VkCommandBuffer commandBuffer){
            VkBufferImageCopy region{};
            region.bufferOffset = 0;
            region.bufferRowLength = 0; // Tightly packed
            region.bufferImageHeight = 0;
            
            region.imageSubresource.aspectMask = aspect;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;
            
            region.imageOffset = {0, 0, 0};
            region.imageExtent = {width, height, 1};
            
            vkCmdCopyImageToBuffer(commandBuffer,
                                   image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   buffer,
                                   1,
                                   &region);
        }, queue, device, commandPool);
    }
    
    static void copyBufferToImage(VkBuffer buffer,
                                  VkImage image,
                                  uint32_t width,
                                  uint32_t height,
                                  VkQueue graphicsQueue,
                                  VkCommandPool commandPool,
                                  VkDevice device,
                                  VkImageAspectFlagBits aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        issueSingleTimeCommand([=](VkCommandBuffer commandBuffer){
            VkBufferImageCopy region{};
            region.bufferOffset = 0;
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;

            region.imageSubresource.aspectMask = aspect;
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
    
    VkFormat format_;
    uint32_t width_;
    uint32_t height_;
};
