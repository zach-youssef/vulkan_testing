#pragma once

#include "RenderGraph.h"
#include "Renderable.h"
#include "VkUtil.h"

template<uint MAX_FRAMES>
class ComputeNode : public RenderNode<MAX_FRAMES> {
public:
    ComputeNode<MAX_FRAMES>(std::unique_ptr<ComputeMaterial<MAX_FRAMES>>&& computePass,
                            VkDevice device,
                            VkQueue computeQueue,
                            std::array<VkCommandBuffer, MAX_FRAMES> commandBuffers)
    : RenderNode<MAX_FRAMES>(device),
    computeQueue_(computeQueue),
    commandBuffers_(commandBuffers),
    computePass_(std::move(computePass)) {}
    
    NodeDevice getDeviceType() override {
        return NodeDevice::GPU;
    }
    
    void submit(uint32_t frameIndex, VkExtent2D swapchainExtent, uint32_t, VkFramebuffer) override {
        // Update material
        computePass_->update(frameIndex, swapchainExtent);
        
        // Start command buffer
        auto& commandBuffer = commandBuffers_[frameIndex];
        VK_SUCCESS_OR_THROW(vkResetCommandBuffer(commandBuffer, 0),
                            "Failed to reset compute command buffer");
        
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        VK_SUCCESS_OR_THROW(vkBeginCommandBuffer(commandBuffer, &beginInfo),
                            "Failed to begin compute commmand buffer");

        // Bind pipeline & descriptor sets
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, computePass_->getPipeline());
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                computePass_->getPipelineLayout(),
                                0, 1,
                                computePass_->getDescriptorSet(frameIndex),
                                0, 0);
        // Dispatch workgroups
        auto dispatchSize = computePass_->getDispatchDimensions();
        vkCmdDispatch(commandBuffer, dispatchSize.x, dispatchSize.y, dispatchSize.z);
        
        // End command buffer
        vkEndCommandBuffer(commandBuffer);
        
        // Submit Work
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        
        auto& waitSemaphores = RenderNode<MAX_FRAMES>::waitSemaphores_[frameIndex];
        submitInfo.waitSemaphoreCount = waitSemaphores.size();
        submitInfo.pWaitSemaphores = waitSemaphores.data();
        std::array<VkSemaphore,1> signalSemaphores = {**RenderNode<MAX_FRAMES>::signalSemaphores_[frameIndex]};
        submitInfo.signalSemaphoreCount = signalSemaphores.size();
        submitInfo.pSignalSemaphores = signalSemaphores.data();
        
        VK_SUCCESS_OR_THROW(vkQueueSubmit(computeQueue_,
                                          1,
                                          &submitInfo,
                                          **RenderNode<MAX_FRAMES>::signalFences_[frameIndex]),
                            "Failed to submit compute");
    }
    
protected:
    VkQueue computeQueue_;
    std::array<VkCommandBuffer, MAX_FRAMES> commandBuffers_;
    std::unique_ptr<ComputeMaterial<MAX_FRAMES>> computePass_;
};
