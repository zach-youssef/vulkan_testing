#pragma once

#include "VkUtil.h"
#include "VkTypes.h"
#include "Buffer.h"
#include "Ubo.h"

#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

class Frame {
public:
    Frame(VkDevice device,
          VkCommandPool commandPool,
          VkPhysicalDevice physicalDevice,
          VkDescriptorSet descriptorSet,
          VkImageView imageView,
          VkSampler sampler)
    : device_(device), descriptorSet_(descriptorSet) {
        createCommandBuffer(commandPool);
        createSyncObjects();
        createUniformBuffer(physicalDevice);
        populateDescriptorSet(imageView, sampler);
    }
    
    void waitForFence() {
        vkWaitForFences(device_, 1, (*inFlightFence_).get(), VK_TRUE, UINT64_MAX); // disabled timeout
    }
    
    void resetFence() {
        vkResetFences(device_, 1, (*inFlightFence_).get());
    }
    
    uint32_t aquireImageIndex(VkSwapchainKHR swapChain, bool& outShouldRecreateSwapChain){
        uint32_t imageIndex;
        auto result = vkAcquireNextImageKHR(device_, swapChain, UINT64_MAX, **imageAvailableSemaphore_, VK_NULL_HANDLE, &imageIndex);
        
        outShouldRecreateSwapChain = result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR;
        
        return imageIndex;
    }
    
    VkCommandBuffer getCommandBuffer() {
        return commandBuffer_;
    }
    
    VkDescriptorSet* getDescriptorSet() {
        return &descriptorSet_;
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
    
    void updateUniformBuffer(uint32_t currentImage, VkExtent2D swapChainExtent) {
        static auto startTime = std::chrono::high_resolution_clock::now();
        
        auto currentTime = std::chrono::high_resolution_clock::now();
        float time = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - startTime).count();
        
        auto model = glm::rotate(glm::mat4(1.0f), time * glm::radians(90.f), glm::vec3(0.0f, 0.0f, 1.0f));
        auto view =
        glm::lookAt(glm::vec3(2.0f, 2.0f, 2.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        auto projection = glm::perspective(glm::radians(45.0f), swapChainExtent.width / (float) swapChainExtent.height, 0.1f, 10.0f);
        
        // GLM was originally designed for OpenGL where the Y coordinate of the clip coordinates is inverted
        projection[1][1] *= -1;

        auto ubo = UniformBufferObject::fromModelViewProjection(model, view, projection);
        
        memcpy(mappedUniformBuffer_.get(), &ubo, sizeof(ubo));
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
    
    void createUniformBuffer(VkPhysicalDevice physicalDevice) {
        VK_SUCCESS_OR_THROW(Buffer<UniformBufferObject>::create(uniformBuffer_,
                                                                1,
                                                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                                                                device_,
                                                                physicalDevice),
                            "Failed to create uniform buffer.");
        mappedUniformBuffer_ = uniformBuffer_->getPersistentMapping(0, sizeof(UniformBufferObject));
    }
    
    void populateDescriptorSet(VkImageView imageView, VkSampler sampler) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = uniformBuffer_->getBuffer();
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(UniformBufferObject);
        
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = imageView;
        imageInfo.sampler = sampler;
        
        VkWriteDescriptorSet bufferDescriptorWrite{};
        bufferDescriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        bufferDescriptorWrite.dstSet = descriptorSet_;
        bufferDescriptorWrite.dstBinding = 0;
        bufferDescriptorWrite.dstArrayElement = 0;
        bufferDescriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bufferDescriptorWrite.descriptorCount = 1;
        bufferDescriptorWrite.pBufferInfo = &bufferInfo;
        bufferDescriptorWrite.pImageInfo = nullptr; // Optional
        bufferDescriptorWrite.pTexelBufferView = nullptr; // Optional
        
        VkWriteDescriptorSet imageDescriptorWrite{};
        imageDescriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        imageDescriptorWrite.dstSet = descriptorSet_;
        imageDescriptorWrite.dstBinding = 1;
        imageDescriptorWrite.dstArrayElement = 0;
        imageDescriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        imageDescriptorWrite.descriptorCount = 1;
        imageDescriptorWrite.pImageInfo = &imageInfo;
        
        std::array<VkWriteDescriptorSet, 2> descriptorWrites{
            bufferDescriptorWrite, imageDescriptorWrite
        };
        

        vkUpdateDescriptorSets(device_, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
    }
    
private:
    VkCommandBuffer commandBuffer_;
    VkDevice device_;
    VkDescriptorSet descriptorSet_;
    
    std::unique_ptr<VulkanSemaphore> imageAvailableSemaphore_;
    std::unique_ptr<VulkanSemaphore> renderFinishedSemaphore_;
    std::unique_ptr<VulkanFence> inFlightFence_;
    
    std::unique_ptr<Buffer<UniformBufferObject>> uniformBuffer_;
    // Must be freed before uniform buffer
    std::unique_ptr<UniformBufferObject, std::function<void(UniformBufferObject*)>> mappedUniformBuffer_;
};
