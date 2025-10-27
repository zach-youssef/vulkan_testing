#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "VkUtil.h"
#include "DeviceSelection.h"
#include "QueueFamilyIndices.h"
#include "SwapChainSupportDetails.h"
#include "Frame.h"
#include "VkTypes.h"
#include "Buffer.h"

#include "FileUtil.h"

#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <set>

#include <glm/glm.hpp>

const uint32_t WINDOW_WIDTH = 800;
const uint32_t WINDOW_HEIGHT = 600;

const std::vector<const char*> validationLayers = {
    "VK_LAYER_KHRONOS_validation"
};

const int MAX_FRAMES_IN_FLIGHT = 2;

#ifdef NDEBUG
    const bool enableValidationLayers = false;
#else
    [[maybe_unused]]
    const bool enableValidationLayers = true;
#endif

struct Vertex {
    glm::vec2 pos;
    glm::vec3 color;
    
    static VkVertexInputBindingDescription getBindingDescription() {
        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;
        bindingDescription.stride = sizeof(Vertex);
        // Move to next data entry after each vertex (alternative is after each instance)
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return bindingDescription;
    }
    
    static std::array<VkVertexInputAttributeDescription, 2> getAttributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 2> attributeDescriptions{};
        
        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[0].offset = offsetof(Vertex, pos);
        
        attributeDescriptions[1].binding = 0;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[1].offset = offsetof(Vertex, color);

        return attributeDescriptions;
    }
};

