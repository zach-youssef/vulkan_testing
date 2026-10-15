#pragma once

#include "core/VkTypes.h"
#include "core/VkUtil.h"
#include "core/SwapChainSupportDetails.h"
#include "core/VulkanContext.h"

#include <memory>
#include <vector>

/*
 The swapchain presenting to a context's surface, along with its images, their views and a render
 finished semaphore per image.

 The render finished semaphores are signaled by the work that renders to an image and waited on by
 its present. They are per image rather than per frame in flight, since the presentation engine
 may still be using an image's semaphore until that image is acquired again.

 Not thread safe. recreate() requires that the device is idle.
 */
class Swapchain {
public:
    // framebufferSize is the surface's size in pixels, used when the surface leaves the extent up to
    // the application
    Swapchain(VulkanContext& context, VkExtent2D framebufferSize) : context_(&context) {
        if (!context.hasSurface()) {
            throw std::logic_error("A swapchain requires a context created with a surface");
        }
        create(framebufferSize);
    }

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // Replaces the swapchain (and its images, views and semaphores) to match the surface's
    // current size. The device must be idle.
    void recreate(VkExtent2D framebufferSize) {
        create(framebufferSize);
    }

    VkSwapchainKHR get() {
        return **swapchain_;
    }

    VkFormat getFormat() {
        return format_;
    }

    VkExtent2D getExtent() {
        return extent_;
    }

    uint32_t getImageCount() {
        return static_cast<uint32_t>(images_.size());
    }

    VkImage getImage(uint32_t imageIndex) {
        return images_.at(imageIndex);
    }

    VkImageView getImageView(uint32_t imageIndex) {
        return **imageViews_.at(imageIndex);
    }

    VkSemaphore getRenderFinishedSemaphore(uint32_t imageIndex) {
        return **renderFinishedSemaphores_.at(imageIndex);
    }

    std::vector<VkSemaphore> getRenderFinishedSemaphores() {
        std::vector<VkSemaphore> semaphores;
        for (auto& semaphore : renderFinishedSemaphores_) {
            semaphores.push_back(**semaphore);
        }
        return semaphores;
    }

private:
    void create(VkExtent2D framebufferSize) {
        SwapChainSupportDetails support = querySwapChainSupport(context_->physicalDevice(), context_->surface());

        auto surfaceFormat = support.chooseSwapSurfaceFormat();
        auto presentMode = support.chooseSwapPresentMode();
        auto extent = support.chooseSwapExtent(framebufferSize);

        uint32_t imageCount = support.capabilities.minImageCount + 1;
        // A maximum of 0 means there is no maximum
        if (support.capabilities.maxImageCount > 0) {
            imageCount = std::min(imageCount, support.capabilities.maxImageCount);
        }

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = context_->surface();
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1; // Always 1 unless steroscopic 3D
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        // Set sharing mode based on if a single present/graphics queue will be accessing the swapchain or 2 separate queues
        uint32_t queueFamilyIndices[] = {context_->queueFamily(QueueType::Graphics), context_->queueFamily(QueueType::Present)};
        if (queueFamilyIndices[0] != queueFamilyIndices[1]) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        } else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        // Don't apply any rotation, flips, etc
        createInfo.preTransform = support.capabilities.currentTransform;
        // Make the swapchain image opaque
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

        createInfo.presentMode = presentMode;
        createInfo.clipped = true;

        // Lets the implementation reuse resources of the swapchain being replaced, which is
        // destroyed once the new one exists
        createInfo.oldSwapchain = swapchain_ ? **swapchain_ : VK_NULL_HANDLE;

        VK_SUCCESS_OR_THROW(VulkanSwapchain::create(swapchain_, context_->device(), createInfo),
                            "Failed to create swap chain.");

        images_ = readVkVector<VkImage, VkDevice, VkSwapchainKHR>(context_->device(), **swapchain_,
                                                                  vkGetSwapchainImagesKHR);
        format_ = surfaceFormat.format;
        extent_ = extent;

        imageViews_.clear();
        imageViews_.resize(images_.size());
        for (size_t i = 0; i < images_.size(); ++i) {
            VK_SUCCESS_OR_THROW(VulkanImageView::createForImageWithFormat(imageViews_[i],
                                                                          context_->device(),
                                                                          images_[i],
                                                                          format_),
                                "Failed to create image view for swapchain");
        }

        renderFinishedSemaphores_.clear();
        renderFinishedSemaphores_.resize(images_.size());
        for (auto& semaphore : renderFinishedSemaphores_) {
            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            VK_SUCCESS_OR_THROW(VulkanSemaphore::create(semaphore, context_->device(), semaphoreInfo),
                                "Failed to create semaphore");
        }
    }

    VulkanContext* context_;
    std::unique_ptr<VulkanSwapchain> swapchain_;
    std::vector<VkImage> images_;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<std::unique_ptr<VulkanImageView>> imageViews_;
    std::vector<std::unique_ptr<VulkanSemaphore>> renderFinishedSemaphores_;
};
