#pragma once

#include "graph/RenderGraph.h"
#include "materials/Renderable.h"
#include "core/VkUtil.h"

class ComputeNode : public RenderNode {
public:
    ComputeNode(Handle<ComputeMaterial> computePass,
                            VulkanContext& context)
    : RenderNode(context),
    commandPool_(context.createCommandPool(QueueType::Compute)),
    commandBuffers_(allocateCommandBuffers(*commandPool_)),
    computePass_(std::move(computePass)) {}
    
    NodeDevice getDeviceType() override {
        return NodeDevice::GPU;
    }
    
    void submit(RenderEvalContext& ctx) override {
        // Update material
        computePass_->update(ctx.frameIndex, ctx.swapchainExtent);
        
        // Start command buffer
        auto& commandBuffer = commandBuffers_[ctx.frameIndex];
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
                                computePass_->getDescriptorSet(ctx.frameIndex),
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
        
        auto waitSemaphores = getWaitSemaphores(ctx.frameIndex);
        std::vector<VkPipelineStageFlags> waitStages(waitSemaphores.size(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
        submitInfo.pWaitSemaphores = waitSemaphores.data();
        submitInfo.pWaitDstStageMask = waitStages.data();
        submitInfo.signalSemaphoreCount = getSignalSemaphoreCount(ctx.frameIndex);
        submitInfo.pSignalSemaphores = getSignalSemaphores(ctx.frameIndex);
        
        VK_SUCCESS_OR_THROW(context_->submit(QueueType::Compute,
                                                                     submitInfo,
                                                                     getSignalFence(ctx.frameIndex)),
                            "Failed to submit compute");
    }
    
protected:
    std::unique_ptr<CommandPool> commandPool_;
    PerFrame<VkCommandBuffer> commandBuffers_;
    Handle<ComputeMaterial> computePass_;
};
