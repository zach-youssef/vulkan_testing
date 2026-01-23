#pragma once

#include "VkTypes.h"
#include <functional>

#define RETURN_IF_ERROR(expr)   \
outResult = expr;               \
if (outResult != VK_SUCCESS) {  \
    return;                     \
}

template <typename Data>
class Buffer {
public:
    VkBuffer getBuffer() {
        return **buffer_;
    }
    
    VkDeviceMemory getMemory() {
        return **memory_;
    }
    
    VkDeviceSize getStride() {
        return sizeof(Data);
    }
    
    static VkResult create(std::unique_ptr<Buffer>& outBuffer,
                           size_t numElements,
                           VkBufferUsageFlags usage,
                           VkMemoryPropertyFlags properties,
                           VkDevice device,
                           VkPhysicalDevice physicalDevice) {
        VkResult result;
        
        outBuffer = std::make_unique<Buffer<Data>>(numElements, usage, properties, device, physicalDevice, result);
        
        return result;
    }

    static void copyBuffer(VkBuffer src,
                           VkBuffer dst,
                           VkDeviceSize size,
                           VkQueue graphicsQueue,
                           VkDevice device,
                           VkCommandPool commandPool) {
        issueSingleTimeCommand([src, dst, size](VkCommandBuffer commandBuffer){
            VkBufferCopy copyRegion{};
            copyRegion.srcOffset = 0;
            copyRegion.dstOffset = 0;
            copyRegion.size = size;
            vkCmdCopyBuffer(commandBuffer, src, dst, 1 /*regionCount*/, &copyRegion);
        }, graphicsQueue, device, commandPool);
    }

    static void createAndInitialize(std::unique_ptr<Buffer<Data>>& buffer,
                                    const std::vector<Data>& data,
                                    VkBufferUsageFlags usage,
                                    VkDevice device,
                                    VkPhysicalDevice physicalDevice,
                                    VkQueue graphicsQueue,
                                    VkCommandPool commandPool) {
        size_t bufferSize = sizeof(Data) * data.size();
        
        std::unique_ptr<Buffer<Data>> stagingBuffer;
        VK_SUCCESS_OR_THROW(Buffer<Data>::create(stagingBuffer,
                                                   data.size(),
                                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                                   device,
                                                   physicalDevice),
                            "Failed to create staging buffer");
        
        stagingBuffer->mapAndExecute(0, bufferSize, [bufferSize, &data] (void* mappedData){
            memcpy(mappedData, data.data(), bufferSize);
        });
        
        VK_SUCCESS_OR_THROW(Buffer<Data>::create(buffer,
                                                 data.size(),
                                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                 device,
                                                 physicalDevice),
                            "Failed to create gpu buffer");
        
        copyBuffer(stagingBuffer->getBuffer(),
                   buffer->getBuffer(),
                   bufferSize,
                   graphicsQueue,
                   device,
                   commandPool);
    }

    Buffer(size_t numElements,
           VkBufferUsageFlags usage,
           VkMemoryPropertyFlags properties,
           VkDevice device,
           VkPhysicalDevice physicalDevice,
           VkResult& outResult) : device_(device) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = sizeof(Data) * numElements;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE; // TODO: parameterize
        
        RETURN_IF_ERROR(VulkanBuffer::create(buffer_, device_, bufferInfo))
        
        VkMemoryRequirements memoryRequirements;
        vkGetBufferMemoryRequirements(device_, **buffer_, &memoryRequirements);

        RETURN_IF_ERROR(VulkanMemory::createFromRequirements(memory_,
                                                             device_,
                                                             physicalDevice,
                                                             properties,
                                                             memoryRequirements))
        
        vkBindBufferMemory(device_, **buffer_, **memory_, 0);
    }
    
    void mapAndExecute(VkDeviceSize offset,
                       VkDeviceSize size,
                       std::function<void(void*)> op) {
        void* mappedData;
        vkMapMemory(device_, **memory_, offset, size, 0, &mappedData);
        
        op(mappedData);
        
        vkUnmapMemory(device_, **memory_);
    }
    
    std::unique_ptr<Data, std::function<void(Data*)>> getPersistentMapping(VkDeviceSize offset, VkDeviceSize size) {
        void* data;
        
        vkMapMemory(device_, **memory_, offset, size, 0, &data);
        
        return std::unique_ptr<Data, std::function<void(Data*)>>((Data*) data, [device = device_, memory = **memory_](Data*){
            vkUnmapMemory(device, memory);
        });
    }
    
private:
    VkDevice device_;
    std::unique_ptr<VulkanBuffer> buffer_;
    std::unique_ptr<VulkanMemory> memory_;
};
