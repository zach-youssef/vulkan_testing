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
