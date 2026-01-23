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
    // Should get called at end of child constructors
    // TODO: Find a better pattern
    virtual void populateDescriptorSet(uint32_t frameIndex) = 0;
    
    // Should bind pipeline, bind descriptor sets
    virtual void recordCommandBuffer(uint32_t frameIndex, VkCommandBuffer commandBuffer) = 0;
    
    virtual void update(uint32_t currentImage, VkExtent2D swapChainExtent) = 0;
    
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
    
    std::unique_ptr<VulkanDescriptorSetLayout> descriptorSetLayout_;
    std::unique_ptr<VulkanDescriptorPool> descriptorPool_;
    // TODO: max frames in flight refactor
    std::array<VkDescriptorSet, 2> descriptorSets_;
    
    std::unique_ptr<VulkanPipelineLayout> pipelineLayout_;
    std::unique_ptr<VulkanGraphicsPipeline> pipeline_;
};

class Renderable {
public:
    void submit(uint32_t imageIndex,
                VkCommandBuffer commandBuffer,
                VkQueue graphicsQueue,
                VkSwapchainKHR swapChain,
                VkFence inFlightFence,
                std::vector<VkSemaphore> waitSemaphores = {},
                std::vector<VkSemaphore> signalSemaphores = {}) {
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        
        // (it should wait for the swapchain image to be available before writing out to it)
        VkPipelineStageFlags waitStages[] {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
        submitInfo.pWaitSemaphores = waitSemaphores.data();
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        submitInfo.signalSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size());
        submitInfo.pSignalSemaphores = signalSemaphores.data();
        
        VK_SUCCESS_OR_THROW(vkQueueSubmit(graphicsQueue, 1, &submitInfo, inFlightFence),
                            "Failed to submit draw command buffer.");
    }
    
    Material* getMaterial() {
        return material_.get();
    }
    
    void update(uint32_t frameIndex, VkExtent2D swapchainExtent) {
        material_->update(frameIndex, swapchainExtent);
    }
    
    virtual void recordCommandBuffer(VkCommandBuffer commandBuffer,
                             uint32_t frameIndex,
                             VkExtent2D swapChainExtent) = 0;
    
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

    void recordCommandBuffer(VkCommandBuffer commandBuffer,
                             uint32_t frameIndex,
                             VkExtent2D swapChainExtent) {
        // Issue material commmands
        // Should bind pipeline, bind descriptor sets
        material_->recordCommandBuffer(frameIndex, commandBuffer);
        
        // Bind vertex & index buffers
        VkBuffer vertexBuffers[] {vertexBuffer_->getBuffer()};
        VkDeviceSize offsets[] {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(commandBuffer, indexBuffer_->getBuffer(), 0, VK_INDEX_TYPE_UINT16);
        
        // Set Viewport & Scissor
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(swapChainExtent.width);
        viewport.height = static_cast<float>(swapChainExtent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = swapChainExtent;
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        
        // Issue draw command
        vkCmdDrawIndexed(commandBuffer,
                         indexCount_,
                         1 /*num instances*/,
                         0 /*offset into buffer*/,
                         0 /*offset to add to indices*/,
                         0 /* instancing offset*/);
        
        VK_SUCCESS_OR_THROW(vkEndCommandBuffer(commandBuffer),
                            "Failed to reccord command buffer.");
    }
    
private:
    std::unique_ptr<Buffer<VertexData>> vertexBuffer_;
    std::unique_ptr<Buffer<uint16_t>> indexBuffer_;
    uint32_t indexCount_;
};
