#pragma once

#include "core/VkTypes.h"
#include "core/VkUtil.h"
#include "resources/Buffer.h"
#include "core/VulkanContext.h"
#include "core/Contract.h"
#include "core/Result.h"

#include <stb_image.h>

#include <filesystem>

// How an image's 8-bit channels encode colour. Colour textures (photos, albedo and base colours)
// are usually sRGB, which sampling decodes to linear; data (normal maps, masks, lookup tables) is
// linear, read as stored.
enum class ColorSpace {
    Srgb,
    Linear,
};

class Image {
public:
    // Creates an image owned by the context's ResourceManager. Fails with ErrorCode::Unsupported if
    // the device does not support format for every requested usage.
    static Result<Handle<Image>> create(VulkanContext& context,
                                        uint32_t width,
                                        uint32_t height,
                                        VkFormat format,
                                        VkImageTiling tiling,
                                        VkImageUsageFlags usage,
                                        VkMemoryPropertyFlags properties,
                                        VkImageAspectFlagBits aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        if (!supportsUsage(context, format, tiling, usage)) {
            return Error(ErrorCode::Unsupported, std::string("Format ") + string_VkFormat(format)
                                                 + " does not support image usage " + string_VkImageUsageFlags(usage)
                                                 + " with " + string_VkImageTiling(tiling) + " on this device");
        }

        // Constructed directly since the constructor is private
        std::unique_ptr<Image> image(new Image(context, width, height, format, usage));
        TRY(image->init(tiling, properties, aspect));
        return context.resources().adopt(std::move(image));
    }

    // Whether the device supports format for every usage in usage. Storage support in particular
    // varies between devices (sRGB formats, for instance, are rarely usable as storage images).
    static bool supportsUsage(VulkanContext& context,
                              VkFormat format,
                              VkImageTiling tiling,
                              VkImageUsageFlags usage) {
        VkFormatProperties properties;
        vkGetPhysicalDeviceFormatProperties(context.physicalDevice(), format, &properties);
        const VkFormatFeatureFlags features = tiling == VK_IMAGE_TILING_LINEAR ? properties.linearTilingFeatures
                                                                               : properties.optimalTilingFeatures;
        return (features & requiredFormatFeatures(usage)) == requiredFormatFeatures(usage);
    }

