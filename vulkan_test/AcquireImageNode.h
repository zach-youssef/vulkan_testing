#pragma once

#include "RenderGraph.h"

template<uint MAX_FRAMES>
class AcquireImageNode : public RenderNode<MAX_FRAMES> {
public:
    AcquireImageNode<MAX_FRAMES>(VkDevice device, VkSwapchainKHR swapChain)
    : RenderNode<MAX_FRAMES>(device), swapChain_(swapChain){}
    
    NodeDevice getDeviceType() override {
        return NodeDevice::GPU;
    }
    
    void submit(RenderEvalContext& ctx) override {
        auto result = vkAcquireNextImageKHR(RenderNode<MAX_FRAMES>::device_,
                                            swapChain_,
                                            UINT64_MAX,
                                            **RenderNode<MAX_FRAMES>::signalSemaphores_[ctx.frameIndex],
                                            VK_NULL_HANDLE,
                                            &ctx.imageIndex);
        ctx.shouldRecreateSwapChain = result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR;
    }
private:
    VkSwapchainKHR swapChain_;
};
