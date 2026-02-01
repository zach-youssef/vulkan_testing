#pragma once

#include "RenderGraph.h"
#include "Renderable.h"
#include "VkUtil.h"

template<uint MAX_FRAMES>
class RenderableNode : public RenderNode<MAX_FRAMES> {
public:
    RenderableNode<MAX_FRAMES>(std::unique_ptr<Renderable<MAX_FRAMES>>&& renderable,
                               VkDevice device,
                               VkQueue graphicsQueue,
                               VkRenderPass renderPass,
                               std::array<VkCommandBuffer, MAX_FRAMES> commandBuffers)
    : RenderNode<MAX_FRAMES>(device),
    graphicsQueue_(graphicsQueue),
    renderPass_(renderPass),
    commandBuffers_(commandBuffers),
    renderable_(std::move(renderable)){}
    
    NodeDevice getDeviceType() override {
        return NodeDevice::GPU;
    }
    
    void submit(uint32_t frameIndex, VkExtent2D swapchainExtent, uint32_t, VkFramebuffer framebuffer) override {
        // Update the renderable (probably a uniform buffer)
        renderable_->update(frameIndex, swapchainExtent);
        
        // Start the command buffer
        auto& commandBuffer = commandBuffers_[frameIndex];
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = 0; // Optional
        beginInfo.pInheritanceInfo = nullptr; // Optional
        
        VK_SUCCESS_OR_THROW(vkBeginCommandBuffer(commandBuffer, &beginInfo),
                            "Failed to begin recording command buffer");
        
        // Begin the render pass
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = renderPass_;
        renderPassInfo.framebuffer = framebuffer;
        
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = swapchainExtent;
        
        VkClearValue clearColor = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &clearColor;

        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        
        // Bind our pipeline, descriptors, and buffers
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          renderable_->getMaterial()->getPipeline());
        VkDeviceSize offsets[] {0};
        VkBuffer vertexBuffers[] {renderable_->getVertexBuffer()};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(commandBuffer, renderable_->getIndexBuffer(), 0, VK_INDEX_TYPE_UINT16);
        vkCmdBindDescriptorSets(commandBuffer,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                renderable_->getMaterial()->getPipelineLayout(),
                                0, 1,
                                renderable_->getMaterial()->getDescriptorSet(frameIndex),
                                0, nullptr);
        
        // Set up Viewport & Scissor
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(swapchainExtent.width);
        viewport.height = static_cast<float>(swapchainExtent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = swapchainExtent;
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

        // Issue draw command
        vkCmdDrawIndexed(commandBuffer,
                         renderable_->getIndexCount(),
                         1 /*num instances*/,
                         0 /*offset into buffer*/,
                         0 /*offset to add to indices*/,
                         0 /* instancing offset*/);
        
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
        auto& waitSemaphores = RenderNode<MAX_FRAMES>::waitSemaphores_[frameIndex];
        submitInfo.waitSemaphoreCount = waitSemaphores.size();
        submitInfo.pWaitSemaphores = waitSemaphores.data();
        std::array<VkSemaphore,1> signalSemaphores = {**RenderNode<MAX_FRAMES>::signalSemaphores_[frameIndex]};
        submitInfo.signalSemaphoreCount = signalSemaphores.size();
        submitInfo.pSignalSemaphores = signalSemaphores.data();

        VK_SUCCESS_OR_THROW(vkQueueSubmit(graphicsQueue_,
                                          1,
                                          &submitInfo,
                                          **RenderNode<MAX_FRAMES>::signalFences_[frameIndex]),
                            "Failed to submit draw command buffer.");
    }

private:
    VkQueue graphicsQueue_;
    VkRenderPass renderPass_;
    std::array<VkCommandBuffer, MAX_FRAMES> commandBuffers_;
    std::unique_ptr<Renderable<MAX_FRAMES>> renderable_;
};
