#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "VkTypes.h"
#include "VkUtil.h"
#include "DeviceSelection.h"
#include "QueueFamilyIndices.h"
#include "SwapChainSupportDetails.h"
#include "VkTypes.h"
#include "Buffer.h"
#include "Image.h"
#include "FileUtil.h"
#include "Renderable.h"

#include <glm/glm.hpp>

#include <iostream>

const std::vector<const char*> validationLayers = {
    "VK_LAYER_KHRONOS_validation"
};

#ifdef NDEBUG
    const bool enableValidationLayers = false;
#else
    [[maybe_unused]]
    const bool enableValidationLayers = true;
#endif

class VulkanApp {
public:
    VulkanApp(const uint32_t windowHeight,
              const uint32_t windowWidth,
              const uint32_t maxFramesInFlight)
    : windowHeight_(windowHeight),
    windowWidth_(windowWidth)
    //,maxFramesInFlight_(maxFramesInFlight)
    {}
    
    void init() {
        initWindow();
        initVulkan();
    }
    
    void run() {
        mainLoop();
    }
private: // Main initialize & run functions
    void initWindow() {
        glfwInit();
        
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        
        window_ = std::unique_ptr<GLFWwindow, std::function<void(GLFWwindow*)>>(glfwCreateWindow(windowWidth_, windowHeight_, "Vulkan", nullptr, nullptr), [](GLFWwindow* w){
            glfwDestroyWindow(w);
            glfwTerminate();
        });
        glfwSetWindowUserPointer(window_.get(), this);
        glfwSetFramebufferSizeCallback(window_.get(), onFrameBufferResizeCallback);
    }
    
    static void onFrameBufferResizeCallback(GLFWwindow* window, int /*width*/, int /*height*/) {
        auto app = reinterpret_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
        app->frameBufferResized_ = true;
    }

    void initVulkan() {
        createInstance();
        createSurface();
        choosePhysicalDevice();
        createLogicalDevice();
        createSwapChain();
        createSwapChainImageViews();
        createRenderPass();
        createFramebuffers();
        createCommandPool();
        createSyncObjects();
        createCommandBuffers();
    }
    
    void mainLoop() {
        while(!glfwWindowShouldClose(window_.get())) {
            glfwPollEvents();
            drawFrame();
        }
        vkDeviceWaitIdle(**device_);
    }
    