    // Format features a format must support to be used for usage
    static VkFormatFeatureFlags requiredFormatFeatures(VkImageUsageFlags usage) {
        VkFormatFeatureFlags features = 0;
        if (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) features |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        if (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) features |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if (usage & VK_IMAGE_USAGE_STORAGE_BIT) features |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) features |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
        return features;
    }

    VkImage getImage() {
        return **image_;
    }

    // The image's view. Images used only for transfers (copies) have none, which Vulkan forbids
    // for their usage: asking for it is a contract violation.
    VkImageView getImageView() {
        if (!imageView_) {
            contractViolation("The image has no view: its usage is only transfers");
        }
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

    VkImageUsageFlags getUsage() {
        return usage_;
    }

    // Size in bytes of a single texel for the formats this wrapper supports reading back. Other
    // formats are a contract violation.
    static VkDeviceSize bytesPerPixel(VkFormat format) {
        switch (format) {
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_SRGB:
            case VK_FORMAT_D32_SFLOAT:
                return 4;
            case VK_FORMAT_R16G16B16A16_SFLOAT:
                return 8;
            case VK_FORMAT_R32G32B32A32_SFLOAT:
                return 16;
            default:
                contractViolation(std::string("bytesPerPixel doesn't support ") + string_VkFormat(format));
        }
    }

    // The RGBA8 format holding 8-bit colours in a colour space
    static VkFormat rgba8Format(ColorSpace colorSpace) {
        return colorSpace == ColorSpace::Srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    }

    // Loads an RGBA8 image for sampling from a file (any format stb_image reads: PNG, JPEG, ...),
    // leaving it in SHADER_READ_ONLY_OPTIMAL. Fails with ErrorCode::NotFound if the file doesn't
    // exist, InvalidData if it can't be decoded.
    static Result<Handle<Image>> createFromFile(VulkanContext& context,
                                                const std::filesystem::path& filePath,
                                                ColorSpace colorSpace) {
        const std::string name = filePath.string();
        if (!std::filesystem::exists(filePath)) {
            return Error(ErrorCode::NotFound, "Image '" + name + "' not found");
        }
        int width, height, channels;
        stbi_uc* pixels = stbi_load(name.c_str(), &width, &height, &channels, STBI_rgb_alpha);
        if (!pixels) {
            return Error(ErrorCode::InvalidData, "Failed to load image '" + name + "': " + stbi_failure_reason());
        }
        auto image = createFromUcharBuffer(context, pixels, width, height, VkDeviceSize(width) * height * 4, colorSpace);
        stbi_image_free(pixels);
        return std::move(image).withContext("loading image '" + name + "'");
    }

    // Like createFromFile, from an encoded file's bytes in memory (e.g. an image embedded in a
    // model). description names it in errors.
    static Result<Handle<Image>> createFromEncoded(VulkanContext& context,
                                                   const uint8_t* data,
                                                   size_t size,
                                                   ColorSpace colorSpace,
                                                   const std::string& description) {
        int width, height, channels;
        stbi_uc* pixels = stbi_load_from_memory(data, static_cast<int>(size), &width, &height, &channels, STBI_rgb_alpha);
        if (!pixels) {
            return Error(ErrorCode::InvalidData, "Failed to decode " + description + ": " + stbi_failure_reason());
        }
        auto image = createFromUcharBuffer(context, pixels, width, height, VkDeviceSize(width) * height * 4, colorSpace);
        stbi_image_free(pixels);
        return std::move(image).withContext("loading " + description);
    }

    // Creates an RGBA8 image for sampling from tightly packed pixels, leaving it in
    // SHADER_READ_ONLY_OPTIMAL
    static Result<Handle<Image>> createFromUcharBuffer(VulkanContext& context,
                                                       unsigned char* pixels,
                                                       uint32_t width,
                                                       uint32_t height,
                                                       VkDeviceSize imageSize,
                                                       ColorSpace colorSpace) {
        TRY_ASSIGN(auto image, Image::create(context,
                                             width, height,
                                             rgba8Format(colorSpace),
                                             VK_IMAGE_TILING_OPTIMAL,
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));

        TRY(image->uploadUcharBufferToImage(pixels, imageSize));
        return image;
    }

    // Creates an RGBA8 sRGB image for sampling, leaving it in SHADER_READ_ONLY_OPTIMAL
    static Result<Handle<Image>> createEmptyRGBA(VulkanContext& context,
                                                 uint32_t width,
                                                 uint32_t height) {
        return createEmpty(context, VK_FORMAT_R8G8B8A8_SRGB, width, height);
    }

    // Creates an image for sampling, leaving it in SHADER_READ_ONLY_OPTIMAL. It can also be read back.
    static Result<Handle<Image>> createEmpty(VulkanContext& context,
                                             VkFormat format,
                                             uint32_t width,
                                             uint32_t height) {
        TRY_ASSIGN(auto image, Image::create(context,
                                             width, height,
                                             format,
                                             VK_IMAGE_TILING_OPTIMAL,
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));

        TRY(image->transitionLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        return image;
    }

    // Creates an image that shaders can write as a storage image and also sample, leaving it in the
    // GENERAL layout (which both uses require). It can also be read back. Fails with
    // ErrorCode::Unsupported if format cannot be used as a storage image on this device, as is
    // typical for sRGB formats.
    static Result<Handle<Image>> createStorage(VulkanContext& context,
                                               VkFormat format,
                                               uint32_t width,
                                               uint32_t height) {
        TRY_ASSIGN(auto image, Image::create(context,
                                             width, height,
                                             format,
                                             VK_IMAGE_TILING_OPTIMAL,
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));

        TRY(image->transitionLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL));
        return image;
    }

    Result<void> uploadUcharBufferToImage(unsigned char* pixels,
                                          VkDeviceSize imageSize) {
        TRY_ASSIGN(auto stagingBuffer, Buffer<uint8_t>::createScoped(*context_, imageSize,
                                                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));

        TRY(stagingBuffer->mapAndExecute(0, imageSize, [pixels, imageSize] (void* data) {
            memcpy(data, pixels, static_cast<size_t>(imageSize));
        }));

        TRY(transitionLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));

        TRY(copyBufferToImage(*context_,
                              stagingBuffer->getBuffer(),
                              getImage(),
                              width_, height_));

        return transitionLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
                contractViolation(std::string("Image layout transitions don't support ") + string_VkImageLayout(layout));
        }
    }

    // Transitions image between layouts, blocking until the transition completes. Layouts
    // layoutSync doesn't support are a contract violation.
    static Result<void> transitionImageLayout(VulkanContext& context,
                                              VkImage image,
                                              VkImageLayout oldLayout,
                                              VkImageLayout newLayout,
                                              VkImageAspectFlagBits aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        // Resolve synchronization before recording, so unsupported layouts are caught without
        // leaving a command buffer mid-recording
        const LayoutSync source = layoutSync(oldLayout);
        const LayoutSync destination = layoutSync(newLayout);
        if (newLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            contractViolation("Cannot transition an image to VK_IMAGE_LAYOUT_UNDEFINED");
        }

        return context.submitImmediate([=](VkCommandBuffer commandBuffer){
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
    Result<void> transitionLayout(VkImageLayout oldLayout,
                                  VkImageLayout newLayout,
                                  VkImageAspectFlagBits aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        return transitionImageLayout(*context_, getImage(), oldLayout, newLayout, aspect);
    }

    // Copies the image's contents into host memory. The image must be in currentLayout
    // (any layout supported by transitionImageLayout) and is returned to it afterwards.
    Result<std::vector<uint8_t>> readback(VkImageLayout currentLayout,
                                          VkImageAspectFlagBits aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        const VkDeviceSize size = bytesPerPixel(format_) * width_ * height_;

        TRY_ASSIGN(auto stagingBuffer, Buffer<uint8_t>::createScoped(*context_, size,
                                                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));

        if (currentLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            TRY(transitionLayout(currentLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, aspect));
        }

        TRY(copyImageToBuffer(*context_, getImage(), stagingBuffer->getBuffer(), width_, height_, aspect));

        if (currentLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            TRY(transitionLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, currentLayout, aspect));
        }

        std::vector<uint8_t> pixels(size);
        TRY(stagingBuffer->mapAndExecute(0, size, [&pixels, size](void* data) {
            memcpy(pixels.data(), data, static_cast<size_t>(size));
        }));
        return pixels;
    }

    // Copies an image in TRANSFER_SRC_OPTIMAL into a buffer, blocking until the copy completes
    static Result<void> copyImageToBuffer(VulkanContext& context,
                                          VkImage image,
                                          VkBuffer buffer,
                                          uint32_t width,
                                          uint32_t height,
                                          VkImageAspectFlagBits aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        return context.submitImmediate([=](VkCommandBuffer commandBuffer){
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
    static Result<void> copyBufferToImage(VulkanContext& context,
                                          VkBuffer buffer,
                                          VkImage image,
                                          uint32_t width,
                                          uint32_t height,
                                          VkImageAspectFlagBits aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        return context.submitImmediate([=](VkCommandBuffer commandBuffer){
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
          VkImageUsageFlags usage) : context_(&context), format_(format), usage_(usage), width_(width), height_(height) {}

    Result<void> init(VkImageTiling tiling, VkMemoryPropertyFlags properties, VkImageAspectFlagBits aspect) {
        VkDevice device = context_->device();

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = VkExtent3D{width_, height_, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;

        imageInfo.format = format_;

        imageInfo.tiling = tiling;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; // first transition discards

        imageInfo.usage = usage_;
        // Usable from any of the context's queues without ownership transfers
        const auto& families = context_->queueFamilies();
        imageInfo.sharingMode = families.size() > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.queueFamilyIndexCount = families.size() > 1 ? static_cast<uint32_t>(families.size()) : 0;
        imageInfo.pQueueFamilyIndices = families.size() > 1 ? families.data() : nullptr;

        // Multisampling
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

        VK_TRY(VulkanImage::create(image_, device, imageInfo), "Failed to create image");

        VkMemoryRequirements memoryRequirements;
        vkGetImageMemoryRequirements(device, **image_, &memoryRequirements);

        VK_TRY(VulkanMemory::createFromRequirements(memory_,
                                                    device,
                                                    context_->physicalDevice(),
                                                    properties,
                                                    memoryRequirements),
               "Failed to allocate image memory");

        VK_TRY(vkBindImageMemory(device, **image_, **memory_, 0), "Failed to bind image memory");

        constexpr VkImageUsageFlags viewUsages = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
                                                 | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                                 | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                 | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT
                                                 | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
        if (usage_ & viewUsages) {
            VK_TRY(VulkanImageView::createForImageWithFormat(imageView_, device, **image_, format_, aspect),
                   "Failed to create image view");
        }
        return {};
    }

    VulkanContext* context_;
    std::unique_ptr<VulkanImage> image_;
    std::unique_ptr<VulkanMemory> memory_;

    std::unique_ptr<VulkanImageView> imageView_;

    VkFormat format_;
    VkImageUsageFlags usage_;
    uint32_t width_;
    uint32_t height_;
};