const std::vector<Vertex> vertexData = {
    {{-0.5f, -0.5f},{1.0f, 0.0f, 0.0f}},
    {{0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},
    {{0.5f, 0.5f},  {0.0f, 0.0f, 1.0f}},
    {{-0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}}
};

const std::vector<uint16_t> indexData = {
    0, 1, 2,
    2, 3, 0
};

class HelloTriangleApplication {
public:
    void run() {
        initWindow();
        initVulkan();
        mainLoop();
    }

private:
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
    
    bool checkValidationLayerSupport() {
        std::vector<VkLayerProperties> availableLayers = readVkVector<VkLayerProperties>(vkEnumerateInstanceLayerProperties);
        
        for (const auto layerName : validationLayers) {
            bool layerFound = false;
            for (const auto& layerProperties : availableLayers) {
                if (strcmp(layerName, layerProperties.layerName) == 0) {
                    layerFound = true;
                }
            }
            if (!layerFound) {
                return false;
            }
        }
        return true;
    }
    
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
        appInfo.pApplicationName = "Hello Triangle";
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
    
    void createSurface() {
        VK_SUCCESS_OR_THROW(VulkanSurface::create(surface_, **instance_, window_.get()),
                            "Failed to create window surface");
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
            VkImageViewCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            createInfo.image = swapChainImages_[i];
            createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            createInfo.format = swapChainImageFormat_;
            // here is where you could make channels constant or remapped
            createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            // Interesting part, specifies which part of image to be accessed
            createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            createInfo.subresourceRange.baseMipLevel = 0;
            createInfo.subresourceRange.levelCount = 1;
            createInfo.subresourceRange.baseArrayLayer = 0;
            createInfo.subresourceRange.layerCount = 1;
            VK_SUCCESS_OR_THROW(VulkanImageView::create(swapChainImageViews_[i], **device_, createInfo),
                                "Failed to create image view for swapchain");
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
    
    void initFrames() {
        // createDescriptorSets
        std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, **descriptorSetLayout_);
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = **descriptorPool_;
        allocInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
        allocInfo.pSetLayouts = layouts.data();
        
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> descriptorSets;
        
        VK_SUCCESS_OR_THROW(vkAllocateDescriptorSets(**device_, &allocInfo, descriptorSets.data()),
                            "Failed to allocate descriptor sets.");
        
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            frames_[i] = std::make_unique<Frame>(**device_,
                                                 **commandPool_,
                                                 physicalDevice_,
                                                 descriptorSets[i]);
        }
    }
    
    std::optional<uint32_t> findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties){
        VkPhysicalDeviceMemoryProperties memoryProperties;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties);
        
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            if (typeFilter & (1 << i)
                && (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        
        return {};
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
                                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | usage,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                   **device_,
                                                   physicalDevice_),
                            "Failed to create gpu buffer");
        
        copyBuffer(stagingBuffer->getBuffer(), buffer->getBuffer(), bufferSize);
    }
    
    void createVertexBuffer() {
        createAndInitializeBuffer<Vertex>(vertexBuffer_, vertexData, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    }
    
    void createIndexBuffer() {
        createAndInitializeBuffer<uint16_t>(indexBuffer_, indexData, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    }

    void copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size) {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool = **commandPool_;
        allocInfo.commandBufferCount = 1;
        
        VkCommandBuffer commandBuffer;
        vkAllocateCommandBuffers(**device_, &allocInfo, &commandBuffer);
        
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; // Just used once to transfer vertex data
        
        vkBeginCommandBuffer(commandBuffer, &beginInfo);
        
        VkBufferCopy copyRegion{};
        copyRegion.srcOffset = 0;
        copyRegion.dstOffset = 0;
        copyRegion.size = size;
        vkCmdCopyBuffer(commandBuffer, src, dst, 1 /*regionCount*/, &copyRegion);
        
        vkEndCommandBuffer(commandBuffer);
        
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        
        vkQueueSubmit(graphicsQueue_, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(graphicsQueue_); // Could replace null handle above with a fence instead
        
        vkFreeCommandBuffers(**device_, **commandPool_, 1, &commandBuffer);
    }
    
    void createDescriptorSetLayout() {
        VkDescriptorSetLayoutBinding uboLayoutBinding{};
        uboLayoutBinding.binding = 0;
        uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uboLayoutBinding.descriptorCount = 0;
        uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        uboLayoutBinding.pImmutableSamplers = nullptr; // Optional, only relevant for image sampling
        
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &uboLayoutBinding;
        
        VK_SUCCESS_OR_THROW(VulkanDescriptorSetLayout::create(descriptorSetLayout_, **device_, layoutInfo),
                            "Failed to create descriptor set layout.");
    }
    
    void createDescriptorPool() {
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSize.descriptorCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
        
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        poolInfo.maxSets = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
        
        VK_SUCCESS_OR_THROW(VulkanDescriptorPool::create(descriptorPool_, **device_, poolInfo),
                            "Failed to create descriptor pool");
    }

    void initVulkan() {
        createInstance();
        createSurface();
        choosePhysicalDevice();
        createLogicalDevice();
        createSwapChain();
        createSwapChainImageViews();
        createRenderPass();
        createDescriptorSetLayout();
        createGraphicsPipeline();
        createFramebuffers();
        createCommandPool();
        createDescriptorPool();
        createVertexBuffer();
        createIndexBuffer();
        initFrames();
    }
    
    static void onFrameBufferResizeCallback(GLFWwindow* window, int /*width*/, int /*height*/) {
        auto app = reinterpret_cast<HelloTriangleApplication*>(glfwGetWindowUserPointer(window));
        app->frameBufferResized_ = true;
    }
    
    void initWindow() {
        glfwInit();
        
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        
        window_ = std::unique_ptr<GLFWwindow, std::function<void(GLFWwindow*)>>(glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Vulkan", nullptr, nullptr), [](GLFWwindow* w){
            glfwDestroyWindow(w);
            glfwTerminate();
        });
        glfwSetWindowUserPointer(window_.get(), this);
        glfwSetFramebufferSizeCallback(window_.get(), onFrameBufferResizeCallback);
    }
    
    void drawFrame() {
        auto& currentFrame = frames_[currentFrameIndex_];
        // Wait for previous frame to complete
        currentFrame->waitForFence();
        
        // Acquire index of next image in swapchain
        bool shouldRecreateSwapChain;
        uint32_t imageIndex = currentFrame->aquireImageIndex(**swapChain_, shouldRecreateSwapChain);
        
        if (shouldRecreateSwapChain || frameBufferResized_) {
            frameBufferResized_ = false;
            recreateSwapChain();
            return;
        }
        
        // Only reset fence here now that we know we will be doing work
        currentFrame->resetFence();

        // Record the command buffer
        auto commandBuffer = currentFrame->getCommandBuffer();
        vkResetCommandBuffer(commandBuffer, 0);
        recordCommandBuffer(commandBuffer, imageIndex);
        
        // Submit command buffer
        currentFrame->submit(imageIndex, graphicsQueue_, presentQueue_, **swapChain_);
        
        // Increment frame index
        currentFrameIndex_ = (this->currentFrameIndex_ + 1) % MAX_FRAMES_IN_FLIGHT;
    }
    
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

    void mainLoop() {
        while(!glfwWindowShouldClose(window_.get())) {
            glfwPollEvents();
            drawFrame();
        }
        vkDeviceWaitIdle(**device_);
    }
private:
    std::unique_ptr<GLFWwindow, std::function<void(GLFWwindow*)>> window_;
    std::unique_ptr<VulkanInstance> instance_;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    std::unique_ptr<VulkanDevice> device_;
    VkQueue graphicsQueue_;
    VkQueue presentQueue_;
    std::unique_ptr<VulkanSurface> surface_;
    std::unique_ptr<VulkanSwapchain> swapChain_;
    
    std::vector<VkImage> swapChainImages_;
    VkFormat swapChainImageFormat_;
    VkExtent2D swapChainExtent_;
    
    std::vector<std::unique_ptr<VulkanImageView>> swapChainImageViews_;
    
    std::unique_ptr<VulkanRenderPass> renderPass_;
    
    std::unique_ptr<VulkanDescriptorSetLayout> descriptorSetLayout_;
    std::unique_ptr<VulkanPipelineLayout> pipelineLayout_;
    
    std::unique_ptr<VulkanGraphicsPipeline> pipeline_;
    
    std::vector<std::unique_ptr<VulkanFramebuffer>> swapChainFramebuffers_;
    
    std::unique_ptr<VulkanCommandPool> commandPool_;
    
    std::unique_ptr<VulkanDescriptorPool> descriptorPool_;

    std::array<std::unique_ptr<Frame>, MAX_FRAMES_IN_FLIGHT> frames_;
    uint32_t currentFrameIndex_ = 0;
    
    bool frameBufferResized_ = false;
    
    std::unique_ptr<Buffer<Vertex>> vertexBuffer_;
    std::unique_ptr<Buffer<uint16_t>> indexBuffer_;
};

int main() {
    HelloTriangleApplication app;

    try {
        app.run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
