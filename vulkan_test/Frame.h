#pragma once

#include "VkUtil.h"
#include "VkTypes.h"

class Frame {
public:
    Frame(VkDevice device, VkCommandPool commandPool): device_(device) {
        createCommandBuffer(commandPool);
        createSyncObjects();
    }
    
    
    void waitForFenceAndReset() {
        vkWaitForFences(device_, 1, (*inFlightFence_).get(), VK_TRUE, UINT64_MAX); // disabled timeout
        vkResetFences(device_, 1, (*inFlightFence_).get());
    }
    
    uint32_t aquireImageIndex(VkSwapchainKHR swapChain){
        uint32_t imageIndex;
        vkAcquireNextImageKHR(device_, swapChain, UINT64_MAX, **imageAvailableSemaphore_, VK_NULL_HANDLE, &imageIndex);
        return imageIndex;
    }
    
    VkCommandBuffer getCommandBuffer() {
        return commandBuffer_;
    }
    
    void submit(uint32_t imageIndex, VkQueue graphicsQueue, VkQueue presentQueue, VkSwapchainKHR swapChain) {
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer_;
        
        // (it should wait for the swapchain image to be available before writing out to it)
        VkSemaphore waitSemaphores[] = {**imageAvailableSemaphore_};
        VkPipelineStageFlags waitStages[] {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer_;
        VkSemaphore signalSemaphores[] = {**renderFinishedSemaphore_};
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;
        
        VK_SUCCESS_OR_THROW(vkQueueSubmit(graphicsQueue, 1, &submitInfo, **inFlightFence_),
                            "Failed to submit draw command buffer.");
        
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;
        VkSwapchainKHR swapChains[] = {swapChain};
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapChains;
        presentInfo.pImageIndices = &imageIndex;
        presentInfo.pResults = nullptr; // Array of VkResults to check for each swapchain?
        
        vkQueuePresentKHR(presentQueue, &presentInfo);
    }
    
private:
    void createCommandBuffer(VkCommandPool commandPool) {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        
        VK_SUCCESS_OR_THROW(vkAllocateCommandBuffers(device_, &allocInfo, &commandBuffer_),
                            "Failed to allocate command buffers");
    }
    
    void createSyncObjects() {
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // Init fence as signaled so first frame isn't blocked
        
        VK_SUCCESS_OR_THROW(VulkanSemaphore::create(imageAvailableSemaphore_, device_, semaphoreInfo),
                            "Failed to create image available semaphore.");
        VK_SUCCESS_OR_THROW(VulkanSemaphore::create(renderFinishedSemaphore_, device_, semaphoreInfo),
                            "Failed to create render finished semaphore.");
        VK_SUCCESS_OR_THROW(VulkanFence::create(inFlightFence_, device_, fenceInfo),
                            "Failed to create in-flight fence.");
    }
    
private:
    VkCommandBuffer commandBuffer_;
    VkDevice device_;
    
    std::unique_ptr<VulkanSemaphore> imageAvailableSemaphore_;
    std::unique_ptr<VulkanSemaphore> renderFinishedSemaphore_;
    std::unique_ptr<VulkanFence> inFlightFence_;
};
