#pragma once

#include "core/VkTypes.h"
#include "core/VkUtil.h"
#include "core/VulkanContext.h"
#include "resources/Buffer.h"
#include "resources/Image.h"

#include <algorithm>
#include <any>
#include <array>
#include <mutex>
#include <optional>
#include <vector>

/*
 The universal ("bindless") descriptor set.

 The context owns a single descriptor set (VulkanContext::descriptors()) holding large arrays of
 every kind of descriptor the wrapper uses, and a single pipeline layout: that set plus 128 bytes of
 push constants. Every pipeline uses that layout, so the set is bound once per command buffer and
 never changes between draws or dispatches. Shaders find their resources by index: materials push
 the indices of their DescriptorEntries as push constants.

    binding 0: sampled images    texture2D bindlessTextures[]
    binding 1: samplers          sampler bindlessSamplers[]
    binding 2: storage images    image2D bindlessStorageImages[] (no format; see bindless.glsl)
    binding 3: storage buffers   buffer blocks, declared per type (BINDLESS_BUFFER in bindless.glsl)

 Uniform data lives in storage buffers: update-after-bind for uniform buffers isn't widely
 supported.

 A DescriptorEntry owns a slot in one of the arrays and keeps the resource it describes alive.
 Slots are written once, when the entry is created (the bindings are update-after-bind, partially
 bound and update-unused-while-pending, so entries can be created while frames are in flight). When
 an entry is destroyed its slot is reused only once the work submitted before then has finished
 (VulkanContext::retire).

 Thread safe.
 */

enum class DescriptorType : uint32_t {
    SampledImage = 0,
    Sampler = 1,
    StorageImage = 2,
    StorageBuffer = 3,
};

constexpr uint32_t kDescriptorTypeCount = 4;

