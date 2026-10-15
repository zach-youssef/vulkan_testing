#pragma once

#include "VkTypes.h"
#include "Descriptor.h"
#include "Buffer.h"
#include "VulkanContext.h"
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
    Material<MAX_FRAMES>(VulkanContext& context,
             std::vector<Handle<Descriptor>> descriptors)
    : context_(&context), descriptors_(descriptors) {
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
        
        
        VK_SUCCESS_OR_THROW(VulkanShaderModule::create(shaderModule, device(), createInfo),
                            "Failed to create shader module.");
        
        return shaderModule;
    }
    
    // Writes the descriptors' current resources into the frame's descriptor set. Descriptors whose
    // resources are not bound yet (VK_NULL_HANDLE) are skipped; call this again once they are.
    void populateDescriptorSet(uint32_t frameIndex) {
        std::vector<VkWriteDescriptorSet> descriptorWrites;
        descriptorWrites.reserve(descriptors_.size());
        
        for (uint idx = 0; idx < descriptors_.size(); ++idx) {
            auto* bufferInfo = descriptors_.at(idx)->getBufferInfo(frameIndex);
            auto* imageInfo = descriptors_.at(idx)->getImageInfo(frameIndex);
            const bool bound = (bufferInfo && bufferInfo->buffer != VK_NULL_HANDLE)
                               || (imageInfo && imageInfo->imageView != VK_NULL_HANDLE);
            if (!bound) {
                continue;
            }
            
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = descriptorSets_.at(frameIndex);
            write.dstBinding = idx;
            write.dstArrayElement = 0;
            write.descriptorType = descriptors_.at(idx)->getType();
            write.descriptorCount = 1;
            write.pBufferInfo = bufferInfo;
            write.pImageInfo = imageInfo;
            descriptorWrites.push_back(write);
        }
        
        vkUpdateDescriptorSets(device(), static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
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
        VK_SUCCESS_OR_THROW(VulkanDescriptorSetLayout::create(descriptorSetLayout_, device(), layoutInfo),
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
        
        VK_SUCCESS_OR_THROW(VulkanDescriptorPool::create(descriptorPool_, device(), poolInfo),
                            "Failed to create descriptor pool");
    }
    
    void createDescriptorSets() {
        std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES, **descriptorSetLayout_);
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = **descriptorPool_;
        allocInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES);
        allocInfo.pSetLayouts = layouts.data();
        
        VK_SUCCESS_OR_THROW(vkAllocateDescriptorSets(device(), &allocInfo, descriptorSets_.data()),
                            "Failed to allocate descriptor sets.");
    }

protected:
    VkDevice device() {
        return context_->device();
    }
    
    VulkanContext* context_;
    std::vector<Handle<Descriptor>> descriptors_;

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
    ComputeMaterial<MAX_FRAMES>(VulkanContext& context,
                                std::vector<Handle<Descriptor>> descriptors,
                                const std::vector<char> & computeShaderCode)
    : Material<MAX_FRAMES>(context, descriptors) {
        createComputePipeline(computeShaderCode);
    }
    
private:
    void createComputePipeline(const std::vector<char>& computeShaderCode) {
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = Material<MAX_FRAMES>::descriptorSetLayout_->get();
        VK_SUCCESS_OR_THROW(VulkanPipelineLayout::create(Material<MAX_FRAMES>::pipelineLayout_,
                                                         Material<MAX_FRAMES>::device(),
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
        
        // The VkResult is intentionally left unchecked here (unlike other create calls)
        VulkanComputePipeline::create(computePipeline_, Material<MAX_FRAMES>::device(), pipelineInfo);
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
    Renderable<MAX_FRAMES>(Handle<Material<MAX_FRAMES>> material)
    : material_(std::move(material)) {}
    
protected:
    Handle<Material<MAX_FRAMES>> material_;
};

template<typename VertexData, uint MAX_FRAMES>
class MeshRenderable : public Renderable<MAX_FRAMES> {
public:
    MeshRenderable(const std::vector<VertexData>& vertexData,
                   const std::vector<uint16_t>& indexData,
                   Handle<Material<MAX_FRAMES>> material,
                   VulkanContext& context)
    : Renderable<MAX_FRAMES>(std::move(material)), indexCount_(static_cast<uint32_t>(indexData.size())) {
        Buffer<VertexData>::createAndInitialize(vertexBuffer_, context, vertexData, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        Buffer<uint16_t>::createAndInitialize(indexBuffer_, context, indexData, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
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
    Handle<Buffer<VertexData>> vertexBuffer_;
    Handle<Buffer<uint16_t>> indexBuffer_;
    uint32_t indexCount_;
};
