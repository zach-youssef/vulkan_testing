#pragma once

#include "core/Contract.h"
#include "core/Result.h"
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

 Acquire semaphores are per frame in flight: a frame slot's semaphore is reused only once the
 slot's previous frame, which waited on it, has finished (see VulkanContext::beginFrame).

 Not thread safe. recreate() requires that the device is idle.
 */
class Swapchain {
public:
    // framebufferSize is the surface's size in pixels, used when the surface leaves the extent up to
    // the application. The context must have a surface.
    static Result<std::unique_ptr<Swapchain>> create(VulkanContext& context, VkExtent2D framebufferSize) {
        if (!context.hasSurface()) {
            contractViolation("A swapchain requires a context created with a surface");
        }
        std::unique_ptr<Swapchain> swapchain(new Swapchain(context));
        TRY(swapchain->build(framebufferSize).withContext("creating the swapchain"));
        TRY_ASSIGN(swapchain->acquireSemaphores_, PerFrame<std::unique_ptr<VulkanSemaphore>>::create(
            context, [device = context.device()](uint32_t) -> Result<std::unique_ptr<VulkanSemaphore>> {
                VkSemaphoreCreateInfo semaphoreInfo{};
                semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                std::unique_ptr<VulkanSemaphore> semaphore;
                VK_TRY(VulkanSemaphore::create(semaphore, device, semaphoreInfo), "Failed to create semaphore");
                return semaphore;
            }));
        return swapchain;
    }

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // Replaces the swapchain (and its images, views and semaphores) to match the surface's
    // current size. The device must be idle. On failure the swapchain is left as it was, though
    // it may no longer be presentable.
    Result<void> recreate(VkExtent2D framebufferSize) {
        return build(framebufferSize).withContext("recreating the swapchain");
    }

    struct Acquired {
        VkResult result;            // VK_SUCCESS, VK_SUBOPTIMAL_KHR or VK_ERROR_OUT_OF_DATE_KHR
        uint32_t imageIndex = 0;
        VkSemaphore semaphore = VK_NULL_HANDLE;   // Signaled once the image can be written
    };

    // Acquires the next image for a frame slot. With VK_ERROR_OUT_OF_DATE_KHR no image was
    // acquired and the swapchain must be recreated; other failures are errors.
    Result<Acquired> acquire(uint32_t frameIndex) {
        Acquired acquired;
        acquired.semaphore = **acquireSemaphores_[frameIndex];
        acquired.result = vkAcquireNextImageKHR(context_->device(), **swapchain_, UINT64_MAX,
                                                acquired.semaphore, VK_NULL_HANDLE, &acquired.imageIndex);
        if (acquired.result != VK_SUCCESS && acquired.result != VK_SUBOPTIMAL_KHR
            && acquired.result != VK_ERROR_OUT_OF_DATE_KHR) {
            return Error::vulkan(acquired.result, "Failed to acquire swapchain image");
        }
        return acquired;
    }

    VkSwapchainKHR get() {
        return **swapchain_;
    }

    VkFormat getFormat() {
        return format_;
    }

    VkImageUsageFlags getUsage() {
        return usage_;
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
    explicit Swapchain(VulkanContext& context) : context_(&context) {}

    // Creates the swapchain and its images' views and semaphores, replacing the current ones only
    // once all of them have been created
    Result<void> build(VkExtent2D framebufferSize) {
        TRY_ASSIGN(SwapChainSupportDetails support, querySwapChainSupport(context_->physicalDevice(), context_->surface()));

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
        // Rendered to, and copied from when supported (e.g. for screenshots)
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                | (support.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        usage_ = createInfo.imageUsage;

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

        std::unique_ptr<VulkanSwapchain> swapchain;
        VK_TRY(VulkanSwapchain::create(swapchain, context_->device(), createInfo), "Failed to create swap chain");

        TRY_ASSIGN(auto images, (readVkVector<VkImage, VkDevice, VkSwapchainKHR>(context_->device(), **swapchain,
                                                                                 vkGetSwapchainImagesKHR)));

        std::vector<std::unique_ptr<VulkanImageView>> imageViews(images.size());
        for (size_t i = 0; i < images.size(); ++i) {
            VK_TRY(VulkanImageView::createForImageWithFormat(imageViews[i], context_->device(), images[i], surfaceFormat.format),
                   "Failed to create image view for swapchain");
        }

        std::vector<std::unique_ptr<VulkanSemaphore>> renderFinishedSemaphores(images.size());
        for (auto& semaphore : renderFinishedSemaphores) {
            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            VK_TRY(VulkanSemaphore::create(semaphore, context_->device(), semaphoreInfo), "Failed to create semaphore");
        }

        // The views of the old images are destroyed before the old swapchain (which owns the images)
        imageViews_ = std::move(imageViews);
        renderFinishedSemaphores_ = std::move(renderFinishedSemaphores);
        swapchain_ = std::move(swapchain);
        images_ = std::move(images);
        format_ = surfaceFormat.format;
        extent_ = extent;
        return {};
    }

    VulkanContext* context_;
    std::unique_ptr<VulkanSwapchain> swapchain_;
    std::vector<VkImage> images_;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage_ = 0;
    VkExtent2D extent_{};
    std::vector<std::unique_ptr<VulkanImageView>> imageViews_;
    std::vector<std::unique_ptr<VulkanSemaphore>> renderFinishedSemaphores_;
    PerFrame<std::unique_ptr<VulkanSemaphore>> acquireSemaphores_;
};
