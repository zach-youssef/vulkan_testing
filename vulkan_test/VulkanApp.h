#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "VkTypes.h"
#include "VkUtil.h"
#include "DeviceSelection.h"
#include "QueueFamilyIndices.h"
#include "SwapChainSupportDetails.h"
#include "Frame.h"
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

// TODO: This 100% should not be here, temporarily until render data is decoupled
struct Vertex {
    glm::vec2 pos;
    glm::vec3 color;
    glm::vec2 texCoord0;
    
    static VkVertexInputBindingDescription getBindingDescription() {
        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;
        bindingDescription.stride = sizeof(Vertex);
        // Move to next data entry after each vertex (alternative is after each instance)
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return bindingDescription;
    }
    
    static std::array<VkVertexInputAttributeDescription, 3> getAttributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions{};
        
        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[0].offset = offsetof(Vertex, pos);
        
        attributeDescriptions[1].binding = 0;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[1].offset = offsetof(Vertex, color);
        
        attributeDescriptions[2].binding = 0;
        attributeDescriptions[2].location = 2;
        attributeDescriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[2].offset = offsetof(Vertex, texCoord0);

        return attributeDescriptions;
    }
};

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
        // TODO: Some of these need to be configurable
        // (data inside a frame, graphics pipelines, descriptor sets, etc)
        createInstance();
        createSurface();
        choosePhysicalDevice();
        createLogicalDevice();
        createSwapChain();
        createSwapChainImageViews();
        createRenderPass();
        // ------------------------------
        //createDescriptorSetLayout();
        //createGraphicsPipeline();
        // -------------------------------
        createFramebuffers();
        createCommandPool();
        // -------------------------------
        //createDescriptorPool();
        //createTextureImage();
        //createTextureSampler();
        //createVertexBuffer();
        //createIndexBuffer();
        // --------------------------------
        //initFrames(); // Replace w/ just sync objects
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
        
        // Only reset fence here now that we know we will be doing work
        vkResetFences(**device_, 1, (*inFlightFences_[currentFrameIndex_]).get());
        
        // Grab command buffer
        auto& commandBuffer = commandBuffers_[currentFrameIndex_];
        
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

            // Record the renderable commands
            //renderable->recordCommandBuffer(commandBuffer, currentFrameIndex_, swapChainExtent_);
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
        VkSemaphore wait[] = {**imageAvailableSemaphores_[currentFrameIndex_]};
        VkSemaphore signal[] = {**renderFinishedSemaphores_[currentFrameIndex_]};
        submitInfo.waitSemaphoreCount = 1;//static_cast<uint32_t>(waitSemaphores.size());
        submitInfo.pWaitSemaphores = wait;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        submitInfo.signalSemaphoreCount = 1;//static_cast<uint32_t>(signalSemaphores.size());
        submitInfo.pSignalSemaphores = signal;

        VK_SUCCESS_OR_THROW(vkQueueSubmit(graphicsQueue_, 1, &submitInfo, **inFlightFences_[currentFrameIndex_]),
                            "Failed to submit draw command buffer.");

        /*renderable->submit(imageIndex,
                           commandBuffer,
                           graphicsQueue_,
                           **swapChain_,
                           **inFlightFences_[currentFrameIndex_],
                           {**imageAvailableSemaphores_[currentFrameIndex_]},
                           {**renderFinishedSemaphores_[currentFrameIndex_]});*/

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
    
    void createDescriptorSetLayout() {
        VkDescriptorSetLayoutBinding uboLayoutBinding{};
        uboLayoutBinding.binding = 0;
        uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uboLayoutBinding.descriptorCount = 1;
        uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        uboLayoutBinding.pImmutableSamplers = nullptr; // Optional, only relevant for image sampling
        
        VkDescriptorSetLayoutBinding imageLayoutBinding{};
        imageLayoutBinding.binding = 1;
        imageLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        imageLayoutBinding.descriptorCount = 1;
        imageLayoutBinding.pImmutableSamplers = nullptr;
        imageLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        
        std::array<VkDescriptorSetLayoutBinding, 2> layoutBindings {uboLayoutBinding, imageLayoutBinding};
        
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<uint32_t>(layoutBindings.size());
        layoutInfo.pBindings = layoutBindings.data();
        
        VK_SUCCESS_OR_THROW(VulkanDescriptorSetLayout::create(descriptorSetLayout_, **device_, layoutInfo),
                            "Failed to create descriptor set layout.");
    }
    
    void createGraphicsPipeline() {
        const std::string path = "/Users/zyoussef/code/vulkan_test/vulkan_test/shaders";
        auto vertShaderCode = readFile(path + "/vert.spv");
        auto fragShaderCode = readFile(path + "/frag.spv");
        
        auto vertShaderModule = createShaderModule(vertShaderCode);
        auto fragShaderModule = createShaderModule(fragShaderCode);
        
        VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
        vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
        
        vertShaderStageInfo.module = **vertShaderModule;
        vertShaderStageInfo.pName = "main";
        
        VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
        fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragShaderStageInfo.module = **fragShaderModule;
        fragShaderStageInfo.pName = "main";

        VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};
        
        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        
        auto bindingDescription = Vertex::getBindingDescription();
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputInfo.vertexBindingDescriptionCount = 1;
        vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
        
        auto attributeDescriptions = Vertex::getAttributeDescriptions();
        vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
        vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();
        
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssembly.primitiveRestartEnable = VK_FALSE;
        
        std::vector<VkDynamicState> dynamicStates = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR
        };

        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates = dynamicStates.data();
        
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(swapChainExtent_.width);
        viewport.height = static_cast<float>(swapChainExtent_.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        
        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = swapChainExtent_;
        
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;
        
        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.depthBiasEnable = VK_FALSE;
        rasterizer.depthBiasConstantFactor = 0.0f; // Optional
        rasterizer.depthBiasClamp = 0.0f; // Optional
        rasterizer.depthBiasSlopeFactor = 0.0f; // Optional
        
        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = VK_FALSE;
        colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; // Optional
        colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO; // Optional
        colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD; // Optional
        colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; // Optional
        colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; // Optional
        colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD; // Optional
        
        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.logicOpEnable = VK_FALSE;
        colorBlending.logicOp = VK_LOGIC_OP_COPY; // Optional
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;
        colorBlending.blendConstants[0] = 0.0f; // Optional
        colorBlending.blendConstants[1] = 0.0f; // Optional
        colorBlending.blendConstants[2] = 0.0f; // Optional
        colorBlending.blendConstants[3] = 0.0f; // Optional
        
        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        multisampling.minSampleShading = 1.0f; // Optional
        multisampling.pSampleMask = nullptr; // Optional
        multisampling.alphaToCoverageEnable = VK_FALSE; // Optional
        multisampling.alphaToOneEnable = VK_FALSE; // Optional
        
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = descriptorSetLayout_->get();
        pipelineLayoutInfo.pushConstantRangeCount = 0; // Optional
        pipelineLayoutInfo.pPushConstantRanges = nullptr; // Optional
        
        VK_SUCCESS_OR_THROW(VulkanPipelineLayout::create(pipelineLayout_, **device_, pipelineLayoutInfo),
                            "Failed to create pipeline layout");
        
        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shaderStages;

        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = nullptr; // Optional
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = &dynamicState;
        
        pipelineInfo.layout = **pipelineLayout_;
        pipelineInfo.renderPass = **renderPass_;
        pipelineInfo.subpass = 0;

        pipelineInfo.basePipelineHandle = VK_NULL_HANDLE; // Optional
        pipelineInfo.basePipelineIndex = -1; // Optional
        
        VK_SUCCESS_OR_THROW(VulkanGraphicsPipeline::create(pipeline_, **device_, pipelineInfo),
                            "Failed to create graphics pipeline");
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
    
    void createDescriptorPool() {
        std::array<VkDescriptorPoolSize, 2> poolSize{};
        
        poolSize[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSize[0].descriptorCount = static_cast<uint32_t>(maxFramesInFlight_);
        
        poolSize[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize[1].descriptorCount = static_cast<uint32_t>(maxFramesInFlight_);
        
        
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSize.size());
        poolInfo.pPoolSizes = poolSize.data();
        poolInfo.maxSets = static_cast<uint32_t>(maxFramesInFlight_);
        
        VK_SUCCESS_OR_THROW(VulkanDescriptorPool::create(descriptorPool_, **device_, poolInfo),
                            "Failed to create descriptor pool");
    }
    
    void createTextureImage() {
        static const std::string path = "/Users/zyoussef/code/vulkan_test/vulkan_test/textures";
        
        int width, height, channels;
        
        stbi_uc* pixels = stbi_load((path + "/texture.jpg").c_str(), &width, &height, &channels, STBI_rgb_alpha);
        
        if (!pixels) {
            throw std::runtime_error("Failed to load texture image.");
        }
        
        VkDeviceSize imageSize = width * width * 4;
        
        std::unique_ptr<Buffer<uint8_t>> stagingBuffer;
        VK_SUCCESS_OR_THROW(Buffer<uint8_t>::create(stagingBuffer, imageSize,
                                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                                    **device_, physicalDevice_),
                            "Failed to create image staging buffer");
        
        stagingBuffer->mapAndExecute(0, imageSize, [pixels, imageSize] (void* data) {
            memcpy(data, pixels, static_cast<size_t>(imageSize));
        });
        
        stbi_image_free(pixels);
        
        VK_SUCCESS_OR_THROW(Image::create(textureImage_,
                                          width, height,
                                          VK_FORMAT_R8G8B8A8_SRGB,
                                          VK_IMAGE_TILING_OPTIMAL,
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                          **device_, physicalDevice_),
                            "Failed to create image.");
        
        transitionImageLayout(textureImage_->getImage(),
                              VK_FORMAT_R8G8B8A8_SRGB,
                              VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        
        copyBufferToImage(stagingBuffer->getBuffer(),
                          textureImage_->getImage(),
                          width, height);
        
        transitionImageLayout(textureImage_->getImage(),
                              VK_FORMAT_R8G8B8A8_SRGB,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    
    void createTextureSampler() {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        
        // Alternative is VK_FILTER_NEAREST
        // Mag is for oversampling, min is for undersampling
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        
        // Alternatives are
        // - MIRRORED_REPEAT
        // - CLAMP_TO_EDGE (or MIRRORED_)
        // CLAMP_TO_BORDER
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        
        // Can be set to false if device doesn't support it
        samplerInfo.anisotropyEnable = VK_TRUE;
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
        samplerInfo.maxAnisotropy = properties.limits.maxSamplerAnisotropy;
        
        // Only relevant if using CLAMP_TO_BORDER adress mode
        samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        
        // Use real coordinates instead of UVs
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        
        // I don't understand this but it has some use with shadow maps?
        samplerInfo.compareEnable = VK_FALSE;
        samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
        
        // To be revisted when we implement mip maps
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.mipLodBias = 0.0f;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = 0.0f;
        
        VK_SUCCESS_OR_THROW(VulkanSampler::create(textureSampler_, **device_, samplerInfo),
                            "Failed to create texture sampler");
    }
    
    void createVertexBuffer() {
        createAndInitializeBuffer<Vertex>(vertexBuffer_, vertexData, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    }
    
    void createIndexBuffer() {
        createAndInitializeBuffer<uint16_t>(indexBuffer_, indexData, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    }
    void initFrames() {
        // createDescriptorSets
        std::vector<VkDescriptorSetLayout> layouts(maxFramesInFlight_, **descriptorSetLayout_);
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = **descriptorPool_;
        allocInfo.descriptorSetCount = static_cast<uint32_t>(maxFramesInFlight_);
        allocInfo.pSetLayouts = layouts.data();
        
        std::array<VkDescriptorSet, maxFramesInFlight_> descriptorSets;
        
        VK_SUCCESS_OR_THROW(vkAllocateDescriptorSets(**device_, &allocInfo, descriptorSets.data()),
                            "Failed to allocate descriptor sets.");
        
        for (int i = 0; i < maxFramesInFlight_; ++i) {
            frames_[i] = std::make_unique<Frame>(**device_,
                                                 **commandPool_,
                                                 physicalDevice_,
                                                 descriptorSets[i],
                                                 textureImage_->getImageView(),
                                                 **textureSampler_);
        }
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
    
    void createCommandBuffers() {
        // TODO: maxframes refactor
        for (int frameIndex = 0; frameIndex < maxFramesInFlight_; ++frameIndex) {
            VkCommandBufferAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.commandPool = **commandPool_;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;
            
            VK_SUCCESS_OR_THROW(vkAllocateCommandBuffers(**device_, &allocInfo, &commandBuffers_[frameIndex]),
                                "Failed to allocate command buffers");
        }
    }
    
private: // Additional helper functions
    // TODO: Anything in this section probably belongs elsewhere
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
    
    void startRenderPass(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
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
    }
    
    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
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
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, **pipeline_);
        
        VkBuffer vertexBuffers[] {vertexBuffer_->getBuffer()};
        VkDeviceSize offsets[] {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
        
        vkCmdBindIndexBuffer(commandBuffer, indexBuffer_->getBuffer(), 0, VK_INDEX_TYPE_UINT16);
        
        vkCmdBindDescriptorSets(commandBuffer,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                **pipelineLayout_,
                                0, 1,
                                frames_[currentFrameIndex_]->getDescriptorSet(),
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
                         static_cast<uint32_t>(indexData.size()),
                         1 /*num instances*/,
                         0 /*offset into buffer*/,
                         0 /*offset to add to indices*/,
                         0 /* instancing offset*/);
        
        vkCmdEndRenderPass(commandBuffer);
        
        VK_SUCCESS_OR_THROW(vkEndCommandBuffer(commandBuffer),
                            "Failed to reccord command buffer.");
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
    
    std::unique_ptr<VulkanShaderModule> createShaderModule(const std::vector<char>& code) {
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
        
        std::unique_ptr<VulkanShaderModule> shaderModule;
        
        
        VK_SUCCESS_OR_THROW(VulkanShaderModule::create(shaderModule, **device_, createInfo),
                            "Failed to create shader module.");
        
        return shaderModule;
    }
    
    void transitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout) {
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
                destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
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
        }, graphicsQueue_);
    }
    
    void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
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
        }, graphicsQueue_);
    }
    
    template<typename Data>
    void createAndInitializeBuffer(std::unique_ptr<Buffer<Data>>& buffer, const std::vector<Data>& data, VkBufferUsageFlags usage) {
        size_t bufferSize = sizeof(Data) * data.size();
        
        std::unique_ptr<Buffer<Data>> stagingBuffer;
        VK_SUCCESS_OR_THROW(Buffer<Data>::create(stagingBuffer,
                                                   data.size(),
                                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                                   **device_,
                                                   physicalDevice_),
                            "Failed to create staging buffer");
        
        stagingBuffer->mapAndExecute(0, bufferSize, [bufferSize, &data] (void* mappedData){
            memcpy(mappedData, data.data(), bufferSize);
        });
        
        VK_SUCCESS_OR_THROW(Buffer<Data>::create(buffer,
                                                   data.size(),
                                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                   **device_,
                                                   physicalDevice_),
                            "Failed to create gpu buffer");
        
        copyBuffer(stagingBuffer->getBuffer(), buffer->getBuffer(), bufferSize);
    }
    
    void issueSingleTimeCommand(std::function<void(VkCommandBuffer)> op, VkQueue queue) {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = **commandPool_;
        allocInfo.commandBufferCount = 1;
        
        VkCommandBuffer commandBuffer;
        vkAllocateCommandBuffers(**device_, &allocInfo, &commandBuffer);
        
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        
        vkBeginCommandBuffer(commandBuffer, &beginInfo);
        
        op(commandBuffer);
        
        vkEndCommandBuffer(commandBuffer);
        
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        
        vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
        
        vkFreeCommandBuffers(**device_, **commandPool_, 1, &commandBuffer);
    }

    void copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size) {
        issueSingleTimeCommand([src, dst, size](VkCommandBuffer commandBuffer){
            VkBufferCopy copyRegion{};
            copyRegion.srcOffset = 0;
            copyRegion.dstOffset = 0;
            copyRegion.size = size;
            vkCmdCopyBuffer(commandBuffer, src, dst, 1 /*regionCount*/, &copyRegion);
        }, graphicsQueue_);
    }
    
public:
    void addRenderable(std::unique_ptr<Renderable>&& renderable) {
        renderables_.emplace_back(std::move(renderable));
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
    
    // TODO: Remove, these def needs to be decoupled
    const std::vector<Vertex> vertexData = {
        {{-0.5f, -0.5f},{1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
        {{0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
        {{0.5f, 0.5f},  {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
        {{-0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f}}
    };
    const std::vector<uint16_t> indexData = {
        0, 1, 2,
        2, 3, 0
    };

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
    
    // Surface & Swapchain
    std::unique_ptr<VulkanSurface> surface_;
    std::unique_ptr<VulkanSwapchain> swapChain_;
    
    // Swap Chain Images
    std::vector<VkImage> swapChainImages_;
    VkFormat swapChainImageFormat_;
    VkExtent2D swapChainExtent_;
    std::vector<std::unique_ptr<VulkanImageView>> swapChainImageViews_;

    // TODO: These should be specifiable somehow
    // (Renderpass, descriptor layouts, pipeline layout, etc)
    std::unique_ptr<VulkanRenderPass> renderPass_;
    std::unique_ptr<VulkanDescriptorSetLayout> descriptorSetLayout_;
    std::unique_ptr<VulkanPipelineLayout> pipelineLayout_;
    std::unique_ptr<VulkanGraphicsPipeline> pipeline_;

    // Framebuffers
    std::vector<std::unique_ptr<VulkanFramebuffer>> swapChainFramebuffers_;
    
    // Command & Descriptor Pools
    // TODO: Can be owned here but need to match what is needed
    std::unique_ptr<VulkanCommandPool> commandPool_;
    // TODO: moving to material
    std::unique_ptr<VulkanDescriptorPool> descriptorPool_;
    
    // Per-frame data
    // TODO: Need to figure out how to make the fields on 'Frame' specifiable as well
    std::array<std::unique_ptr<Frame>, maxFramesInFlight_> frames_;
    uint32_t currentFrameIndex_ = 0;
    
    // Render Data
    // TODO: This definitely should be defined somewhere else & provided in draw calls
    std::unique_ptr<Buffer<Vertex>> vertexBuffer_;
    std::unique_ptr<Buffer<uint16_t>> indexBuffer_;
    std::unique_ptr<Image> textureImage_;
    std::unique_ptr<VulkanSampler> textureSampler_;
    
    
    // TODO: Replace 2 w/ constant max_frame_count
    std::array<std::unique_ptr<VulkanSemaphore>, 2> imageAvailableSemaphores_;
    std::array<std::unique_ptr<VulkanSemaphore>, 2> renderFinishedSemaphores_;
    std::array<std::unique_ptr<VulkanFence>, 2> inFlightFences_;
    std::array<VkCommandBuffer, 2> commandBuffers_;
    
    std::vector<std::unique_ptr<Renderable>> renderables_;
};
