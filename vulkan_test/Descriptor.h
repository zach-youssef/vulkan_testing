#pragma once

#include "VkTypes.h"
#include "Buffer.h"
#include "Image.h"
#include "ResourceManager.h"

// Abstract class representing any descriptor
class Descriptor {
public:
    virtual ~Descriptor() = default;
    
    VkDescriptorType getType() {
        return type_;
    }
    
    VkShaderStageFlags getStageFlags() {
        return stageFlags_;
    }
    
    virtual VkDescriptorBufferInfo* getBufferInfo(const uint32_t frameIndex) = 0;
    virtual VkDescriptorImageInfo* getImageInfo(const uint32_t frameIndex) = 0;

protected:
    Descriptor(VkDescriptorType type, VkShaderStageFlags stageFlags): type_(type), stageFlags_(stageFlags) {}
private:
    VkDescriptorType type_;
    VkShaderStageFlags stageFlags_;
};

// Generic buffer descriptor
template<typename Data, uint MAX_FRAMES>
class BufferDescriptor : public Descriptor {
public:
    BufferDescriptor(VkDescriptorType type, VkShaderStageFlags stageFlags, 
                     std::array<Handle<Buffer<Data>>, MAX_FRAMES> buffers, int size, int offset)
    : Descriptor(type, stageFlags), buffers_(std::move(buffers)) {
        for (uint frameIdx = 0; frameIdx < MAX_FRAMES; ++frameIdx) {
            bufferInfos_[frameIdx].buffer = buffers_[frameIdx] ? buffers_[frameIdx]->getBuffer() : VK_NULL_HANDLE;
            bufferInfos_[frameIdx].offset = offset;
            bufferInfos_[frameIdx].range = sizeof(Data) * size;
        }
    }
    BufferDescriptor(VkDescriptorType type, VkShaderStageFlags stageFlags, 
                     std::array<Handle<Buffer<Data>>, MAX_FRAMES> buffers, int size)
    : BufferDescriptor(type, stageFlags, std::move(buffers), size, 0) {}

    VkDescriptorBufferInfo* getBufferInfo(const uint32_t frameIndex) override {
        return &bufferInfos_.at(frameIndex);
    }
    
    VkDescriptorImageInfo* getImageInfo(const uint32_t frameIndex) override {
        return nullptr;
    }

    void bindBuffer(Handle<Buffer<Data>> buffer, int size, const uint32_t frameIndex = 0) {
        bufferInfos_[frameIndex].buffer = buffer->getBuffer();
        bufferInfos_[frameIndex].range = sizeof(Data) * size;
        buffers_[frameIndex] = std::move(buffer);
    }
private:
    // Keeps the referenced buffers alive for as long as this descriptor
    std::array<Handle<Buffer<Data>>, MAX_FRAMES> buffers_;
    std::array<VkDescriptorBufferInfo, MAX_FRAMES> bufferInfos_;
};

// Descriptor for uniform buffers
template<typename Ubo, uint MAX_FRAMES>
class UniformBufferDescriptor : public Descriptor {
public:
    UniformBufferDescriptor<Ubo, MAX_FRAMES>(
        std::array<Handle<Buffer<Ubo>>, MAX_FRAMES> buffers, 
        VkShaderStageFlags stageFlags,
        bool offsetByFrame = false
    )
    : Descriptor(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, stageFlags), buffers_(std::move(buffers)) {
        for (uint frameIdx = 0; frameIdx < MAX_FRAMES; ++frameIdx) {
            bufferInfos_[frameIdx].buffer = buffers_[frameIdx] ? buffers_[frameIdx]->getBuffer() : VK_NULL_HANDLE;
            bufferInfos_[frameIdx].offset = offsetByFrame? frameIdx * sizeof(Ubo) : 0;
            bufferInfos_[frameIdx].range = sizeof(Ubo);
        }
    }
    
    VkDescriptorBufferInfo* getBufferInfo(const uint32_t frameIndex) override {
        return &bufferInfos_.at(frameIndex);
    }
    
    VkDescriptorImageInfo* getImageInfo(const uint32_t frameIndex) override {
        return nullptr;
    }

    void bindBuffer(const uint32_t frameIndex, Handle<Buffer<Ubo>> buffer) {
        bufferInfos_[frameIndex].buffer = buffer->getBuffer();
        buffers_[frameIndex] = std::move(buffer);
    }
private:
    // Keeps the referenced buffers alive for as long as this descriptor
    std::array<Handle<Buffer<Ubo>>, MAX_FRAMES> buffers_;
    std::array<VkDescriptorBufferInfo, MAX_FRAMES> bufferInfos_;
};

// Abstract image desriptor
template<uint MAX_FRAMES>
class ImageDescriptor : public Descriptor {
public:
    virtual ~ImageDescriptor() = default;
    
    VkDescriptorImageInfo* getImageInfo(const uint32_t frameIndex) override {
        return &imageInfos_.at(frameIndex);
    }
    
    VkDescriptorBufferInfo* getBufferInfo(const uint32_t frameIndex) override {
        return nullptr;
    }
protected:
    // layout is the layout the images will be in whenever the descriptor is used
    ImageDescriptor<MAX_FRAMES>(VkDescriptorType type,
                                VkShaderStageFlags stageFlags,
                                std::array<Handle<Image>, MAX_FRAMES> images,
                                VkImageLayout layout)
    : Descriptor(type, stageFlags), images_(std::move(images)) {
        for (uint frameIdx = 0; frameIdx < MAX_FRAMES; ++frameIdx) {
            imageInfos_[frameIdx].imageLayout = layout;
            imageInfos_[frameIdx].imageView = images_[frameIdx]->getImageView();
        }
    }

    // Keeps the referenced images alive for as long as this descriptor
    std::array<Handle<Image>, MAX_FRAMES> images_;
    std::array<VkDescriptorImageInfo, MAX_FRAMES> imageInfos_;
};

// Storage Image Descriptor
// Storage images must be in VK_IMAGE_LAYOUT_GENERAL (or SHARED_PRESENT_KHR) when used
template<uint MAX_FRAMES>
class StorageImageDescriptor : public ImageDescriptor<MAX_FRAMES> {
public:
    StorageImageDescriptor<MAX_FRAMES>(VkShaderStageFlags stageFlags,
                                       std::array<Handle<Image>, MAX_FRAMES> images,
                                       VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL)
    : ImageDescriptor<MAX_FRAMES>(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, stageFlags, std::move(images), layout) {}
};

// Combined Image Sampler Descriptor
template<uint MAX_FRAMES>
class CombinedImageSamplerDescriptor : public ImageDescriptor<MAX_FRAMES> {
public:
    CombinedImageSamplerDescriptor<MAX_FRAMES>(VkShaderStageFlags stageFlags,
                                               std::array<Handle<Image>, MAX_FRAMES> images,
                                               Handle<VulkanSampler> sampler,
                                               VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    : ImageDescriptor<MAX_FRAMES>(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, stageFlags, std::move(images), layout),
    sampler_(std::move(sampler)) {
        for (uint frameIdx = 0; frameIdx < MAX_FRAMES; ++frameIdx) {
            ImageDescriptor<MAX_FRAMES>::imageInfos_[frameIdx].sampler = **sampler_;
        }
    }
private:
    // Keeps the referenced sampler alive for as long as this descriptor
    Handle<VulkanSampler> sampler_;
};
