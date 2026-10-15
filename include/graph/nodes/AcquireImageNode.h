#pragma once

#include "graph/RenderGraph.h"

class AcquireImageNode : public RenderNode {
public:
    AcquireImageNode(VulkanContext& context)
    : RenderNode(context){}
    
    NodeDevice getDeviceType() override {
        return NodeDevice::GPU;
    }
    
    bool acquiresImage() override {
        return true;
    }
    
    void submit(RenderEvalContext& ctx) override {
        auto result = vkAcquireNextImageKHR(device(),
                                            ctx.swapChain,
                                            UINT64_MAX,
                                            getFrameSignalSemaphore(ctx.frameIndex),
                                            VK_NULL_HANDLE,
                                            &ctx.imageIndex);
        // A suboptimal image can still be rendered to and presented; an out of date swapchain
        // provides no image at all
        ctx.shouldRecreateSwapChain |= result == VK_ERROR_OUT_OF_DATE_KHR
                                      || result == VK_SUBOPTIMAL_KHR;
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            ctx.abortFrame = true;
        } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            VK_SUCCESS_OR_THROW(result, "Failed to acquire swapchain image");
        }
    }
};
