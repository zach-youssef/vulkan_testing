#pragma once

#include "graph/RenderGraph.h"
#include "materials/Renderable.h"
#include "core/VkUtil.h"

#include <unordered_set>

class RenderablesNode : public RenderNode {
public:
    RenderablesNode(std::vector<Handle<Renderable>> renderables,
                               VulkanContext& context,
                               VkRenderPass renderPass)
    : RenderNode(context),
    renderPass_(renderPass),
    commandPool_(context.createCommandPool(QueueType::Graphics)),
    commandBuffers_(allocateCommandBuffers(*commandPool_)),
    renderables_(std::move(renderables)) {
        // Renderables may share a material; collect each distinct material once
        // so that it is only updated once per frame
        std::unordered_set<Material*> seen;
        for (auto& renderable : renderables_) {
            auto* material = renderable->getMaterial();
            if (seen.insert(material).second) {
                materials_.push_back(material);
            }
        }
    }

    NodeDevice getDeviceType() override {
        return NodeDevice::GPU;
    }

    void submit(RenderEvalContext& ctx) override {
        // Start the command buffer
        auto& commandBuffer = commandBuffers_[ctx.frameIndex];
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
        renderPassInfo.framebuffer = ctx.frameBuffers.at(ctx.imageIndex);
        
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = ctx.swapchainExtent;
        
        VkClearValue clearColor = {{{0.0f, 0.0f, 0.0f, 0.0f}}};
        VkClearValue clearDepth = {{1.0f, 0.0f}};
        std::array<VkClearValue, 2> clearValues = {clearColor, clearDepth};
        renderPassInfo.clearValueCount = 2;
        renderPassInfo.pClearValues = clearValues.data();

        // Update each material (probably a uniform buffer)
        for (auto* material : materials_) {
            material->update(ctx.frameIndex, ctx.swapchainExtent);
        }

        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        for (auto& renderable : renderables_) {
            // Bind our pipeline, descriptors, and buffers
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            renderable->getMaterial()->getPipeline());
            VkDeviceSize offsets[] {0};
            VkBuffer vertexBuffers[] {renderable->getVertexBuffer()};
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
            vkCmdBindIndexBuffer(commandBuffer, renderable->getIndexBuffer(), 0, VK_INDEX_TYPE_UINT16);
            vkCmdBindDescriptorSets(commandBuffer,
                                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    renderable->getMaterial()->getPipelineLayout(),
                                    0, 1,
                                    renderable->getMaterial()->getDescriptorSet(ctx.frameIndex),
                                    0, nullptr);
            
            // Set up Viewport & Scissor
            VkViewport viewport{};
            viewport.x = 0.0f;
            viewport.y = 0.0f;
            viewport.width = static_cast<float>(ctx.swapchainExtent.width);
            viewport.height = static_cast<float>(ctx.swapchainExtent.height);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
            VkRect2D scissor{};
            scissor.offset = {0, 0};
            scissor.extent = ctx.swapchainExtent;
            vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

            // Issue draw command
            vkCmdDrawIndexed(commandBuffer,
                            renderable->getIndexCount(),
                            1 /*num instances*/,
                            0 /*offset into buffer*/,
                            0 /*offset to add to indices*/,
                            0 /* instancing offset*/);
        }
        
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
        submitInfo.pWaitDstStageMask = waitStages;
        auto waitSemaphores = getWaitSemaphores(ctx.frameIndex);
        submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
        submitInfo.pWaitSemaphores = waitSemaphores.data();
        submitInfo.signalSemaphoreCount = getSignalSemaphoreCount(ctx.frameIndex);
        submitInfo.pSignalSemaphores = getSignalSemaphores(ctx.frameIndex);

        VK_SUCCESS_OR_THROW(context_->submit(QueueType::Graphics,
                                                                     submitInfo,
                                                                     getSignalFence(ctx.frameIndex)),
                            "Failed to submit draw command buffer.");
    }

private:
    VkRenderPass renderPass_;
    std::unique_ptr<CommandPool> commandPool_;
    PerFrame<VkCommandBuffer> commandBuffers_;
    std::vector<Handle<Renderable>> renderables_;
    // Distinct materials of renderables_ (kept alive by the renderables' handles)
    std::vector<Material*> materials_;
};
