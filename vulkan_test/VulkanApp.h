#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "VkUtil.h"
#include "DeviceSelection.h"
#include "Buffer.h"
#include "RenderGraph.h"
#include "Image.h"

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

template<uint MAX_FRAMES>
class VulkanApp {
public:
    VulkanApp(const uint32_t windowHeight,
              const uint32_t windowWidth)
    : windowHeight_(windowHeight), windowWidth_(windowWidth) {}
    
    void init(bool forceFullFloatColor=false) {
        initWindow();
        initVulkan(forceFullFloatColor);
    }
    
    void run() {
        mainLoop();
    }

    void holdWindow() {
        while(!glfwWindowShouldClose(window_.get())) {
            // Do nothing
        }
        vkDeviceWaitIdle(**device_);
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

    void initVulkan(bool forceFullFloatColor) {
        createInstance();
        createSurface();
        choosePhysicalDevice();
        createLogicalDevice();
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
        vkDeviceWaitIdle(**device_);
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

public:    
    void createRenderPass(bool forceFullFloatColor) {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = forceFullFloatColor? VK_FORMAT_R32G32B32A32_SFLOAT : swapChainImageFormat_;
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

        VkAttachmentDescription depthAttachment{};
        depthAttachment.format = VK_FORMAT_D32_SFLOAT;
        depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; // clear data before loading
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; 
        depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthAttachmentRef{};
        depthAttachmentRef.attachment = 1;
        depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        
        subpass.colorAttachmentCount = 1; // matches inputs to shader
        subpass.pColorAttachments = &colorAttachmentRef;
        subpass.pDepthStencilAttachment = &depthAttachmentRef;
        
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        
        std::array<VkAttachmentDescription, 2> attachments = {colorAttachment, depthAttachment};
        
        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 2;
        renderPassInfo.pAttachments = attachments.data();
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;
        
        VK_SUCCESS_OR_THROW(VulkanRenderPass::create(renderPass_, **device_, renderPassInfo),
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
    
    void createCommandBuffers() {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = **commandPool_;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = MAX_FRAMES;
        
        VK_SUCCESS_OR_THROW(vkAllocateCommandBuffers(**device_, &allocInfo, commandBuffers_.data()),
                            "Failed to allocate command buffers");
        VK_SUCCESS_OR_THROW(vkAllocateCommandBuffers(**device_, &allocInfo, computeCommandBuffers_.data()),
                            "Failed to allocate command buffers");
    }

    void createDepthTexture() {
        Image::create(depthTexture_, 
                      swapChainExtent_.width, 
                      swapChainExtent_.height, 
                      VK_FORMAT_D32_SFLOAT, 
                      VK_IMAGE_TILING_OPTIMAL, 
                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 
                      getDevice(),
                      getPhysicalDevice(),
                      VK_IMAGE_ASPECT_DEPTH_BIT);
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
    // TODO: Replace with CPU render graph nodes
    // Add callback to be called before drawing each frame
    void addPreDrawCallback(std::shared_ptr<std::function<void(VulkanApp&, uint32_t)>>& callback) {
        preDrawCallbacks_.push_back(callback);
    }
    
    void setRenderGraph(std::unique_ptr<RenderGraph<MAX_FRAMES>>&& renderGraph) {
        renderGraph_ = std::move(renderGraph);
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
    
    VkQueue getComputeQueue() {
        return computeQueue_;
    }
    
    VkQueue getPresentQueue() {
        return presentQueue_;
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

    // Depth Texture
    std::unique_ptr<Image> depthTexture_;

    // TODO: Move into RenderableNode
    std::unique_ptr<VulkanRenderPass> renderPass_;

    // Framebuffers
    std::vector<std::unique_ptr<VulkanFramebuffer>> swapChainFramebuffers_;
    
    // Command & Descriptor Pools
    // TODO: Can be owned here but need to match what is needed
    std::unique_ptr<VulkanCommandPool> commandPool_;
    
    // Current frame index
    uint32_t currentFrameIndex_ = 0;
    
    std::array<VkCommandBuffer, MAX_FRAMES> commandBuffers_;
    std::array<VkCommandBuffer, MAX_FRAMES> computeCommandBuffers_;
    
    std::vector<std::shared_ptr<std::function<void(VulkanApp&,uint32_t)>>> preDrawCallbacks_{};
    std::unique_ptr<RenderGraph<MAX_FRAMES>> renderGraph_{};
};