    void drawFrame() {
        // Wait for previous frame to complete
        vkWaitForFences(**device_, 1, (*inFlightFences_[currentFrameIndex_]).get(), VK_TRUE, UINT64_MAX);
        
        // Acquire index of next image in swapchain
        uint32_t imageIndex;
        auto result = vkAcquireNextImageKHR(**device_, **swapChain_, UINT64_MAX, **imageAvailableSemaphores_[currentFrameIndex_], VK_NULL_HANDLE, &imageIndex);
        bool shouldRecreateSwapChain = result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR;

        if (shouldRecreateSwapChain || frameBufferResized_) {
            frameBufferResized_ = false;
            recreateSwapChain();
            return;
        }

        // Perform any pre-draw actions
        for (auto& callback : preDrawCallbacks_) {
            (*callback)(*this, currentFrameIndex_);
        }
        
        // Only reset fence here now that we know we will be doing work
        vkResetFences(**device_, 1, (*inFlightFences_[currentFrameIndex_]).get());
        
        // If we have compute passes, dispatch those first
        if (!computePasses_.empty()) {
            // Make sure our sync objects are initialized
            if (!computeSyncInitialized_) {
                createComputeSyncObjects();
            }
            
            // Grab Commmand Buffer
            auto commandBuffer = computeCommandBuffers_[currentFrameIndex_];
            
            
            for (int passIndex = 0; passIndex < computePasses_.size(); ++passIndex) {
                // Grab & update compute material
                auto& computePass = computePasses_.at(passIndex);
                computePass->update(currentFrameIndex_, swapChainExtent_);
                
                // Start command buffer
                VK_SUCCESS_OR_THROW(vkResetCommandBuffer(commandBuffer, 0), "Failed to reset compute cb");
                VkCommandBufferBeginInfo beginInfo{};
                beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                VK_SUCCESS_OR_THROW(vkBeginCommandBuffer(commandBuffer, &beginInfo),
                                    "Failed to begin compute commmand buffer");

                // Bind pipeline & descriptor sets
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, computePass->getPipeline());
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        computePass->getPipelineLayout(),
                                        0, 1,
                                        computePass->getDescriptorSet(currentFrameIndex_),
                                        0, 0);
                // Dispatch workgroups
                auto dispatchSize = computePass->getDispatchDimensions();
                vkCmdDispatch(commandBuffer, dispatchSize.x, dispatchSize.y, dispatchSize.z);
                
                // End command buffer
                vkEndCommandBuffer(commandBuffer);
                
                // Submit Work
                VkSubmitInfo submitInfo{};
                submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submitInfo.commandBufferCount = 1;
                submitInfo.pCommandBuffers = &commandBuffer;
                VkSemaphore wait[1];
                if (passIndex > 0) {
                    wait[0] = **computeSemaphores_[currentFrameIndex_][passIndex - 1];
                    submitInfo.waitSemaphoreCount = 1;
                    submitInfo.pWaitSemaphores = wait;
                }
                VkSemaphore signal[] = {**computeSemaphores_[currentFrameIndex_][passIndex]};
                submitInfo.signalSemaphoreCount = 1;
                submitInfo.pSignalSemaphores = signal;
                VK_SUCCESS_OR_THROW(vkQueueSubmit(computeQueue_, 1, &submitInfo, VK_NULL_HANDLE),
                                    "Failed to submit compute");
            }
        }
        
        // Grab command buffer
        auto commandBuffer = commandBuffers_[currentFrameIndex_];
        
        // Reset it
        VK_SUCCESS_OR_THROW(vkResetCommandBuffer(commandBuffer, 0), "Failed to reset cb")

        // Start recording
        //startRenderPass(commandBuffer, imageIndex);
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = 0; // Optional
        beginInfo.pInheritanceInfo = nullptr; // Optional
        
        VK_SUCCESS_OR_THROW(vkBeginCommandBuffer(commandBuffer, &beginInfo),
                            "Failed to begin recording command buffer");
        
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = **renderPass_;
        renderPassInfo.framebuffer = **swapChainFramebuffers_[imageIndex];
        
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = swapChainExtent_;
        
        VkClearValue clearColor = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &clearColor;

        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        // TODO: 99% sure this loop does not work with more than one renderable
        for (auto& renderable : renderables_) {
            // Update the uniform buffer
            renderable->update(currentFrameIndex_, swapChainExtent_);
            
            // Moving out of the renderable to here seems to get us farther
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              renderable->getMaterial()->getPipeline());
            
            VkDeviceSize offsets[] {0};
            VkBuffer vertexBuffers[] {renderable->getVertexBuffer()};
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
            vkCmdBindIndexBuffer(commandBuffer, renderable->getIndexBuffer(), 0, VK_INDEX_TYPE_UINT16);
            
            vkCmdBindDescriptorSets(commandBuffer,
                                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    renderable->getMaterial()->getPipelineLayout(),
                                    0, 1,
                                    renderable->getMaterial()->getDescriptorSet(currentFrameIndex_),
                                    0, nullptr);
            
            VkViewport viewport{};
            viewport.x = 0.0f;
            viewport.y = 0.0f;
            viewport.width = static_cast<float>(swapChainExtent_.width);
            viewport.height = static_cast<float>(swapChainExtent_.height);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

            VkRect2D scissor{};
            scissor.offset = {0, 0};
            scissor.extent = swapChainExtent_;
            vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
            
            vkCmdDrawIndexed(commandBuffer,
                             renderable->getIndexCount(),
                             1 /*num instances*/,
                             0 /*offset into buffer*/,
                             0 /*offset to add to indices*/,
                             0 /* instancing offset*/);
        }
        

        // End render pass
        vkCmdEndRenderPass(commandBuffer);
        
        // End command buffer
        vkEndCommandBuffer(commandBuffer);
        
        // Submit to graphics queue
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        // (it should wait for the swapchain image to be available before writing out to it)
        VkPipelineStageFlags waitStages[] {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        VkSemaphore wait[2];
        wait[0] = **imageAvailableSemaphores_[currentFrameIndex_];
        // make sure any compute work was finished
        if (!computePasses_.empty()) {
            wait[1] = **computeSemaphores_[currentFrameIndex_].at(computePasses_.size() - 1);
        }
        VkSemaphore signal[] = {**renderFinishedSemaphores_[currentFrameIndex_]};
        submitInfo.waitSemaphoreCount = computePasses_.empty() ? 1 : 2;
        submitInfo.pWaitSemaphores = wait;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        submitInfo.signalSemaphoreCount = 1;//static_cast<uint32_t>(signalSemaphores.size());
        submitInfo.pSignalSemaphores = signal;

        VK_SUCCESS_OR_THROW(vkQueueSubmit(graphicsQueue_, 1, &submitInfo, **inFlightFences_[currentFrameIndex_]),
                            "Failed to submit draw command buffer.");

        // Submit present queue
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = (*renderFinishedSemaphores_[currentFrameIndex_]).get();
        VkSwapchainKHR swapChains[] = {**swapChain_};
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapChains;
        presentInfo.pImageIndices = &imageIndex;
        presentInfo.pResults = nullptr; // Array of VkResults to check for each swapchain?
        
        vkQueuePresentKHR(presentQueue_, &presentInfo);

        // Increment frame index
        currentFrameIndex_ = (this->currentFrameIndex_ + 1) % maxFramesInFlight_;
    }
    
