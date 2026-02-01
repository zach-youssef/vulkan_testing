#pragma once

#include "RenderGraph.h"

template<uint MAX_FRAMES>
class PresentNode : public RenderNode<MAX_FRAMES> {
public:
    PresentNode<MAX_FRAMES>(VkDevice device,
                            VkQueue presentQueue)
    : RenderNode<MAX_FRAMES>(device),
    presentQueue_(presentQueue) {}
    
    NodeDevice getDeviceType() override {
        return NodeDevice::GPU;
    }
    
    void submit(RenderEvalContext& ctx) override {
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

        auto& waitSemaphores = RenderNode<MAX_FRAMES>::waitSemaphores_[ctx.frameIndex];
        presentInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
        presentInfo.pWaitSemaphores = waitSemaphores.data();
        VkSwapchainKHR swapChains[] = {ctx.swapChain};
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapChains;
        presentInfo.pImageIndices = &ctx.imageIndex;
        presentInfo.pResults = nullptr; // Array of VkResults to check for each swapchain?
        
        vkQueuePresentKHR(presentQueue_, &presentInfo);
    }
private:
    VkQueue presentQueue_;
};
