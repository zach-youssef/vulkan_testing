#pragma once

#include "core/Result.h"
#include "core/VkTypes.h"

#include <algorithm>
#include <array>
#include <cstdint>

/*
 The Vulkan objects of the universal ("bindless") descriptor set: its layout, pool and set, and the
 pipeline layout every pipeline uses. The VulkanContext creates them along with the device; the
 DescriptorHeap (DescriptorHeap.h) hands out their slots. See DescriptorHeap.h for the bindings.
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

// How many descriptors of each type the universal set holds. Clamped to the device's limits.
struct DescriptorCapacities {
    uint32_t sampledImages = 4096;
    uint32_t samplers = 256;
    uint32_t storageImages = 1024;
    uint32_t storageBuffers = 4096;
};

class UniversalSet {
public:
    // Bytes of push constants every pipeline has (the minimum every device supports)
    static constexpr uint32_t kPushConstantSize = 128;

    static Result<UniversalSet> create(VkDevice device, VkPhysicalDevice physicalDevice, DescriptorCapacities requested) {
        UniversalSet set;
        set.clampCapacities(physicalDevice, requested);
        TRY(set.createLayout(device).withContext("creating the universal descriptor set"));
        TRY(set.createSet(device).withContext("creating the universal descriptor set"));
        TRY(set.createPipelineLayout(device).withContext("creating the universal descriptor set"));
        return set;
    }

    // Number of descriptors of a type (after clamping to the device's limits)
    uint32_t capacity(DescriptorType type) const { return capacities_[static_cast<uint32_t>(type)]; }

    VkDescriptorSetLayout setLayout() const { return **setLayout_; }
    VkDescriptorSet set() const { return set_; }
    VkPipelineLayout pipelineLayout() const { return **pipelineLayout_; }

private:
    static constexpr VkDescriptorBindingFlags kBindingFlags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT
                                                              | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT
                                                              | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;

    void clampCapacities(VkPhysicalDevice physicalDevice, DescriptorCapacities requested) {
        VkPhysicalDeviceVulkan12Properties limits{};
        limits.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
        VkPhysicalDeviceProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties.pNext = &limits;
        vkGetPhysicalDeviceProperties2(physicalDevice, &properties);

        capacities_ = {
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
        for (uint32_t count : capacities_) total += count;
        if (total > limits.maxPerStageUpdateAfterBindResources) {
            const double scale = static_cast<double>(limits.maxPerStageUpdateAfterBindResources) / total;
            for (auto& count : capacities_) {
                count = static_cast<uint32_t>(count * scale);
            }
        }
    }

    Result<void> createLayout(VkDevice device) {
        std::array<VkDescriptorSetLayoutBinding, kDescriptorTypeCount> bindings{};
        std::array<VkDescriptorBindingFlags, kDescriptorTypeCount> flags{};
        for (uint32_t type = 0; type < kDescriptorTypeCount; ++type) {
            bindings[type].binding = type;
            bindings[type].descriptorType = toVkDescriptorType(static_cast<DescriptorType>(type));
            bindings[type].descriptorCount = capacities_[type];
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
        VK_TRY(VulkanDescriptorSetLayout::create(setLayout_, device, layoutInfo),
               "Failed to create the universal descriptor set layout");
        return {};
    }

    Result<void> createSet(VkDevice device) {
        std::array<VkDescriptorPoolSize, kDescriptorTypeCount> sizes{};
        for (uint32_t type = 0; type < kDescriptorTypeCount; ++type) {
            sizes[type] = {toVkDescriptorType(static_cast<DescriptorType>(type)), std::max(1u, capacities_[type])};
        }
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = kDescriptorTypeCount;
        poolInfo.pPoolSizes = sizes.data();
        VK_TRY(VulkanDescriptorPool::create(pool_, device, poolInfo), "Failed to create the universal descriptor pool");

        VkDescriptorSetLayout layout = **setLayout_;
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = **pool_;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &layout;
        VK_TRY(vkAllocateDescriptorSets(device, &allocInfo, &set_), "Failed to allocate the universal descriptor set");
        return {};
    }

    Result<void> createPipelineLayout(VkDevice device) {
        VkPushConstantRange pushConstants{VK_SHADER_STAGE_ALL, 0, kPushConstantSize};
        VkDescriptorSetLayout layout = **setLayout_;
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &layout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushConstants;
        VK_TRY(VulkanPipelineLayout::create(pipelineLayout_, device, layoutInfo),
               "Failed to create the universal pipeline layout");
        return {};
    }

    std::array<uint32_t, kDescriptorTypeCount> capacities_{};
    std::unique_ptr<VulkanDescriptorSetLayout> setLayout_;
    std::unique_ptr<VulkanDescriptorPool> pool_;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    std::unique_ptr<VulkanPipelineLayout> pipelineLayout_;
};