private: // Vulkan Initialization Functions
    void createInstance() {
        logSupportedExtensions();
        logAvailableLayers();
#ifndef __APPLE__
        //TODO: Figure out why validation layers aren't available
        // MoltenVK logs in XCode seem to give us plenty of info as it is? ¯\_(ツ)_/¯
        if (enableValidationLayers && !checkValidationLayerSupport()) {
            throw std::runtime_error("Validation layers requested but not available!");
        }
#endif
        
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "VulkanApp";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "No Engine";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_0;
        
        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        
        uint32_t glfwExtensionCount = 0;
        const char** glfwExtensions;

        glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

        std::vector<const char*> requiredExtensions;

        for(uint32_t i = 0; i < glfwExtensionCount; i++) {
            requiredExtensions.emplace_back(glfwExtensions[i]);
        }

        requiredExtensions.emplace_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);

        createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;

        createInfo.enabledExtensionCount = (uint32_t) requiredExtensions.size();
        createInfo.ppEnabledExtensionNames = requiredExtensions.data();
        
        createInfo.enabledLayerCount = 0;
        
        VK_SUCCESS_OR_THROW(VulkanInstance::create(instance_, createInfo),
                            "Failed to create Instance!");
    }
    
    void createSurface() {
        VK_SUCCESS_OR_THROW(VulkanSurface::create(surface_, **instance_, window_.get()),
                            "Failed to create window surface");
    }
    
    void choosePhysicalDevice() {
        physicalDevice_ = pickPhysicalDevice(**instance_, **surface_);
        
        if (physicalDevice_ == VK_NULL_HANDLE) {
            throw std::runtime_error("Failed to find a suitable GPU.");
        }
    }
    
    void createLogicalDevice() {
        QueueFamilyIndices indices = findQueueFamilies(physicalDevice_, **surface_);
        
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        
        float queuePriority = 1.0f; // Required even if only 1 queue
        for (uint32_t queueFamily : indices.uniqueQueueFamilies()) {
            VkDeviceQueueCreateInfo queueCreateInfo{};
            queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCreateInfo.queueFamilyIndex = queueFamily;
            queueCreateInfo.queueCount = 1;
            queueCreateInfo.pQueuePriorities = &queuePriority;
            
            queueCreateInfos.push_back(queueCreateInfo);
        }
        
        VkPhysicalDeviceFeatures deviceFeatures{}; // empty for now;
        
        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.pEnabledFeatures = &deviceFeatures;
        
        createInfo.enabledLayerCount = 0;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
        createInfo.ppEnabledExtensionNames = deviceExtensions.data();
        
        VK_SUCCESS_OR_THROW(VulkanDevice::create(device_, physicalDevice_, createInfo),
                            "Failed to create logical device.");
        
        vkGetDeviceQueue(**device_, indices.graphicsFamily.value(), 0, &graphicsQueue_);
        vkGetDeviceQueue(**device_, indices.presentFamily.value(), 0, &presentQueue_);
        // Compute queue technically same as graphics in current implementation
        // Creating dedicated handle in case I ever actually implement the async queue
        vkGetDeviceQueue(**device_, indices.graphicsFamily.value(), 0, &computeQueue_);
    }
    
    void createSwapChain() {
        SwapChainSupportDetails swapChainSupport = querySwapChainSupport(physicalDevice_, **surface_);
        
        auto surfaceFormat = swapChainSupport.chooseSwapSurfaceFormat();
        auto presentMode = swapChainSupport.chooseSwapPresentMode();
        auto extent = swapChainSupport.chooseSwapExtent(window_.get());
        
        uint32_t imageCount = std::min(swapChainSupport.capabilities.minImageCount + 1,
                                       swapChainSupport.capabilities.maxImageCount);
        
        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = **surface_;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1; // Always 1 unless steroscopic 3D
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        
        // Set sharing mode based on if a single present/graphics queue will be accessing the swapchain or 2 separate queues
        QueueFamilyIndices indices = findQueueFamilies(physicalDevice_, **surface_);
        uint32_t queueFamilyIndices[] = {indices.graphicsFamily.value(), indices.presentFamily.value()};
        if (indices.graphicsFamily != indices.presentFamily) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        } else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            createInfo.queueFamilyIndexCount = 0; // Optional
            createInfo.pQueueFamilyIndices = nullptr; // Optional
        }
        
        // Don't apply any rotation, flips, etc
        createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
        // Make the swapchain image opaque
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        
        createInfo.presentMode = presentMode;
        createInfo.clipped = true;
        
        if (swapChain_) {
            createInfo.oldSwapchain = **swapChain_;
        } else {
            createInfo.oldSwapchain = VK_NULL_HANDLE;
        }
        
        VK_SUCCESS_OR_THROW(VulkanSwapchain::create(swapChain_, **device_, createInfo),
                            "Failed to create swap chain.");
        
        // Retrieve swapchain images
        swapChainImages_ = readVkVector<VkImage, VkDevice, VkSwapchainKHR>(**device_, **swapChain_,
                                                                                vkGetSwapchainImagesKHR);
        
        // Cache swapchain properties
        swapChainImageFormat_ = surfaceFormat.format;
        swapChainExtent_ = extent;
    }
    
    void createSwapChainImageViews() {
        swapChainImageViews_.resize(swapChainImages_.size());
        
        for (size_t i = 0; i < swapChainImages_.size(); ++i) {
            VK_SUCCESS_OR_THROW(VulkanImageView::createForImageWithFormat(swapChainImageViews_[i],
                                                                          **device_,
                                                                          swapChainImages_[i],
                                                                          swapChainImageFormat_),
                                "Failed to create image view for swapchain");
        }
    }
    
    void createRenderPass() {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = swapChainImageFormat_;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; // clear data before loading
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE; // Store rendered content in memory after rendering
        
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    
        VkAttachmentReference colorAttachmentRef{};
        colorAttachmentRef.attachment = 0;
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        
        subpass.colorAttachmentCount = 1; // matches inputs to shader
        subpass.pColorAttachments = &colorAttachmentRef;
        
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        
        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &colorAttachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;
        
        VK_SUCCESS_OR_THROW(VulkanRenderPass::create(renderPass_, **device_, renderPassInfo),
                            "Failed to create render pass");
    }
    
    void createFramebuffers() {
        swapChainFramebuffers_.resize(swapChainImageViews_.size());
        for (size_t i = 0; i < swapChainImageViews_.size(); ++i) {
            VkImageView attachments[] = {
                **swapChainImageViews_[i]
            };

            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = **renderPass_;
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = attachments;
            framebufferInfo.width = swapChainExtent_.width;
            framebufferInfo.height = swapChainExtent_.height;
            framebufferInfo.layers = 1;
            
            VK_SUCCESS_OR_THROW(VulkanFramebuffer::create(swapChainFramebuffers_[i], **device_, framebufferInfo),
                                "Failed to create framebuffer");
        }
    }
    
    void createCommandPool() {
        QueueFamilyIndices queueFamilyIndices = findQueueFamilies(physicalDevice_, **surface_);

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();
        
        VK_SUCCESS_OR_THROW(VulkanCommandPool::create(commandPool_, **device_, poolInfo),
                            "Failed to create command pool");
    }
    
    void createSyncObjects() {
        // TODO: maxFramesInFlight_ refactor
        for (int frameIndex = 0; frameIndex < maxFramesInFlight_; ++frameIndex) {
            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // Init fence as signaled so first frame isn't blocked
            
            VK_SUCCESS_OR_THROW(VulkanSemaphore::create(imageAvailableSemaphores_[frameIndex], **device_, semaphoreInfo),
                                "Failed to create image available semaphore.");
            VK_SUCCESS_OR_THROW(VulkanSemaphore::create(renderFinishedSemaphores_[frameIndex], **device_, semaphoreInfo),
                                "Failed to create render finished semaphore.");
            VK_SUCCESS_OR_THROW(VulkanFence::create(inFlightFences_[frameIndex], **device_, fenceInfo),
                                "Failed to create in-flight fence.");
        }
    }
    
    void createComputeSyncObjects() {
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        
        // TODO: maxFramesInFlight_ refactor
        for (int frameIndex = 0; frameIndex < maxFramesInFlight_; ++frameIndex) {
            computeSemaphores_[frameIndex].resize(computePasses_.size());
            for (int i = 0; i < computePasses_.size(); i++) {
                VK_SUCCESS_OR_THROW(VulkanSemaphore::create(computeSemaphores_[frameIndex][i], **device_, semaphoreInfo),
                                    "Failed to create compute semaphore");
            }
        }
    }
    
    void createCommandBuffers() {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = **commandPool_;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        // TODO: maxframes refactor
        allocInfo.commandBufferCount = 2;
        
        VK_SUCCESS_OR_THROW(vkAllocateCommandBuffers(**device_, &allocInfo, commandBuffers_.data()),
                            "Failed to allocate command buffers");
        VK_SUCCESS_OR_THROW(vkAllocateCommandBuffers(**device_, &allocInfo, computeCommandBuffers_.data()),
                            "Failed to allocate command buffers");
    }
    
