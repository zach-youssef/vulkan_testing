#pragma once

#include "VkTypes.h"
#include "Descriptor.h"
#include "Buffer.h"
#include "VkUtil.h"

#include <glm/glm.hpp>

/*
 Overview of Structure:
 
 Renderable:
    - Vertex Buffer
    - Index Buffer
    - Material
 
 Material:
    - Pipeline State / Shader
    - Descriptor Set Layout / Bindings
    - Buffers / Samplers for descriptor set
 */


template<uint MAX_FRAMES>
class Material {
public:
    virtual ~Material() = default;
    
    virtual void update(uint32_t currentImage, VkExtent2D swapChainExtent) = 0;
    
    VkDescriptorSet* getDescriptorSet(uint32_t index) {
        return &descriptorSets_.at(index);
    }
    
    virtual VkPipeline getPipeline() {
        return **pipeline_;
    }
    
    VkPipelineLayout getPipelineLayout() {
        return **pipelineLayout_;
    }

protected:
    Material<MAX_FRAMES>(VkDevice device,
             VkPhysicalDevice physicalDevice,
             std::vector<std::shared_ptr<Descriptor>> descriptors)
    : device_(device), physicalDevice_(physicalDevice), descriptors_(descriptors) {
        createDescriptorSetLayout();
        createDescriptorPool();
        createDescriptorSets();
        for (uint32_t frameIndex = 0; frameIndex < MAX_FRAMES; ++frameIndex) {
            populateDescriptorSet(frameIndex);
        }
    }
    
    std::unique_ptr<VulkanShaderModule> createShaderModule(const std::vector<char>& code) {
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
        
        std::unique_ptr<VulkanShaderModule> shaderModule;
        
        
        VK_SUCCESS_OR_THROW(VulkanShaderModule::create(shaderModule, device_, createInfo),
                            "Failed to create shader module.");
        
        return shaderModule;
    }
    
    void populateDescriptorSet(uint32_t frameIndex) {
        std::vector<VkWriteDescriptorSet> descriptorWrites;
        descriptorWrites.resize(descriptors_.size());
        
        for (uint idx = 0; idx < descriptors_.size(); ++idx) {
            descriptorWrites[idx].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[idx].dstSet = descriptorSets_.at(frameIndex);
            descriptorWrites[idx].dstBinding = idx;
            descriptorWrites[idx].dstArrayElement = 0;
            descriptorWrites[idx].descriptorType = descriptors_.at(idx)->getType();
            descriptorWrites[idx].descriptorCount = 1;
            descriptorWrites[idx].pBufferInfo = descriptors_.at(idx)->getBufferInfo(frameIndex);
            descriptorWrites[idx].pImageInfo = descriptors_.at(idx)->getImageInfo(frameIndex);
        }
        
        vkUpdateDescriptorSets(device_, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
    }
    
private:
    void createDescriptorSetLayout() {
        std::vector<VkDescriptorSetLayoutBinding> layoutBindings;
        layoutBindings.resize(descriptors_.size());
        
        for (uint idx = 0; idx < descriptors_.size(); ++idx) {
            layoutBindings[idx].binding = idx;
            layoutBindings[idx].descriptorType = descriptors_.at(idx)->getType();
            layoutBindings[idx].descriptorCount = 1;
            layoutBindings[idx].stageFlags = descriptors_.at(idx)->getStageFlags();
            layoutBindings[idx].pImmutableSamplers = nullptr; // Optional
        }
        
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<uint32_t>(layoutBindings.size());
        layoutInfo.pBindings = layoutBindings.data();
        VK_SUCCESS_OR_THROW(VulkanDescriptorSetLayout::create(descriptorSetLayout_, device_, layoutInfo),
                            "Failed to create descriptor set layout.");
    }
    
    void createDescriptorPool() {
        std::vector<VkDescriptorPoolSize> poolSizes{};
        poolSizes.resize(descriptors_.size());
        
        for(uint idx = 0; idx < descriptors_.size(); ++idx) {
            poolSizes[idx].type = descriptors_.at(idx)->getType();
            poolSizes[idx].descriptorCount = static_cast<uint32_t>(MAX_FRAMES);
        }
        
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        poolInfo.maxSets = static_cast<uint32_t>(MAX_FRAMES);
        
        VK_SUCCESS_OR_THROW(VulkanDescriptorPool::create(descriptorPool_, device_, poolInfo),
                            "Failed to create descriptor pool");
    }
    
    void createDescriptorSets() {
        std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES, **descriptorSetLayout_);
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = **descriptorPool_;
        allocInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES);
        allocInfo.pSetLayouts = layouts.data();
        
        VK_SUCCESS_OR_THROW(vkAllocateDescriptorSets(device_, &allocInfo, descriptorSets_.data()),
                            "Failed to allocate descriptor sets.");
    }

