#pragma once

#include "core/VkTypes.h"
#include "core/ResourceManager.h"
#include "core/VulkanContext.h"
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

    // Size in bytes
    VkDeviceSize getSize() {
        return size_;
    }
    
    // Creates a buffer owned by the context's ResourceManager
    static VkResult create(Handle<Buffer>& outBuffer,
                           VulkanContext& context,
                           size_t numElements,
                           VkBufferUsageFlags usage,
                           VkMemoryPropertyFlags properties) {
        VkResult result;
        // Constructed directly since the constructor is private
        std::unique_ptr<Buffer> buffer(new Buffer(numElements, usage, properties, context, result));
        outBuffer = context.resources().adopt(std::move(buffer));
        return result;
    }
    
    // Creates an unmanaged buffer for strictly scoped use, such as a staging buffer. Throws on failure.
    static std::unique_ptr<Buffer> createScoped(VulkanContext& context,
                                                size_t numElements,
                                                VkBufferUsageFlags usage,
                                                VkMemoryPropertyFlags properties) {
        VkResult result;
        std::unique_ptr<Buffer> buffer(new Buffer(numElements, usage, properties, context, result));
        VK_SUCCESS_OR_THROW(result, "Failed to create buffer");
        return buffer;
    }

    // Copies size bytes from src to dst, blocking until the copy completes
    static void copyBuffer(VulkanContext& context,
                           VkBuffer src,
                           VkBuffer dst,
                           VkDeviceSize size) {
        context.submitImmediate([src, dst, size](VkCommandBuffer commandBuffer){
            VkBufferCopy copyRegion{};
            copyRegion.srcOffset = 0;
            copyRegion.dstOffset = 0;
            copyRegion.size = size;
            vkCmdCopyBuffer(commandBuffer, src, dst, 1 /*regionCount*/, &copyRegion);
        });
    }
    
    // Creates a managed buffer holding data, uploaded through a staging buffer
    static void createAndInitialize(Handle<Buffer<Data>>& outBuffer,
                                    VulkanContext& context,
                                    const std::vector<Data>& data,
                                    VkBufferUsageFlags usage,
                                    VkMemoryPropertyFlags memFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
        size_t bufferSize = sizeof(Data) * data.size();
        
        auto stagingBuffer = createScoped(context,
                                          data.size(),
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        
        stagingBuffer->mapAndExecute(0, bufferSize, [bufferSize, &data] (void* mappedData){
            memcpy(mappedData, data.data(), bufferSize);
        });
        
        VK_SUCCESS_OR_THROW(Buffer<Data>::create(outBuffer,
                                                 context,
                                                 data.size(),
                                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
                                                 memFlags),
                            "Failed to create gpu buffer");
        
        copyBuffer(context, stagingBuffer->getBuffer(), outBuffer->getBuffer(), bufferSize);
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
    Buffer(size_t numElements,
           VkBufferUsageFlags usage,
           VkMemoryPropertyFlags properties,
           VulkanContext& context,
           VkResult& outResult) : device_(context.device()), size_(sizeof(Data) * numElements) {
        VkPhysicalDevice physicalDevice = context.physicalDevice();
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = sizeof(Data) * numElements;
        bufferInfo.usage = usage;
        // Usable from any of the context's queues without ownership transfers
        const auto& families = context.queueFamilies();
        bufferInfo.sharingMode = families.size() > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
        bufferInfo.queueFamilyIndexCount = families.size() > 1 ? static_cast<uint32_t>(families.size()) : 0;
        bufferInfo.pQueueFamilyIndices = families.size() > 1 ? families.data() : nullptr;
        
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
    
    VkDevice device_;
    VkDeviceSize size_;
    std::unique_ptr<VulkanBuffer> buffer_;
    std::unique_ptr<VulkanMemory> memory_;
};
