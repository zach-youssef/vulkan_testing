#pragma once

#include "Ubo.h"
#include "Renderable.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

class BasicMaterial : public Material {
public:
    BasicMaterial(VkDevice device,
                  VkPhysicalDevice physicalDevice,
                  VkExtent2D swapchainExtent,
                  VkRenderPass renderPass,
                  // TODO max frames refactor
                  std::array<VkImageView, 2> textureImages,
                  VkSampler textureSampler,
                  const std::vector<char>& vertSpirv,
                  const std::vector<char>& fragSpirv,
                  VkVertexInputBindingDescription bindingDescription,
                  // TODO make that 2 customizable
                  std::array<VkVertexInputAttributeDescription, 2> attributeDescriptions)
    : Material(device, physicalDevice),
    textureImages_(textureImages),
    textureSampler_(textureSampler) {
        createDescriptorSetLayout();
        createDescriptorPool();
        createGraphicsPipeline(vertSpirv, fragSpirv, swapchainExtent, renderPass, bindingDescription, attributeDescriptions);
        createDescriptorSets();
        for (uint32_t frameIndex = 0; frameIndex < 2; ++frameIndex) {
            createUniformBuffer(frameIndex);
            populateDescriptorSet(frameIndex);
        }
    }
    
    void populateDescriptorSet(uint32_t frameIndex) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = uniformBuffers_[frameIndex]->getBuffer();
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(UniformBufferObject);
        
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = textureImages_[frameIndex];
        imageInfo.sampler = textureSampler_;
        
        VkWriteDescriptorSet bufferDescriptorWrite{};
        bufferDescriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        bufferDescriptorWrite.dstSet = descriptorSets_[frameIndex];
        bufferDescriptorWrite.dstBinding = 0;
        bufferDescriptorWrite.dstArrayElement = 0;
        bufferDescriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bufferDescriptorWrite.descriptorCount = 1;
        bufferDescriptorWrite.pBufferInfo = &bufferInfo;
        bufferDescriptorWrite.pImageInfo = nullptr; // Optional
        bufferDescriptorWrite.pTexelBufferView = nullptr; // Optional
        
        VkWriteDescriptorSet imageDescriptorWrite{};
        imageDescriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        imageDescriptorWrite.dstSet = descriptorSets_[frameIndex];
        imageDescriptorWrite.dstBinding = 1;
        imageDescriptorWrite.dstArrayElement = 0;
        imageDescriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        imageDescriptorWrite.descriptorCount = 1;
        imageDescriptorWrite.pImageInfo = &imageInfo;
        
        std::array<VkWriteDescriptorSet, 2> descriptorWrites{
            bufferDescriptorWrite, imageDescriptorWrite
        };
        

        vkUpdateDescriptorSets(device_, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
    }
    
    void update(uint32_t currentImage, VkExtent2D swapChainExtent) {
        auto model = glm::identity<glm::mat4>();
        auto view = glm::identity<glm::mat4>();
        auto projection = glm::ortho(-1.0, 1.0, -1.0, 1.0);
        
        // GLM was originally designed for OpenGL where the Y coordinate of the clip coordinates is inverted
        projection[1][1] *= -1;

        auto ubo = UniformBufferObject::fromModelViewProjection(model, view, projection);
        
        memcpy(mappedUniformBuffers_[currentImage].get(), &ubo, sizeof(ubo));
    }
private:
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
        
        VK_SUCCESS_OR_THROW(VulkanDescriptorSetLayout::create(descriptorSetLayout_, device_, layoutInfo),
                            "Failed to create descriptor set layout.");
    }
    
    // TODO: There's probably a way to abstract away the idea of creating a pool from a layout
    void createDescriptorPool() {
        std::array<VkDescriptorPoolSize, 2> poolSize{};
        
        poolSize[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSize[0].descriptorCount = static_cast<uint32_t>(2);
        
        poolSize[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize[1].descriptorCount = static_cast<uint32_t>(2);
        
        
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSize.size());
        poolInfo.pPoolSizes = poolSize.data();
        poolInfo.maxSets = static_cast<uint32_t>(2);
        
        VK_SUCCESS_OR_THROW(VulkanDescriptorPool::create(descriptorPool_, device_, poolInfo),
                            "Failed to create descriptor pool");
    }
    
    void createGraphicsPipeline(const std::vector<char>& vertShaderCode,
                                const std::vector<char>& fragShaderCode,
                                VkExtent2D swapChainExtent,
                                VkRenderPass renderPass,
                                VkVertexInputBindingDescription bindingDescription,
                                //TODO make 2 configurable
                                std::array<VkVertexInputAttributeDescription, 2> attributeDescriptions) {
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
        
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputInfo.vertexBindingDescriptionCount = 1;
        vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
        
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
        viewport.width = static_cast<float>(swapChainExtent.width);
        viewport.height = static_cast<float>(swapChainExtent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        
        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = swapChainExtent;
        
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
        
        VK_SUCCESS_OR_THROW(VulkanPipelineLayout::create(pipelineLayout_, device_, pipelineLayoutInfo),
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
        pipelineInfo.renderPass = renderPass;
        pipelineInfo.subpass = 0;

        pipelineInfo.basePipelineHandle = VK_NULL_HANDLE; // Optional
        pipelineInfo.basePipelineIndex = -1; // Optional
        
        VK_SUCCESS_OR_THROW(VulkanGraphicsPipeline::create(pipeline_, device_, pipelineInfo),
                            "Failed to create graphics pipeline");
    }
    
    // TODO: this can probably be moved to the abstract material
    void createDescriptorSets() {
        std::vector<VkDescriptorSetLayout> layouts(2, **descriptorSetLayout_);
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = **descriptorPool_;
        allocInfo.descriptorSetCount = static_cast<uint32_t>(2);
        allocInfo.pSetLayouts = layouts.data();
        
        VK_SUCCESS_OR_THROW(vkAllocateDescriptorSets(device_, &allocInfo, descriptorSets_.data()),
                            "Failed to allocate descriptor sets.");
    }
    
    void createUniformBuffer(uint32_t frameIndex) {
        VK_SUCCESS_OR_THROW(Buffer<UniformBufferObject>::create(uniformBuffers_[frameIndex],
                                                                1,
                                                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                                                                device_,
                                                                physicalDevice_),
                            "Failed to create uniform buffer.");
        mappedUniformBuffers_[frameIndex] = uniformBuffers_[frameIndex]->getPersistentMapping(0, sizeof(UniformBufferObject));
    }
private:
    // TODO: max frames refactor
    std::array<std::unique_ptr<Buffer<UniformBufferObject>>, 2> uniformBuffers_;
    std::array<std::unique_ptr<UniformBufferObject, std::function<void(UniformBufferObject*)>>, 2> mappedUniformBuffers_;
    std::array<VkImageView, 2> textureImages_;
    VkSampler textureSampler_;
};
