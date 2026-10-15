#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "core/VkUtil.h"
#include "core/DeviceSelection.h"
#include "resources/Buffer.h"
#include "graph/RenderGraph.h"
#include "resources/Image.h"
#include "core/ResourceManager.h"
#include "core/VulkanContext.h"

#include <glm/glm.hpp>

#include <iostream>

template<uint MAX_FRAMES>
class VulkanApp {
public:
    VulkanApp(const uint32_t windowHeight,
              const uint32_t windowWidth)
    : windowHeight_(windowHeight), windowWidth_(windowWidth) {}
    
    // enableValidation turns on the Khronos validation layer (when installed), which reports
    // API misuse to std::cerr
    void init(bool forceFullFloatColor=false, bool enableValidation=false) {
        initWindow();
        initVulkan(forceFullFloatColor, enableValidation);
    }
    
    void run() {
        mainLoop();
    }

    void holdWindow() {
        while(!glfwWindowShouldClose(window_.get())) {
            // Do nothing
        }
        context_->waitIdleAll();
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

    void initVulkan(bool forceFullFloatColor, bool enableValidation) {
        createContext(enableValidation);
        createSwapChain();
        createSwapChainImageViews();
        createDepthTexture();
        createRenderPass(forceFullFloatColor);
        createFramebuffers();
        createCommandPool();
        createCommandBuffers();
    }
    
    void mainLoop() {
        while(!glfwWindowShouldClose(window_.get())) {
            glfwPollEvents();
            drawFrame();
        }
        context_->waitIdleAll();
    }
    
public:
    void drawFrame() {
        // Wait for previous frame to complete
        renderGraph_->waitUntilComplete(currentFrameIndex_);
        
        // Perform any pre-draw actions
        for (auto& callback : preDrawCallbacks_) {
            (*callback)(*this, currentFrameIndex_);
        }

        // Construct our evaluation context
        RenderEvalContext ctx {
            currentFrameIndex_,
            swapChainExtent_,
            0, {},
            **swapChain_,
            frameBufferResized_,
        };
        
        for (auto& framebuffer : swapChainFramebuffers_) {
            ctx.frameBuffers.push_back(**framebuffer);
        }
        
        // Attempt to exectue the render graph
        renderGraph_->submit(ctx);
        
        // The graph might have exited early if the swapchain is out of date
        if (ctx.shouldRecreateSwapChain) {
            frameBufferResized_ = false;
            recreateSwapChain();
            return;
        }

        // Increment frame index
        currentFrameIndex_ = (this->currentFrameIndex_ + 1) % MAX_FRAMES;
    }
    
private: // Vulkan Initialization Functions
    void createContext(bool enableValidation) {
        logSupportedExtensions();
        logAvailableLayers();
        
        VulkanContextConfig config;
        config.applicationName = "VulkanApp";
        config.enableValidation = enableValidation;
        
        uint32_t glfwExtensionCount = 0;
        const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
        config.instanceExtensions.assign(glfwExtensions, glfwExtensions + glfwExtensionCount);
        
        config.createSurface = [window = window_.get()](VkInstance instance, VkSurfaceKHR* surface) {
            return glfwCreateWindowSurface(instance, window, nullptr, surface);
        };
        
        context_ = std::make_unique<VulkanContext>(std::move(config));
    }
    
    void createSwapChain() {
        SwapChainSupportDetails swapChainSupport = querySwapChainSupport(context_->physicalDevice(), context_->surface());
        
        auto surfaceFormat = swapChainSupport.chooseSwapSurfaceFormat();
        auto presentMode = swapChainSupport.chooseSwapPresentMode();
        auto extent = swapChainSupport.chooseSwapExtent(window_.get());
        
        uint32_t imageCount = std::min(swapChainSupport.capabilities.minImageCount + 1,
                                       swapChainSupport.capabilities.maxImageCount);
        
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
        
        VK_SUCCESS_OR_THROW(VulkanSwapchain::create(swapChain_, context_->device(), createInfo),
                            "Failed to create swap chain.");
        
        // Retrieve swapchain images
        swapChainImages_ = readVkVector<VkImage, VkDevice, VkSwapchainKHR>(context_->device(), **swapChain_,
                                                                                vkGetSwapchainImagesKHR);
        
        // Cache swapchain properties
        swapChainImageFormat_ = surfaceFormat.format;
        swapChainExtent_ = extent;
    }
    
    void createSwapChainImageViews() {
        swapChainImageViews_.resize(swapChainImages_.size());
        
        for (size_t i = 0; i < swapChainImages_.size(); ++i) {
            VK_SUCCESS_OR_THROW(VulkanImageView::createForImageWithFormat(swapChainImageViews_[i],
                                                                          context_->device(),
                                                                          swapChainImages_[i],
                                                                          swapChainImageFormat_),
                                "Failed to create image view for swapchain");
        }
    }

public:    
    void createRenderPass(bool forceFullFloatColor) {
        VK_SUCCESS_OR_THROW(VulkanRenderPass::createColorDepth(renderPass_,
                                                               context_->device(),
                                                               forceFullFloatColor? VK_FORMAT_R32G32B32A32_SFLOAT : swapChainImageFormat_,
                                                               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
                            "Failed to create render pass");
    }
private:
    
    void createFramebuffers() {
        swapChainFramebuffers_.resize(swapChainImageViews_.size());
        for (size_t i = 0; i < swapChainImageViews_.size(); ++i) {
            VkImageView attachments[] = {
                **swapChainImageViews_[i],
                depthTexture_->getImageView()
            };

            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = **renderPass_;
            framebufferInfo.attachmentCount = 2;
            framebufferInfo.pAttachments = attachments;
            framebufferInfo.width = swapChainExtent_.width;
            framebufferInfo.height = swapChainExtent_.height;
            framebufferInfo.layers = 1;
            
            VK_SUCCESS_OR_THROW(VulkanFramebuffer::create(swapChainFramebuffers_[i], context_->device(), framebufferInfo),
                                "Failed to create framebuffer");
        }
    }
    
    void createCommandPool() {
        commandPool_ = context_->createCommandPool(QueueType::Graphics);
    }
    
    void createCommandBuffers() {
        commandBuffers_ = commandPool_->allocateArray<MAX_FRAMES>();
        computeCommandBuffers_ = commandPool_->allocateArray<MAX_FRAMES>();
    }

    void createDepthTexture() {
        VK_SUCCESS_OR_THROW(Image::create(depthTexture_,
                                          *context_,
                                          swapChainExtent_.width,
                                          swapChainExtent_.height,
                                          VK_FORMAT_D32_SFLOAT,
                                          VK_IMAGE_TILING_OPTIMAL,
                                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                          VK_IMAGE_ASPECT_DEPTH_BIT),
                            "Failed to create depth texture");
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
        context_->waitIdleAll();
        
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
    // TODO: Replace with CPU render graph nodes
    // Add callback to be called before drawing each frame
    void addPreDrawCallback(std::shared_ptr<std::function<void(VulkanApp&, uint32_t)>>& callback) {
        preDrawCallbacks_.push_back(callback);
    }
    
    void setRenderGraph(std::unique_ptr<RenderGraph<MAX_FRAMES>>&& renderGraph) {
        renderGraph_ = std::move(renderGraph);
    }
    
public: // Public getters
    VulkanContext& getContext() {
        return *context_;
    }
    
    VkExtent2D getSwapchainExtent() {
        return swapChainExtent_;
    }
    
    VkRenderPass getRenderPass() {
        return **renderPass_;
    }
    
    VkSwapchainKHR getSwapchain() {
        return **swapChain_;
    }
    
    std::array<VkCommandBuffer, MAX_FRAMES> getGraphicsCommandBuffers() {
        return commandBuffers_;
    }
    
    std::array<VkCommandBuffer, MAX_FRAMES> getComputeCommandBuffers() {
        return computeCommandBuffers_;
    }

    VkImage getSwapchainImage(size_t frameIndex) {
        return swapChainImages_.at(frameIndex);
    }

    VkImageView getSwapchainImageView(size_t imageIndex) {
        return *swapChainImageViews_.at(imageIndex)->get();
    }
private: // Member variables
    // Application constants
    const uint32_t windowHeight_;
    const uint32_t windowWidth_;

    // GLFW Variables
    std::unique_ptr<GLFWwindow, std::function<void(GLFWwindow*)>> window_;
    bool frameBufferResized_;

    // Instance, device, queues & managed resources. Declared after the window (which its
    // surface depends on) and before everything below, which must be destroyed first.
    std::unique_ptr<VulkanContext> context_;
    
    // Swapchain
    std::unique_ptr<VulkanSwapchain> swapChain_;
    
    // Swap Chain Images
    std::vector<VkImage> swapChainImages_;
    VkFormat swapChainImageFormat_;
    VkExtent2D swapChainExtent_;
    std::vector<std::unique_ptr<VulkanImageView>> swapChainImageViews_;

    // Depth Texture
    Handle<Image> depthTexture_;

    // TODO: Move into RenderableNode
    std::unique_ptr<VulkanRenderPass> renderPass_;

    // Framebuffers
    std::vector<std::unique_ptr<VulkanFramebuffer>> swapChainFramebuffers_;
    
    // Command & Descriptor Pools
    // TODO: Can be owned here but need to match what is needed
    std::unique_ptr<CommandPool> commandPool_;
    
    // Current frame index
    uint32_t currentFrameIndex_ = 0;
    
    std::array<VkCommandBuffer, MAX_FRAMES> commandBuffers_;
    std::array<VkCommandBuffer, MAX_FRAMES> computeCommandBuffers_;
    
    std::vector<std::shared_ptr<std::function<void(VulkanApp&,uint32_t)>>> preDrawCallbacks_{};
    std::unique_ptr<RenderGraph<MAX_FRAMES>> renderGraph_{};
};