inline VkDescriptorType toVkDescriptorType(DescriptorType type) {
    switch (type) {
        case DescriptorType::SampledImage: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case DescriptorType::Sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
        case DescriptorType::StorageImage: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case DescriptorType::StorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    return VK_DESCRIPTOR_TYPE_MAX_ENUM;
}

inline const char* descriptorTypeName(DescriptorType type) {
    switch (type) {
        case DescriptorType::SampledImage: return "sampled image";
        case DescriptorType::Sampler: return "sampler";
        case DescriptorType::StorageImage: return "storage image";
        case DescriptorType::StorageBuffer: return "storage buffer";
    }
    return "?";
}

// Hands out indices in [0, capacity), reusing freed ones (most recently freed first). Not thread safe.
class SlotAllocator {
public:
    explicit SlotAllocator(uint32_t capacity = 0) : capacity_(capacity) {}

    std::optional<uint32_t> allocate() {
        if (!free_.empty()) {
            uint32_t slot = free_.back();
            free_.pop_back();
            return slot;
        }
        if (next_ < capacity_) {
            return next_++;
        }
        return std::nullopt;
    }

    void free(uint32_t slot) {
        free_.push_back(slot);
    }

    uint32_t capacity() const { return capacity_; }
    uint32_t used() const { return next_ - static_cast<uint32_t>(free_.size()); }

private:
    uint32_t capacity_;
    uint32_t next_ = 0;
    std::vector<uint32_t> free_;
};

class DescriptorHeap;

// A slot in the universal descriptor set, describing a resource it keeps alive. Created by the
// DescriptorHeap's factories; the slot is freed (after in-flight work) when the entry is destroyed.
class DescriptorEntry {
public:
    ~DescriptorEntry();

    DescriptorEntry(const DescriptorEntry&) = delete;
    DescriptorEntry& operator=(const DescriptorEntry&) = delete;

    DescriptorType type() const { return type_; }

    // The index shaders use to find the resource in its array
    uint32_t index() const { return index_; }

    // Images: the image and the layout it must be in when shaders use it
    const Handle<Image>& image() const { return image_; }
    VkImageLayout imageLayout() const { return imageLayout_; }

    const Handle<VulkanSampler>& sampler() const { return sampler_; }

    // Storage buffers: the buffer, and an opaque handle keeping it alive (a Handle<Buffer<T>>)
    VkBuffer buffer() const { return buffer_; }
    const std::any& bufferHandle() const { return bufferHandle_; }

private:
    friend class DescriptorHeap;
    DescriptorEntry(DescriptorHeap& heap, DescriptorType type, uint32_t index)
    : heap_(&heap), type_(type), index_(index) {}

    DescriptorHeap* heap_;
    DescriptorType type_;
    uint32_t index_;
    Handle<Image> image_;
    VkImageLayout imageLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    Handle<VulkanSampler> sampler_;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    std::any bufferHandle_;
};

class DescriptorHeap {
public:
    // Bytes of push constants every pipeline has (the minimum every device supports)
    static constexpr uint32_t kPushConstantSize = 128;

    DescriptorHeap(VulkanContext& context, DescriptorCapacities requested) : context_(&context) {
        clampCapacities(requested);
        createLayout();
        createSet();
        createPipelineLayout();
    }

    DescriptorHeap(const DescriptorHeap&) = delete;
    DescriptorHeap& operator=(const DescriptorHeap&) = delete;

    VkDescriptorSetLayout setLayout() { return **setLayout_; }
    VkDescriptorSet set() { return set_; }

    // The pipeline layout every pipeline uses: the universal set and kPushConstantSize bytes of
    // push constants visible to all stages
    VkPipelineLayout pipelineLayout() { return **pipelineLayout_; }

    // Binds the universal set for a bind point (once per command buffer is enough)
    void bind(VkCommandBuffer commandBuffer, VkPipelineBindPoint bindPoint) {
        vkCmdBindDescriptorSets(commandBuffer, bindPoint, **pipelineLayout_, 0, 1, &set_, 0, nullptr);
    }

    // An image shaders sample, which must be in `layout` when they do
    Handle<DescriptorEntry> sampledImage(Handle<Image> image,
                                         VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        requireUsage(*image, VK_IMAGE_USAGE_SAMPLED_BIT, "sampled");
        auto entry = allocate(DescriptorType::SampledImage);
        VkDescriptorImageInfo info{VK_NULL_HANDLE, image->getImageView(), layout};
        write(*entry, &info, nullptr);
        entry->image_ = std::move(image);
        entry->imageLayout_ = layout;
        return entry;
    }

    // An image shaders load from or store to; storage images are used in the GENERAL layout
    Handle<DescriptorEntry> storageImage(Handle<Image> image) {
        requireUsage(*image, VK_IMAGE_USAGE_STORAGE_BIT, "storage");
        auto entry = allocate(DescriptorType::StorageImage);
        VkDescriptorImageInfo info{VK_NULL_HANDLE, image->getImageView(), VK_IMAGE_LAYOUT_GENERAL};
        write(*entry, &info, nullptr);
        entry->image_ = std::move(image);
        entry->imageLayout_ = VK_IMAGE_LAYOUT_GENERAL;
        return entry;
    }

    Handle<DescriptorEntry> sampler(Handle<VulkanSampler> sampler) {
        auto entry = allocate(DescriptorType::Sampler);
        VkDescriptorImageInfo info{**sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        write(*entry, &info, nullptr);
        entry->sampler_ = std::move(sampler);
        return entry;
    }

    // A buffer shaders read or write as a storage buffer (including uniform data)
    template<typename T>
    Handle<DescriptorEntry> storageBuffer(Handle<Buffer<T>> buffer, VkDeviceSize offset = 0,
                                          VkDeviceSize range = VK_WHOLE_SIZE) {
        auto entry = allocate(DescriptorType::StorageBuffer);
        VkDescriptorBufferInfo info{buffer->getBuffer(), offset, range};
        write(*entry, nullptr, &info);
        entry->buffer_ = buffer->getBuffer();
        entry->bufferHandle_ = std::move(buffer);
        return entry;
    }

    uint32_t capacity(DescriptorType type) {
        std::lock_guard<std::mutex> lock(mutex_);
        return slots_[static_cast<uint32_t>(type)].capacity();
    }

    // Slots in use (entries alive, or freed but waiting for in-flight work)
    uint32_t used(DescriptorType type) {
        std::lock_guard<std::mutex> lock(mutex_);
        return slots_[static_cast<uint32_t>(type)].used();
    }

private:
    friend class DescriptorEntry;

    static constexpr VkDescriptorBindingFlags kBindingFlags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT
                                                              | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT
                                                              | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;

    void clampCapacities(DescriptorCapacities requested) {
        VkPhysicalDeviceVulkan12Properties limits{};
        limits.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
        VkPhysicalDeviceProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties.pNext = &limits;
        vkGetPhysicalDeviceProperties2(context_->physicalDevice(), &properties);

        std::array<uint32_t, kDescriptorTypeCount> capacity{
            std::min({requested.sampledImages, limits.maxDescriptorSetUpdateAfterBindSampledImages,
                      limits.maxPerStageDescriptorUpdateAfterBindSampledImages}),
            std::min({requested.samplers, limits.maxDescriptorSetUpdateAfterBindSamplers,
                      limits.maxPerStageDescriptorUpdateAfterBindSamplers}),
            std::min({requested.storageImages, limits.maxDescriptorSetUpdateAfterBindStorageImages,
                      limits.maxPerStageDescriptorUpdateAfterBindStorageImages}),
            std::min({requested.storageBuffers, limits.maxDescriptorSetUpdateAfterBindStorageBuffers,
                      limits.maxPerStageDescriptorUpdateAfterBindStorageBuffers}),
        };
        // Every stage sees the whole set, so its total must fit the per-stage resource limit
        uint64_t total = 0;
        for (uint32_t count : capacity) total += count;
        if (total > limits.maxPerStageUpdateAfterBindResources) {
            const double scale = static_cast<double>(limits.maxPerStageUpdateAfterBindResources) / total;
            for (auto& count : capacity) {
                count = static_cast<uint32_t>(count * scale);
            }
        }
        for (uint32_t type = 0; type < kDescriptorTypeCount; ++type) {
            slots_[type] = SlotAllocator(capacity[type]);
        }
    }

    void createLayout() {
        std::array<VkDescriptorSetLayoutBinding, kDescriptorTypeCount> bindings{};
        std::array<VkDescriptorBindingFlags, kDescriptorTypeCount> flags{};
        for (uint32_t type = 0; type < kDescriptorTypeCount; ++type) {
            bindings[type].binding = type;
            bindings[type].descriptorType = toVkDescriptorType(static_cast<DescriptorType>(type));
            bindings[type].descriptorCount = slots_[type].capacity();
            bindings[type].stageFlags = VK_SHADER_STAGE_ALL;
            flags[type] = kBindingFlags;
        }
        VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
        flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        flagsInfo.bindingCount = kDescriptorTypeCount;
        flagsInfo.pBindingFlags = flags.data();

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.pNext = &flagsInfo;
        layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutInfo.bindingCount = kDescriptorTypeCount;
        layoutInfo.pBindings = bindings.data();
        VK_SUCCESS_OR_THROW(VulkanDescriptorSetLayout::create(setLayout_, context_->device(), layoutInfo),
                            "Failed to create the universal descriptor set layout");
    }

    void createSet() {
        std::array<VkDescriptorPoolSize, kDescriptorTypeCount> sizes{};
        for (uint32_t type = 0; type < kDescriptorTypeCount; ++type) {
            sizes[type] = {toVkDescriptorType(static_cast<DescriptorType>(type)), std::max(1u, slots_[type].capacity())};
        }
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = kDescriptorTypeCount;
        poolInfo.pPoolSizes = sizes.data();
        VK_SUCCESS_OR_THROW(VulkanDescriptorPool::create(pool_, context_->device(), poolInfo),
                            "Failed to create the universal descriptor pool");

        VkDescriptorSetLayout layout = **setLayout_;
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = **pool_;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &layout;
        VK_SUCCESS_OR_THROW(vkAllocateDescriptorSets(context_->device(), &allocInfo, &set_),
                            "Failed to allocate the universal descriptor set");
    }

    void createPipelineLayout() {
        VkPushConstantRange pushConstants{VK_SHADER_STAGE_ALL, 0, kPushConstantSize};
        VkDescriptorSetLayout layout = **setLayout_;
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &layout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushConstants;
        VK_SUCCESS_OR_THROW(VulkanPipelineLayout::create(pipelineLayout_, context_->device(), layoutInfo),
                            "Failed to create the universal pipeline layout");
    }

    static void requireUsage(Image& image, VkImageUsageFlags usage, const char* what) {
        if ((image.getUsage() & usage) != usage) {
            throw std::invalid_argument(std::string("A ") + what + " image descriptor needs an image created with "
                                        + string_VkImageUsageFlags(usage));
        }
    }

    Handle<DescriptorEntry> allocate(DescriptorType type) {
        std::optional<uint32_t> slot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            slot = slots_[static_cast<uint32_t>(type)].allocate();
        }
        if (!slot) {
            // Freed slots may be waiting for in-flight work
            context_->collectRetired();
            std::lock_guard<std::mutex> lock(mutex_);
            slot = slots_[static_cast<uint32_t>(type)].allocate();
        }
        if (!slot) {
            throw std::runtime_error(std::string("The universal descriptor set has no free ") + descriptorTypeName(type)
                                     + " slots (see VulkanContextConfig::descriptorCapacities)");
        }
        return context_->resources().adopt(std::unique_ptr<DescriptorEntry>(new DescriptorEntry(*this, type, *slot)));
    }

    void write(const DescriptorEntry& entry, const VkDescriptorImageInfo* image, const VkDescriptorBufferInfo* buffer) {
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set_;
        write.dstBinding = static_cast<uint32_t>(entry.type());
        write.dstArrayElement = entry.index();
        write.descriptorCount = 1;
        write.descriptorType = toVkDescriptorType(entry.type());
        write.pImageInfo = image;
        write.pBufferInfo = buffer;
        // Descriptor set updates are externally synchronized
        std::lock_guard<std::mutex> lock(mutex_);
        vkUpdateDescriptorSets(context_->device(), 1, &write, 0, nullptr);
    }

    // Frees a slot once the work submitted so far (which may use it) has finished
    void release(DescriptorType type, uint32_t index) {
        context_->retire([this, type, index] {
            std::lock_guard<std::mutex> lock(mutex_);
            slots_[static_cast<uint32_t>(type)].free(index);
        });
    }

    VulkanContext* context_;
    std::mutex mutex_;
    std::array<SlotAllocator, kDescriptorTypeCount> slots_;
    std::unique_ptr<VulkanDescriptorSetLayout> setLayout_;
    std::unique_ptr<VulkanDescriptorPool> pool_;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    std::unique_ptr<VulkanPipelineLayout> pipelineLayout_;
};

inline DescriptorEntry::~DescriptorEntry() {
    heap_->release(type_, index_);
}

// VulkanContext::descriptors, defined here since VulkanContext.h cannot depend on DescriptorHeap
inline DescriptorHeap& VulkanContext::descriptors() {
    std::call_once(descriptorsCreated_, [this] {
        descriptors_ = std::unique_ptr<DescriptorHeap, void (*)(DescriptorHeap*)>(
            new DescriptorHeap(*this, config_.descriptorCapacities), [](DescriptorHeap* heap) { delete heap; });
    });
    return *descriptors_;
}
