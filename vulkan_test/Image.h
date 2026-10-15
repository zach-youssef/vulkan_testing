#pragma once

#include "VkTypes.h"
#include "VkUtil.h"
#include "Buffer.h"
#include "VulkanContext.h"

#include <stb_image.h>

#define RETURN_IF_ERROR(expr)   \
outResult = expr;               \
if (outResult != VK_SUCCESS) {  \
    return;                     \
}

class Image {
public:
    // Creates an image owned by the context's ResourceManager
    static VkResult create(Handle<Image>& outImage,
                           VulkanContext& context,
                           uint32_t width,
                           uint32_t height,
                           VkFormat format,
                           VkImageTiling tiling,
                           VkImageUsageFlags usage,
                           VkMemoryPropertyFlags properties,
                           VkImageAspectFlagBits aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        VkResult result;

        // Constructed directly since the constructor is private
        std::unique_ptr<Image> image(new Image(context, width, height,
                                               format, tiling,
                                               usage, properties,
                                               result, aspect));
        outImage = context.resources().adopt(std::move(image));

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

    // Loads an RGBA8 sRGB image from a file, leaving it in SHADER_READ_ONLY_OPTIMAL
    static void createFromFile(Handle<Image>& outImage,
                               VulkanContext& context,
                               const std::string& filePath) {
        int width, height, channels;

        stbi_uc* pixels = stbi_load((filePath).c_str(), &width, &height, &channels, STBI_rgb_alpha);

        if (!pixels) {
            throw std::runtime_error("Failed to load texture image.");
        }

        VkDeviceSize imageSize = width * height * 4;

        createFromUcharBuffer(outImage, context, pixels, width, height, imageSize);

        stbi_image_free(pixels);
    }

    // Creates an RGBA8 sRGB image from tightly packed pixels, leaving it in SHADER_READ_ONLY_OPTIMAL
    static void createFromUcharBuffer(Handle<Image>& outImage,
                                      VulkanContext& context,
                                      unsigned char* pixels,
                                      uint32_t width,
                                      uint32_t height,
                                      VkDeviceSize imageSize) {
        VK_SUCCESS_OR_THROW(Image::create(outImage,
                                          context,
                                          width, height,
                                          VK_FORMAT_R8G8B8A8_SRGB,
                                          VK_IMAGE_TILING_OPTIMAL,
                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
                            "Failed to create image.");

        outImage->uploadUcharBufferToImage(pixels, imageSize);
    }

    static void createEmptyRGBA(Handle<Image>& outImage,
                                VulkanContext& context,
                                uint32_t width,
                                uint32_t height) {
        createEmpty(outImage, context, VK_FORMAT_R8G8B8A8_SRGB, width, height);
    }

    // Creates an image for sampling / storage, leaving it in SHADER_READ_ONLY_OPTIMAL
    static void createEmpty(Handle<Image>& outImage,
                            VulkanContext& context,
                            VkFormat format,
                            uint32_t width,
                            uint32_t height) {
        VK_SUCCESS_OR_THROW(Image::create(outImage,
                                          context,
                                          width, height,
                                          format,
                                          VK_IMAGE_TILING_OPTIMAL,
                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
                            "Failed to create image.");

        outImage->transitionLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    void uploadUcharBufferToImage(unsigned char* pixels,
                                  VkDeviceSize imageSize) {
        auto stagingBuffer = Buffer<uint8_t>::createScoped(*context_, imageSize,
                                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        stagingBuffer->mapAndExecute(0, imageSize, [pixels, imageSize] (void* data) {
            memcpy(data, pixels, static_cast<size_t>(imageSize));
        });

        transitionLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        copyBufferToImage(*context_,
                          stagingBuffer->getBuffer(),
                          getImage(),
                          width_, height_);

        transitionLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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

    // Transitions image between layouts, blocking until the transition completes
    static void transitionImageLayout(VulkanContext& context,
                                      VkImage image,
                                      VkImageLayout oldLayout,
                                      VkImageLayout newLayout,
                                      VkImageAspectFlagBits aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        // Resolve synchronization before recording, so unsupported layouts throw
        // without leaving a command buffer mid-recording
        const LayoutSync source = layoutSync(oldLayout);
        const LayoutSync destination = layoutSync(newLayout);
        if (newLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            throw std::invalid_argument("Cannot transition an image to VK_IMAGE_LAYOUT_UNDEFINED");
        }

        context.submitImmediate([=](VkCommandBuffer commandBuffer){
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
        });
    }

    // Convenience wrapper around transitionImageLayout for this image
    void transitionLayout(VkImageLayout oldLayout,
                          VkImageLayout newLayout,
                          VkImageAspectFlagBits aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        transitionImageLayout(*context_, getImage(), oldLayout, newLayout, aspect);
    }

    // Copies the image's contents into host memory. The image must be in currentLayout
    // (any layout supported by transitionImageLayout) and is returned to it afterwards.
    std::vector<uint8_t> readback(VkImageLayout currentLayout,
                                  VkImageAspectFlagBits aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        const VkDeviceSize size = bytesPerPixel(format_) * width_ * height_;

        auto stagingBuffer = Buffer<uint8_t>::createScoped(*context_, size,
                                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (currentLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            transitionLayout(currentLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, aspect);
        }

        copyImageToBuffer(*context_, getImage(), stagingBuffer->getBuffer(), width_, height_, aspect);

        if (currentLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            transitionLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, currentLayout, aspect);
        }

        std::vector<uint8_t> pixels(size);
        stagingBuffer->mapAndExecute(0, size, [&pixels, size](void* data) {
            memcpy(pixels.data(), data, static_cast<size_t>(size));
        });
        return pixels;
    }

    // Copies an image in TRANSFER_SRC_OPTIMAL into a buffer, blocking until the copy completes
    static void copyImageToBuffer(VulkanContext& context,
                                  VkImage image,
                                  VkBuffer buffer,
                                  uint32_t width,
                                  uint32_t height,
                                  VkImageAspectFlagBits aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        context.submitImmediate([=](VkCommandBuffer commandBuffer){
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
        });
    }

    // Copies a buffer into an image in TRANSFER_DST_OPTIMAL, blocking until the copy completes
    static void copyBufferToImage(VulkanContext& context,
                                  VkBuffer buffer,
                                  VkImage image,
                                  uint32_t width,
                                  uint32_t height,
                                  VkImageAspectFlagBits aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        context.submitImmediate([=](VkCommandBuffer commandBuffer){
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
        });
    }

private:
    Image(VulkanContext& context,
          uint32_t width,
          uint32_t height,
          VkFormat format,
          VkImageTiling tiling,
          VkImageUsageFlags usage,
          VkMemoryPropertyFlags properties,
          VkResult& outResult,
          VkImageAspectFlagBits aspect) : context_(&context), format_(format), width_(width), height_(height) {
        VkDevice device = context.device();

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

        RETURN_IF_ERROR(VulkanImage::create(image_, device, imageInfo))

        VkMemoryRequirements memoryRequirements;
        vkGetImageMemoryRequirements(device, **image_, &memoryRequirements);

        RETURN_IF_ERROR(VulkanMemory::createFromRequirements(memory_,
                                                             device,
                                                             context.physicalDevice(),
                                                             properties,
                                                             memoryRequirements))

        vkBindImageMemory(device, **image_, **memory_, 0);

        RETURN_IF_ERROR(VulkanImageView::createForImageWithFormat(imageView_, device, **image_, format, aspect));
    }

    VulkanContext* context_;
    std::unique_ptr<VulkanImage> image_;
    std::unique_ptr<VulkanMemory> memory_;

    std::unique_ptr<VulkanImageView> imageView_;

    VkFormat format_;
    uint32_t width_;
    uint32_t height_;
};
