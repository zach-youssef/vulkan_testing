#pragma once

#include "VkTypes.h"

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
    
    // Should get called at end of child constructors
    // TODO: Find a better pattern
    virtual void populateDescriptorSet(uint32_t frameIndex) = 0;
    
    virtual void update(uint32_t currentImage, VkExtent2D swapChainExtent) = 0;
    
    VkDescriptorSet* getDescriptorSet(uint32_t index) {
        return &descriptorSets_.at(index);
        //return &descriptorSets_[index];
    }
    
    VkPipeline getPipeline() {
        return **pipeline_;
    }
    
    VkPipelineLayout getPipelineLayout() {
        return **pipelineLayout_;
    }

protected:
    Material(VkDevice device, VkPhysicalDevice physicalDevice) : device_(device), physicalDevice_(physicalDevice){}
    
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

protected:
    VkDevice device_;
    VkPhysicalDevice physicalDevice_;

    std::unique_ptr<VulkanPipelineLayout> pipelineLayout_;
    std::unique_ptr<VulkanGraphicsPipeline> pipeline_;

    std::unique_ptr<VulkanDescriptorSetLayout> descriptorSetLayout_;
    std::unique_ptr<VulkanDescriptorPool> descriptorPool_;
    // TODO: max frames in flight refactor
    std::array<VkDescriptorSet, 2> descriptorSets_;
};

class Renderable {
public:
    virtual ~Renderable() = default;
    
    Material* getMaterial() {
        return material_.get();
    }
    
    void update(uint32_t frameIndex, VkExtent2D swapchainExtent) {
        material_->update(frameIndex, swapchainExtent);
    }
    
    virtual VkBuffer getVertexBuffer() = 0;
    virtual VkBuffer getIndexBuffer() = 0;
    virtual uint32_t getIndexCount() = 0;
    
protected:
    Renderable(std::unique_ptr<Material>&& material) {
        material_ = std::move(material);
    }
    
protected:
    std::unique_ptr<Material> material_;
};

template<typename VertexData>
class MeshRenderable : public Renderable {
public:
    MeshRenderable(const std::vector<VertexData>& vertexData,
                   const std::vector<uint16_t>& indexData,
                   std::unique_ptr<Material>&& material,
                   VkDevice device,
                   VkPhysicalDevice physicalDevice,
                   VkQueue graphicsQueue,
                   VkCommandPool commandPool)
    : Renderable(std::move(material)), indexCount_(static_cast<uint32_t>(indexData.size())) {
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
