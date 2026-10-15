#pragma once

#include "graph/RenderGraph.h"

class PresentNode : public RenderNode {
public:
    PresentNode(VulkanContext& context)
    : RenderNode(context) {}
    
    NodeDevice getDeviceType() override {
        return NodeDevice::GPU;
    }
    
    void submit(RenderEvalContext& ctx) override {
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

        auto waitSemaphores = getWaitSemaphores(ctx.frameIndex);
        presentInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
        presentInfo.pWaitSemaphores = waitSemaphores.data();
        VkSwapchainKHR swapChains[] = {ctx.swapChain};
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapChains;
        presentInfo.pImageIndices = &ctx.imageIndex;
        presentInfo.pResults = nullptr; // Array of VkResults to check for each swapchain?
        
        context_->present(presentInfo);
    }
};
