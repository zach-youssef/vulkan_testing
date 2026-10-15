#pragma once

#include "core/VkTypes.h"
#include "core/VkUtil.h"
#include "resources/Image.h"
#include "core/ResourceManager.h"
#include "core/VulkanContext.h"

/*
 A render target that is not backed by a swapchain: a color image and a depth image
 bound to a framebuffer, with a compatible render pass.

 After the render pass completes, the color image is left in TRANSFER_SRC_OPTIMAL so it
 can be read back with readback().
 */
class OffscreenRenderTarget {
public:
    static constexpr VkImageLayout kColorFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    OffscreenRenderTarget(VulkanContext& context,
                          uint32_t width,
                          uint32_t height,
                          VkFormat colorFormat)
    : extent_{width, height} {
        VkDevice device = context.device();
        
        VK_SUCCESS_OR_THROW(Image::create(color_, context,
                                          width, height,
                                          colorFormat,
                                          VK_IMAGE_TILING_OPTIMAL,
                                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
                            "Failed to create offscreen color image");

        VK_SUCCESS_OR_THROW(Image::create(depth_, context,
                                          width, height,
                                          VK_FORMAT_D32_SFLOAT,
                                          VK_IMAGE_TILING_OPTIMAL,
                                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                          VK_IMAGE_ASPECT_DEPTH_BIT),
                            "Failed to create offscreen depth image");

        // Make color writes available to transfers (readback) once the render pass ends
        VkSubpassDependency toTransfer{};
        toTransfer.srcSubpass = 0;
        toTransfer.dstSubpass = VK_SUBPASS_EXTERNAL;
        toTransfer.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toTransfer.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        VK_SUCCESS_OR_THROW(VulkanRenderPass::createColorDepth(renderPass_, device, colorFormat,
                                                               kColorFinalLayout, {toTransfer}),
                            "Failed to create offscreen render pass");

        VkImageView attachments[] = {
            color_->getImageView(),
            depth_->getImageView()
        };

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = **renderPass_;
        framebufferInfo.attachmentCount = 2;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = width;
        framebufferInfo.height = height;
        framebufferInfo.layers = 1;

        VK_SUCCESS_OR_THROW(VulkanFramebuffer::create(framebuffer_, device, framebufferInfo),
                            "Failed to create offscreen framebuffer");
    }

    VkExtent2D getExtent() {
        return extent_;
    }

    VkRenderPass getRenderPass() {
        return **renderPass_;
    }

    VkFramebuffer getFramebuffer() {
        return **framebuffer_;
    }

    Handle<Image> getColorImage() {
        return color_;
    }

    // Reads back the color image. Must only be called once rendering into it has completed.
    std::vector<uint8_t> readback() {
        return color_->readback(kColorFinalLayout);
    }

private:
    VkExtent2D extent_;

    Handle<Image> color_;
    Handle<Image> depth_;
    std::unique_ptr<VulkanRenderPass> renderPass_;
    std::unique_ptr<VulkanFramebuffer> framebuffer_;
};