protected:
    VkDevice device_;
    VkPhysicalDevice physicalDevice_;
    std::vector<std::shared_ptr<Descriptor>> descriptors_;

    std::unique_ptr<VulkanPipelineLayout> pipelineLayout_;
    std::unique_ptr<VulkanGraphicsPipeline> pipeline_;

    std::unique_ptr<VulkanDescriptorSetLayout> descriptorSetLayout_;
    std::unique_ptr<VulkanDescriptorPool> descriptorPool_;
    std::array<VkDescriptorSet, MAX_FRAMES> descriptorSets_;
};

template<uint MAX_FRAMES>
class ComputeMaterial : public Material<MAX_FRAMES> {
public:
    virtual glm::vec3 getDispatchDimensions() = 0;
    
    VkPipeline getPipeline() override {
        return **computePipeline_;
    }
protected:
    ComputeMaterial<MAX_FRAMES>(VkDevice device,
                                VkPhysicalDevice physicalDevice,
                                std::vector<std::shared_ptr<Descriptor>> descriptors,
                                const std::vector<char> & computeShaderCode)
    : Material<MAX_FRAMES>(device, physicalDevice, descriptors) {
        createComputePipeline(computeShaderCode);
    }
    
private:
    void createComputePipeline(const std::vector<char>& computeShaderCode) {
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = Material<MAX_FRAMES>::descriptorSetLayout_->get();
        VK_SUCCESS_OR_THROW(VulkanPipelineLayout::create(Material<MAX_FRAMES>::pipelineLayout_,
                                                         Material<MAX_FRAMES>::device_,
                                                         pipelineLayoutInfo),
                            "Failed to create compute pipeline layout");
        
        auto computeShaderModule = Material<MAX_FRAMES>::createShaderModule(computeShaderCode);
        
        VkPipelineShaderStageCreateInfo computeShaderStageInfo{};
        computeShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        computeShaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        computeShaderStageInfo.module = **computeShaderModule;
        computeShaderStageInfo.pName = "main";
        
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.layout = **Material<MAX_FRAMES>::pipelineLayout_;
        pipelineInfo.stage = computeShaderStageInfo;
        
        VulkanComputePipeline::create(computePipeline_, Material<MAX_FRAMES>::device_, pipelineInfo);//,
    }

protected:
    std::unique_ptr<VulkanComputePipeline> computePipeline_;
};

template<uint MAX_FRAMES>
class Renderable {
public:
    virtual ~Renderable<MAX_FRAMES>() = default;
    
    Material<MAX_FRAMES>* getMaterial() {
        return material_.get();
    }
    
    void update(uint32_t frameIndex, VkExtent2D swapchainExtent) {
        material_->update(frameIndex, swapchainExtent);
    }
    
    virtual VkBuffer getVertexBuffer() = 0;
    virtual VkBuffer getIndexBuffer() = 0;
    virtual uint32_t getIndexCount() = 0;
    
protected:
    Renderable<MAX_FRAMES>(std::unique_ptr<Material<MAX_FRAMES>>&& material) {
        material_ = std::move(material);
    }
    
protected:
    std::unique_ptr<Material<MAX_FRAMES>> material_;
};

template<typename VertexData, uint MAX_FRAMES>
class MeshRenderable : public Renderable<MAX_FRAMES> {
public:
    MeshRenderable(const std::vector<VertexData>& vertexData,
                   const std::vector<uint16_t>& indexData,
                   std::unique_ptr<Material<MAX_FRAMES>>&& material,
                   VkDevice device,
                   VkPhysicalDevice physicalDevice,
                   VkQueue graphicsQueue,
                   VkCommandPool commandPool)
    : Renderable<MAX_FRAMES>(std::move(material)), indexCount_(static_cast<uint32_t>(indexData.size())) {
        Buffer<VertexData>::createAndInitialize(vertexBuffer_,
                                                vertexData,
                                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                                device,
                                                physicalDevice,
                                                graphicsQueue,
                                                commandPool);
        Buffer<uint16_t>::createAndInitialize(indexBuffer_,
                                              indexData,
                                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                              device,
                                              physicalDevice,
                                              graphicsQueue,
                                              commandPool);
    }

    VkBuffer getVertexBuffer() {
        return vertexBuffer_->getBuffer();
    }
    
    VkBuffer getIndexBuffer() {
        return indexBuffer_->getBuffer();
    }
    
    uint32_t getIndexCount() {
        return indexCount_;
    }

private:
    std::unique_ptr<Buffer<VertexData>> vertexBuffer_;
    std::unique_ptr<Buffer<uint16_t>> indexBuffer_;
    uint32_t indexCount_;
};
