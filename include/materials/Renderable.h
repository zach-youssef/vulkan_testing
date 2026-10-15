#pragma once

#include "core/VkTypes.h"
#include "descriptors/Descriptor.h"
#include "resources/Buffer.h"
#include "core/VulkanContext.h"
#include "core/VkUtil.h"
#include "materials/AttachmentFormats.h"

#include <optional>

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


class Material {
public:
    virtual ~Material() = default;
    
    // Called by the render graph once per frame before any node records, e.g. to write uniform
    // buffers. surfaceExtent is {0, 0} without a surface.
    virtual void update(uint32_t frameIndex, VkExtent2D surfaceExtent) = 0;

    // Binds the pipeline and the frame's descriptor set for drawing or dispatching
    void bind(VkCommandBuffer commandBuffer, uint32_t frameIndex) {
        vkCmdBindPipeline(commandBuffer, bindPoint(), getPipeline());
        vkCmdBindDescriptorSets(commandBuffer, bindPoint(), getPipelineLayout(),
                                0, 1, getDescriptorSet(frameIndex), 0, nullptr);
    }

    virtual VkPipelineBindPoint bindPoint() {
        return VK_PIPELINE_BIND_POINT_GRAPHICS;
    }

    // The formats a graphics material's pipeline renders to; empty for compute materials
    virtual std::optional<AttachmentFormats> attachmentFormats() {
        return std::nullopt;
    }
    
    // The frame's descriptor set. Each frame's set is written the first time it is requested,
    // including frames added when the number of frames in flight grows.
    VkDescriptorSet* getDescriptorSet(uint32_t index) {
        auto& frameSet = descriptorSets_[index];
        if (!frameSet.populated) {
            populateDescriptorSet(index);
        }
        return &frameSet.set;
    }
    
    virtual VkPipeline getPipeline() {
        return **pipeline_;
    }
    
    VkPipelineLayout getPipelineLayout() {
        return **pipelineLayout_;
    }

protected:
    Material(VulkanContext& context,
             std::vector<Handle<Descriptor>> descriptors)
    : context_(&context), descriptors_(descriptors) {
        createDescriptorSetLayout();
        descriptorSets_ = PerFrame<FrameDescriptorSet>(context, [this](uint32_t) {
            return createDescriptorSet();
        });
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
        auto& frameSet = descriptorSets_[frameIndex];
        std::vector<VkWriteDescriptorSet> descriptorWrites;
        // Referenced by descriptorWrites, so sized up front to keep their addresses stable
        std::vector<VkDescriptorBufferInfo> bufferInfos(descriptors_.size());
        std::vector<VkDescriptorImageInfo> imageInfos(descriptors_.size());
        
        for (uint idx = 0; idx < descriptors_.size(); ++idx) {
            auto bufferInfo = descriptors_.at(idx)->getBufferInfo(frameIndex);
            auto imageInfo = descriptors_.at(idx)->getImageInfo(frameIndex);
            const bool bound = (bufferInfo && bufferInfo->buffer != VK_NULL_HANDLE)
                               || (imageInfo && imageInfo->imageView != VK_NULL_HANDLE);
            if (!bound) {
                continue;
            }
            
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = frameSet.set;
            write.dstBinding = idx;
            write.dstArrayElement = 0;
            write.descriptorType = descriptors_.at(idx)->getType();
            write.descriptorCount = 1;
            if (bufferInfo) {
                bufferInfos[idx] = *bufferInfo;
                write.pBufferInfo = &bufferInfos[idx];
            }
            if (imageInfo) {
                imageInfos[idx] = *imageInfo;
                write.pImageInfo = &imageInfos[idx];
            }
            descriptorWrites.push_back(write);
        }
        
        vkUpdateDescriptorSets(device(), static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
        frameSet.populated = true;
    }
    
private:
    // A frame's descriptor set, allocated from a pool of its own so that frames can be added
    // and removed independently
    struct FrameDescriptorSet {
        std::unique_ptr<VulkanDescriptorPool> pool;
        VkDescriptorSet set = VK_NULL_HANDLE;
        bool populated = false;
    };

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
    
    FrameDescriptorSet createDescriptorSet() {
        FrameDescriptorSet frameSet;

        std::vector<VkDescriptorPoolSize> poolSizes{};
        poolSizes.resize(descriptors_.size());
        for(uint idx = 0; idx < descriptors_.size(); ++idx) {
            poolSizes[idx].type = descriptors_.at(idx)->getType();
            poolSizes[idx].descriptorCount = 1;
        }
        
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        poolInfo.maxSets = 1;
        VK_SUCCESS_OR_THROW(VulkanDescriptorPool::create(frameSet.pool, device(), poolInfo),
                            "Failed to create descriptor pool");

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = **frameSet.pool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = descriptorSetLayout_->get();
        VK_SUCCESS_OR_THROW(vkAllocateDescriptorSets(device(), &allocInfo, &frameSet.set),
                            "Failed to allocate descriptor sets.");
        return frameSet;
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
    // Destroyed before the layout. Sets are freed with their pools.
    PerFrame<FrameDescriptorSet> descriptorSets_;
};

class ComputeMaterial : public Material {
public:
    virtual glm::vec3 getDispatchDimensions() = 0;
    
    VkPipeline getPipeline() override {
        return **computePipeline_;
    }

    VkPipelineBindPoint bindPoint() override {
        return VK_PIPELINE_BIND_POINT_COMPUTE;
    }
protected:
    ComputeMaterial(VulkanContext& context,
                                std::vector<Handle<Descriptor>> descriptors,
                                const std::vector<char> & computeShaderCode)
    : Material(context, descriptors) {
        createComputePipeline(computeShaderCode);
    }
    
private:
    void createComputePipeline(const std::vector<char>& computeShaderCode) {
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = descriptorSetLayout_->get();
        VK_SUCCESS_OR_THROW(VulkanPipelineLayout::create(pipelineLayout_,
                                                         device(),
                                                         pipelineLayoutInfo),
                            "Failed to create compute pipeline layout");
        
        auto computeShaderModule = createShaderModule(computeShaderCode);
        
        VkPipelineShaderStageCreateInfo computeShaderStageInfo{};
        computeShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        computeShaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        computeShaderStageInfo.module = **computeShaderModule;
        computeShaderStageInfo.pName = "main";
        
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.layout = **pipelineLayout_;
        pipelineInfo.stage = computeShaderStageInfo;
        
        // The VkResult is intentionally left unchecked here (unlike other create calls)
        VulkanComputePipeline::create(computePipeline_, device(), pipelineInfo);
    }

protected:
    std::unique_ptr<VulkanComputePipeline> computePipeline_;
};

class Renderable {
public:
    virtual ~Renderable() = default;
    
    Material* getMaterial() {
        return material_.get();
    }

    Handle<Material> getMaterialHandle() {
        return material_;
    }
    
    virtual VkBuffer getVertexBuffer() = 0;
    virtual VkBuffer getIndexBuffer() = 0;
    virtual uint32_t getIndexCount() = 0;
    
protected:
    Renderable(Handle<Material> material)
    : material_(std::move(material)) {}
    
protected:
    Handle<Material> material_;
};

template<typename VertexData>
class MeshRenderable : public Renderable {
public:
    MeshRenderable(const std::vector<VertexData>& vertexData,
                   const std::vector<uint16_t>& indexData,
                   Handle<Material> material,
                   VulkanContext& context)
    : Renderable(std::move(material)), indexCount_(static_cast<uint32_t>(indexData.size())) {
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
