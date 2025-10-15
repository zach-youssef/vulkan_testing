#pragma once

#include "VkUtil.h"

class Frame {
public:
    Frame(Handle<VkDevice>& device): device(device) {}
    
    void init(Handle<VkCommandPool>& commandPool) {
        createCommandBuffer(commandPool);
        createSyncObjects();
    }
    
    void waitForFenceAndReset() {
        vkWaitForFences(*this->device, 1, this->inFlightFence.get(), VK_TRUE, UINT64_MAX); // disabled timeout
        vkResetFences(*this->device, 1, this->inFlightFence.get());
    }
    
    uint32_t aquireImageIndex(Handle<VkSwapchainKHR>& swapChain){
        uint32_t imageIndex;
        vkAcquireNextImageKHR(*this->device, *swapChain, UINT64_MAX, *this->imageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex);
        return imageIndex;
    }
    
    VkCommandBuffer getCommandBuffer() {
        return this->commandBuffer;
    }
    
    void submit(uint32_t imageIndex, VkQueue graphicsQueue, VkQueue presentQueue, Handle<VkSwapchainKHR>& swapChain) {
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        
        // (it should wait for the swapchain image to be available before writing out to it)
        VkSemaphore waitSemaphores[] = {*this->imageAvailableSemaphore};
        VkPipelineStageFlags waitStages[] {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        VkSemaphore signalSemaphores[] = {*this->renderFinishedSemaphore};
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;
        
        VK_SUCCESS_OR_THROW(vkQueueSubmit(graphicsQueue, 1, &submitInfo, *this->inFlightFence),
                            "Failed to submit draw command buffer.");
        
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;
        VkSwapchainKHR swapChains[] = {*swapChain};
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapChains;
        presentInfo.pImageIndices = &imageIndex;
        presentInfo.pResults = nullptr; // Array of VkResults to check for each swapchain?
        
        vkQueuePresentKHR(presentQueue, &presentInfo);
    }
    
private:
    void createCommandBuffer(Handle<VkCommandPool>& commandPool) {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = *commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        
        VK_SUCCESS_OR_THROW(vkAllocateCommandBuffers(*this->device, &allocInfo, &this->commandBuffer),
                            "Failed to allocate command buffers");
    }
    
    void createSyncObjects() {
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // Init fence as signaled so first frame isn't blocked
        
        VK_SUCCESS_OR_THROW(vkCreateSemaphore(*this->device, &semaphoreInfo, nullptr, this->imageAvailableSemaphore.get()),
                            "Failed to create image available semaphore.");
        VK_SUCCESS_OR_THROW(vkCreateSemaphore(*this->device, &semaphoreInfo, nullptr, this->renderFinishedSemaphore.get()),
                            "Failed to create render finished semaphore.");
        VK_SUCCESS_OR_THROW(vkCreateFence(*this->device, &fenceInfo, nullptr, this->inFlightFence.get()),
                            "Failed to create in-flight fence.");
    }
    
private:
    VkCommandBuffer commandBuffer;
    Handle<VkDevice>& device;
    
    Handle<VkSemaphore> imageAvailableSemaphore{
        deleterWithDevice<VkSemaphore>(this->device, vkDestroySemaphore)};
    Handle<VkSemaphore> renderFinishedSemaphore{
        deleterWithDevice<VkSemaphore>(this->device, vkDestroySemaphore)};
    Handle<VkFence> inFlightFence{
        deleterWithDevice<VkFence>(this->device, vkDestroyFence)};
};
