#pragma once

#include "graph/RenderGraph.h"

template<uint MAX_FRAMES>
class AcquireImageNode : public RenderNode<MAX_FRAMES> {
public:
    AcquireImageNode<MAX_FRAMES>(VulkanContext& context)
    : RenderNode<MAX_FRAMES>(context){}
    
    NodeDevice getDeviceType() override {
        return NodeDevice::GPU;
    }
    
    void submit(RenderEvalContext& ctx) override {
        auto result = vkAcquireNextImageKHR(RenderNode<MAX_FRAMES>::device(),
                                            ctx.swapChain,
                                            UINT64_MAX,
                                            **RenderNode<MAX_FRAMES>::signalSemaphores_[ctx.frameIndex],
                                            VK_NULL_HANDLE,
                                            &ctx.imageIndex);
        ctx.shouldRecreateSwapChain |= result == VK_ERROR_OUT_OF_DATE_KHR
                                      || result == VK_SUBOPTIMAL_KHR;
    }
};
