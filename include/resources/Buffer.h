#pragma once

#include "core/VkTypes.h"
#include "core/ResourceManager.h"
#include "core/VulkanContext.h"
#include "core/Result.h"
#include <functional>

template <typename Data>
class Buffer {
public:
    // A mapping of the buffer's memory, unmapped when destroyed
    using Mapping = std::unique_ptr<Data, std::function<void(Data*)>>;

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

    // What it was created for (e.g. VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT), which a graph importing it
    // checks its uses against
    VkBufferUsageFlags getUsage() const {
        return usage_;
    }
    
    // Creates a buffer owned by the context's ResourceManager
    static Result<Handle<Buffer>> create(VulkanContext& context,
                                         size_t numElements,
                                         VkBufferUsageFlags usage,
                                         VkMemoryPropertyFlags properties) {
        TRY_ASSIGN(auto buffer, createScoped(context, numElements, usage, properties));
        return context.resources().adopt(std::move(buffer));
    }
    
    // Creates an unmanaged buffer for strictly scoped use, such as a staging buffer
    static Result<std::unique_ptr<Buffer>> createScoped(VulkanContext& context,
                                                        size_t numElements,
                                                        VkBufferUsageFlags usage,
                                                        VkMemoryPropertyFlags properties) {
        // Constructed directly since the constructor is private
        std::unique_ptr<Buffer> buffer(new Buffer(context.device(), sizeof(Data) * numElements));
        TRY(buffer->init(context, usage, properties));
        return buffer;
    }

    // Copies size bytes from src to dst, blocking until the copy completes
    static Result<void> copyBuffer(VulkanContext& context,
                                   VkBuffer src,
                                   VkBuffer dst,
                                   VkDeviceSize size) {
        return context.submitImmediate([src, dst, size](VkCommandBuffer commandBuffer){
            VkBufferCopy copyRegion{};
            copyRegion.srcOffset = 0;
            copyRegion.dstOffset = 0;
            copyRegion.size = size;
            vkCmdCopyBuffer(commandBuffer, src, dst, 1 /*regionCount*/, &copyRegion);
        });
    }
    
    // Creates a managed buffer holding data, uploaded through a staging buffer
    static Result<Handle<Buffer<Data>>> createAndInitialize(VulkanContext& context,
                                                            const std::vector<Data>& data,
                                                            VkBufferUsageFlags usage,
                                                            VkMemoryPropertyFlags memFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
        size_t bufferSize = sizeof(Data) * data.size();
        
        TRY_ASSIGN(auto stagingBuffer, createScoped(context,
                                                    data.size(),
                                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
        
        TRY(stagingBuffer->mapAndExecute(0, bufferSize, [bufferSize, &data] (void* mappedData){
            memcpy(mappedData, data.data(), bufferSize);
        }));
        
        TRY_ASSIGN(auto buffer, Buffer<Data>::create(context,
                                                     data.size(),
                                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
                                                     memFlags));
        
        TRY(copyBuffer(context, stagingBuffer->getBuffer(), buffer->getBuffer(), bufferSize));
        return buffer;
    }

    // Maps size bytes from offset, calls op with the mapping, and unmaps them
    Result<void> mapAndExecute(VkDeviceSize offset,
                               VkDeviceSize size,
                               std::function<void(void*)> op) {
        void* mappedData;
        VK_TRY(vkMapMemory(device_, **memory_, offset, size, 0, &mappedData), "Failed to map buffer memory");
        
        op(mappedData);
        
        vkUnmapMemory(device_, **memory_);
        return {};
    }
    
    // Maps size bytes from offset until the returned mapping is destroyed
    Result<Mapping> getPersistentMapping(VkDeviceSize offset, VkDeviceSize size) {
        void* data;
        
        VK_TRY(vkMapMemory(device_, **memory_, offset, size, 0, &data), "Failed to map buffer memory");
        
        return Mapping((Data*) data, [device = device_, memory = **memory_](Data*){
            vkUnmapMemory(device, memory);
        });
    }
    
private:
    Buffer(VkDevice device, VkDeviceSize size) : device_(device), size_(size) {}

    Result<void> init(VulkanContext& context, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) {
        VkPhysicalDevice physicalDevice = context.physicalDevice();
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size_;
        bufferInfo.usage = usage;
        usage_ = usage;
        // Usable from any of the context's queues without ownership transfers
        const auto& families = context.queueFamilies();
        bufferInfo.sharingMode = families.size() > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
        bufferInfo.queueFamilyIndexCount = families.size() > 1 ? static_cast<uint32_t>(families.size()) : 0;
        bufferInfo.pQueueFamilyIndices = families.size() > 1 ? families.data() : nullptr;
        
        VK_TRY(VulkanBuffer::create(buffer_, device_, bufferInfo), "Failed to create buffer");
        
        VkMemoryRequirements memoryRequirements;
        vkGetBufferMemoryRequirements(device_, **buffer_, &memoryRequirements);

        VK_TRY(VulkanMemory::createFromRequirements(memory_,
                                                    device_,
                                                    physicalDevice,
                                                    properties,
                                                    memoryRequirements),
               "Failed to allocate buffer memory");
        
        VK_TRY(vkBindBufferMemory(device_, **buffer_, **memory_, 0), "Failed to bind buffer memory");
        return {};
    }
    
    VkDevice device_;
    VkDeviceSize size_;
    VkBufferUsageFlags usage_ = 0;
    std::unique_ptr<VulkanBuffer> buffer_;
    std::unique_ptr<VulkanMemory> memory_;
};

// Memory the CPU writes and shaders read: device local where the device can also map it (all
// memory on unified memory architectures such as Apple's, and resizable BAR on discrete GPUs), so
// shaders read video memory rather than across the bus; otherwise host memory
inline VkMemoryPropertyFlags mappedMemoryFor(VulkanContext& context) {
    constexpr VkMemoryPropertyFlags mapped = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(context.physicalDevice(), &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & (mapped | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) == (mapped | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            return mapped | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        }
    }
    return mapped;
}