private: // Additional helper functions
    // TODO: Might be nice to move swapchain fields into dedicated object
    void recreateSwapChain() {
        int width = 0, height = 0;
        glfwGetFramebufferSize(window_.get(), &width, &height);
        while (width == 0 || height == 0){
            glfwGetFramebufferSize(window_.get(), &width, &height);
            glfwWaitEvents(); // Sleep the thread until something happens to the window
        }
        vkDeviceWaitIdle(**device_);
        
        createSwapChain();
        createSwapChainImageViews();
        createFramebuffers();
    }
    
    void logSupportedExtensions () {
        std::cout << "Available Extensions: " << std::endl;
        for (const auto& extension :
             readVkVector<VkExtensionProperties, const char*>(vkEnumerateInstanceExtensionProperties)) {
            std::cout << "\t" << extension.extensionName << std::endl;
        }
    }
    
    void logAvailableLayers() {
        std::cout << "Available Layers: " << std::endl;
        for (const auto& layerProperties :
             readVkVector<VkLayerProperties>(vkEnumerateInstanceLayerProperties)) {
            std::cout << "\t" << layerProperties.layerName << std::endl;
        }
    }
    
public:
    // Add renderable to main render pass
    void addRenderable(std::unique_ptr<Renderable>&& renderable) {
        renderables_.emplace_back(std::move(renderable));
    }
    
    // Add compute stage to occur before graphics
    void addComputeStage(std::unique_ptr<ComputeMaterial>&& computeMaterial) {
        computePasses_.emplace_back(std::move(computeMaterial));
    }

    // Add callback to be called before drawing each frame
    void addPreDrawCallback(std::shared_ptr<std::function<void(VulkanApp&, uint32_t)>>& callback) {
        preDrawCallbacks_.push_back(callback);
    }
    
