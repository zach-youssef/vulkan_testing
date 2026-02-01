#pragma once

#include "RenderGraph.h"

template<uint MAX_FRAMES>
class PresentNode : public RenderNode<MAX_FRAMES> {
public:
    PresentNode<MAX_FRAMES>(VkDevice device,
                            VkQueue presentQueue,
                            VkSwapchainKHR swapChain)
    : RenderNode<MAX_FRAMES>(device),
    presentQueue_(presentQueue),
    swapChain_(swapChain){}
    
    NodeDevice getDeviceType() override {
        return NodeDevice::GPU;
    }
    
    void submit(RenderEvalContext& ctx) override {
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

        auto& waitSemaphores = RenderNode<MAX_FRAMES>::waitSemaphores_[ctx.frameIndex];
        presentInfo.waitSemaphoreCount = waitSemaphores.size();
        presentInfo.pWaitSemaphores = waitSemaphores.data();
        VkSwapchainKHR swapChains[] = {swapChain_};
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapChains;
        presentInfo.pImageIndices = &ctx.imageIndex;
        presentInfo.pResults = nullptr; // Array of VkResults to check for each swapchain?
        
        vkQueuePresentKHR(presentQueue_, &presentInfo);
    }
private:
    VkQueue presentQueue_;
    VkSwapchainKHR swapChain_;
};
