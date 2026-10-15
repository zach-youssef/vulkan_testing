#pragma once

#include "graph/RenderGraph.h"

class AcquireImageNode : public RenderNode {
public:
    AcquireImageNode(VulkanContext& context)
    : RenderNode(context){}
    
    NodeDevice getDeviceType() override {
        return NodeDevice::GPU;
    }
    
    void submit(RenderEvalContext& ctx) override {
        auto result = vkAcquireNextImageKHR(device(),
                                            ctx.swapChain,
                                            UINT64_MAX,
                                            getFrameSignalSemaphore(ctx.frameIndex),
                                            VK_NULL_HANDLE,
                                            &ctx.imageIndex);
        ctx.shouldRecreateSwapChain |= result == VK_ERROR_OUT_OF_DATE_KHR
                                      || result == VK_SUBOPTIMAL_KHR;
    }
};