public: // Public getters
    VkDevice getDevice() {
        return **device_;
    }
    
    VkPhysicalDevice getPhysicalDevice() {
        return physicalDevice_;
    }
    
    VkCommandPool getCommandPool() {
        return **commandPool_;
    }
    
    VkQueue getGraphicsQueue() {
        return graphicsQueue_;
    }
    
    VkExtent2D getSwapchainExtent() {
        return swapChainExtent_;
    }
    
    VkRenderPass getRenderPass() {
        return **renderPass_;
    }
private: // Member variables
    // Application constants
    const uint32_t windowHeight_;
    const uint32_t windowWidth_;
    // TODO Use vectors everywhere so this can be customized
    static const uint32_t maxFramesInFlight_ = 2;

    // GLFW Variables
    std::unique_ptr<GLFWwindow, std::function<void(GLFWwindow*)>> window_;
    bool frameBufferResized_;

    // Vulkan Instance & Device Handles
    std::unique_ptr<VulkanInstance> instance_;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    std::unique_ptr<VulkanDevice> device_;
    
    // Hardware Queues
    VkQueue graphicsQueue_;
    VkQueue presentQueue_;
    VkQueue computeQueue_;
    
    // Surface & Swapchain
    std::unique_ptr<VulkanSurface> surface_;
    std::unique_ptr<VulkanSwapchain> swapChain_;
    
    // Swap Chain Images
    std::vector<VkImage> swapChainImages_;
    VkFormat swapChainImageFormat_;
    VkExtent2D swapChainExtent_;
    std::vector<std::unique_ptr<VulkanImageView>> swapChainImageViews_;

    // TODO: Eventually create some kind of render graph system
    std::unique_ptr<VulkanRenderPass> renderPass_;

    // Framebuffers
    std::vector<std::unique_ptr<VulkanFramebuffer>> swapChainFramebuffers_;
    
    // Command & Descriptor Pools
    // TODO: Can be owned here but need to match what is needed
    std::unique_ptr<VulkanCommandPool> commandPool_;
    
    // Current frame index
    uint32_t currentFrameIndex_ = 0;
    
    // TODO: Replace 2 w/ constant max_frame_count
    std::array<std::unique_ptr<VulkanSemaphore>, 2> imageAvailableSemaphores_;
    std::array<std::unique_ptr<VulkanSemaphore>, 2> renderFinishedSemaphores_;
    std::array<std::unique_ptr<VulkanFence>, 2> inFlightFences_;
    
    std::array<VkCommandBuffer, 2> commandBuffers_;
    std::array<VkCommandBuffer, 2> computeCommandBuffers_;
    
    std::vector<std::unique_ptr<Renderable>> renderables_{};
    std::vector<std::unique_ptr<ComputeMaterial>> computePasses_{};
    std::vector<std::shared_ptr<std::function<void(VulkanApp&,uint32_t)>>> preDrawCallbacks_{};
    
    std::array<std::vector<std::unique_ptr<VulkanSemaphore>>, 2> computeSemaphores_;
    bool computeSyncInitialized_ = false;
};
