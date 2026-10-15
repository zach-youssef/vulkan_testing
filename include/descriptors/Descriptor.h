#pragma once

#include "core/VkTypes.h"
#include "core/FrameResources.h"
#include "core/ResourceManager.h"
#include "resources/Buffer.h"
#include "resources/Image.h"

#include <optional>

/*
 Descriptors describe one binding of a material's descriptor set. The resources they reference are
 given as FrameBindings: either one resource shared by every frame in flight (for static data such
 as a loaded texture) or a PerFrameHandles collection with a resource per frame (for data written
 every frame, such as uniform buffers). Descriptors keep the resources they reference alive.
 */

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

    // What to write for a frame. Empty if this is not a buffer (or image) descriptor. Resources
    // that are not bound yet are VK_NULL_HANDLE.
    virtual std::optional<VkDescriptorBufferInfo> getBufferInfo(uint32_t frameIndex) = 0;
    virtual std::optional<VkDescriptorImageInfo> getImageInfo(uint32_t frameIndex) = 0;

protected:
    Descriptor(VkDescriptorType type, VkShaderStageFlags stageFlags): type_(type), stageFlags_(stageFlags) {}
private:
    VkDescriptorType type_;
    VkShaderStageFlags stageFlags_;
};

// Generic buffer descriptor, covering size elements of Data from offset bytes into each buffer
template<typename Data>
class BufferDescriptor : public Descriptor {
public:
    BufferDescriptor(VkDescriptorType type, VkShaderStageFlags stageFlags,
                     FrameBinding<Buffer<Data>> buffers, int size, int offset = 0)
    : Descriptor(type, stageFlags), buffers_(std::move(buffers)), size_(size), offset_(offset) {}

    std::optional<VkDescriptorBufferInfo> getBufferInfo(uint32_t frameIndex) override {
        auto buffer = buffers_.get(frameIndex);
        VkDescriptorBufferInfo info{};
        info.buffer = buffer ? buffer->getBuffer() : VK_NULL_HANDLE;
        info.offset = offset_;
        info.range = sizeof(Data) * size_;
        return info;
    }

    std::optional<VkDescriptorImageInfo> getImageInfo(uint32_t) override {
        return std::nullopt;
    }

    // Binds new buffers, each holding size elements. Materials must re-populate their
    // descriptor sets afterwards.
    void bind(FrameBinding<Buffer<Data>> buffers, int size) {
        buffers_ = std::move(buffers);
        size_ = size;
    }

private:
    FrameBinding<Buffer<Data>> buffers_;
    int size_;
    int offset_;
};

// Descriptor for uniform buffers
template<typename Ubo>
class UniformBufferDescriptor : public Descriptor {
public:
    // With offsetByFrame, frame i reads the i-th Ubo of its buffer, so a single buffer holding a
    // Ubo per frame can be shared by every frame
    UniformBufferDescriptor(FrameBinding<Buffer<Ubo>> buffers,
                            VkShaderStageFlags stageFlags,
                            bool offsetByFrame = false)
    : Descriptor(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, stageFlags),
    buffers_(std::move(buffers)),
    offsetByFrame_(offsetByFrame) {}

    std::optional<VkDescriptorBufferInfo> getBufferInfo(uint32_t frameIndex) override {
        auto buffer = buffers_.get(frameIndex);
        VkDescriptorBufferInfo info{};
        info.buffer = buffer ? buffer->getBuffer() : VK_NULL_HANDLE;
        info.offset = offsetByFrame_ ? frameIndex * sizeof(Ubo) : 0;
        info.range = sizeof(Ubo);
        return info;
    }

    std::optional<VkDescriptorImageInfo> getImageInfo(uint32_t) override {
        return std::nullopt;
    }

    // Binds new buffers. Materials must re-populate their descriptor sets afterwards.
    void bind(FrameBinding<Buffer<Ubo>> buffers) {
        buffers_ = std::move(buffers);
    }

private:
    FrameBinding<Buffer<Ubo>> buffers_;
    bool offsetByFrame_;
};

// Abstract image desriptor
class ImageDescriptor : public Descriptor {
public:
    virtual ~ImageDescriptor() = default;

    std::optional<VkDescriptorImageInfo> getImageInfo(uint32_t frameIndex) override {
        auto image = images_.get(frameIndex);
        VkDescriptorImageInfo info{};
        info.imageLayout = layout_;
        info.imageView = image ? image->getImageView() : VK_NULL_HANDLE;
        info.sampler = getSampler();
        return info;
    }

    std::optional<VkDescriptorBufferInfo> getBufferInfo(uint32_t) override {
        return std::nullopt;
    }

protected:
    // layout is the layout the images will be in whenever the descriptor is used
    ImageDescriptor(VkDescriptorType type,
                    VkShaderStageFlags stageFlags,
                    FrameBinding<Image> images,
                    VkImageLayout layout)
    : Descriptor(type, stageFlags), images_(std::move(images)), layout_(layout) {}

    virtual VkSampler getSampler() {
        return VK_NULL_HANDLE;
    }

private:
    FrameBinding<Image> images_;
    VkImageLayout layout_;
};

// Storage Image Descriptor
// Storage images must be in VK_IMAGE_LAYOUT_GENERAL (or SHARED_PRESENT_KHR) when used
class StorageImageDescriptor : public ImageDescriptor {
public:
    StorageImageDescriptor(VkShaderStageFlags stageFlags,
                           FrameBinding<Image> images,
                           VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL)
    : ImageDescriptor(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, stageFlags, std::move(images), layout) {}
};

// Combined Image Sampler Descriptor
class CombinedImageSamplerDescriptor : public ImageDescriptor {
public:
    CombinedImageSamplerDescriptor(VkShaderStageFlags stageFlags,
                                   FrameBinding<Image> images,
                                   Handle<VulkanSampler> sampler,
                                   VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    : ImageDescriptor(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, stageFlags, std::move(images), layout),
    sampler_(std::move(sampler)) {}

protected:
    VkSampler getSampler() override {
        return **sampler_;
    }

private:
    // Keeps the referenced sampler alive for as long as this descriptor
    Handle<VulkanSampler> sampler_;
};
