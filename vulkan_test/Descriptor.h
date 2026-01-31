#pragma once

#include "VkTypes.h"

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

// Descriptor for uniform buffers
template<typename Ubo, uint MAX_FRAMES>
class UniformBufferDescriptor : public Descriptor {
public:
    UniformBufferDescriptor<Ubo, MAX_FRAMES>(std::array<VkBuffer, MAX_FRAMES> buffers, VkShaderStageFlags stageFlags)
    : Descriptor(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, stageFlags) {
        for (uint frameIdx = 0; frameIdx < MAX_FRAMES; ++frameIdx) {
            bufferInfos_[frameIdx].buffer = buffers[frameIdx];
            bufferInfos_[frameIdx].offset = 0;
            bufferInfos_[frameIdx].range = sizeof(Ubo);
        }
    }
    
    VkDescriptorBufferInfo* getBufferInfo(const uint32_t frameIndex) override {
        return &bufferInfos_.at(frameIndex);
    }
    
    VkDescriptorImageInfo* getImageInfo(const uint32_t frameIndex) override {
        return nullptr;
    }

    void bindBuffer(const uint32_t frameIndex, VkBuffer buffer) {
        bufferInfos_[frameIndex].buffer = buffer;
    }
private:
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
    ImageDescriptor<MAX_FRAMES>(VkDescriptorType type,
                                VkShaderStageFlags stageFlags,
                                std::array<VkImageView, MAX_FRAMES> imageViews)
    : Descriptor(type, stageFlags) {
        for (uint frameIdx = 0; frameIdx < MAX_FRAMES; ++frameIdx) {
            imageInfos_[frameIdx].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfos_[frameIdx].imageView = imageViews[frameIdx];
        }
    }

    std::array<VkDescriptorImageInfo, MAX_FRAMES> imageInfos_;
};

// Store Image Descriptor
template<uint MAX_FRAMES>
class StorageImageDescriptor : public ImageDescriptor<MAX_FRAMES> {
public:
    StorageImageDescriptor<MAX_FRAMES>(VkShaderStageFlags stageFlags,
                                       std::array<VkImageView, MAX_FRAMES> imageViews)
    : ImageDescriptor<MAX_FRAMES>(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, stageFlags, imageViews) {}
};

// Combined Image Sampler Descriptor
template<uint MAX_FRAMES>
class CombinedImageSamplerDescriptor : public ImageDescriptor<MAX_FRAMES> {
public:
    CombinedImageSamplerDescriptor<MAX_FRAMES>(VkShaderStageFlags stageFlags,
                                               std::array<VkImageView, MAX_FRAMES> imageViews,
                                               VkSampler sampler)
    : ImageDescriptor<MAX_FRAMES>(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, stageFlags, imageViews) {
        for (uint frameIdx = 0; frameIdx < MAX_FRAMES; ++frameIdx) {
            ImageDescriptor<MAX_FRAMES>::imageInfos_[frameIdx].sampler = sampler;
        }
    }
};
