#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <vector>

/*
 Device features, as a VkPhysicalDeviceFeatures2 chain of the core, Vulkan 1.1, 1.2 and 1.3
 feature structures, and extensions' feature structures (VK_EXT_shader_atomic_float's), which are in
 the chain only when one of their features is enabled, and whose extensions() the device then
 enables too.

 Vulkan 1.3 is the minimum version. Beyond it, the wrapper relies on (DeviceFeatures::required()):
    - samplerAnisotropy: samplers are always created with anisotropic filtering
    - timelineSemaphore: per-queue timelines synchronize submissions and frames
    - synchronization2: barriers and submissions use the synchronization2 structures
    - dynamicRendering: render targets are begun with vkCmdBeginRendering, without render passes
    - descriptor indexing (runtime arrays, partially bound, update after bind and update unused
      while pending for sampled images, storage images and storage buffers): the universal
      descriptor set (see DescriptorHeap)
    - shaderStorageImageReadWithoutFormat / WriteWithoutFormat: the universal set's storage images
      are declared without a format
    - shaderDemoteToHelperInvocation: glslc compiles discard for Vulkan 1.3 as a demotion (every
      Vulkan 1.3 device supports it)

 Applications needing more (e.g. 8- and 16-bit storage) set them in VulkanContextConfig::features
 (AppConfig::features). Devices lacking any of them aren't used, and the device is created with them
 enabled as well as the wrapper's.

 The wrapper also enables features it can do without when the device has them
 (DeviceFeatures::optional()), and code using them checks VulkanContext::enabledFeatures():
    - drawIndirectCount: draws whose count a buffer holds (IndirectDrawNode)
 */

// The names of each feature structure's VkBool32 members, in declaration order. Generated from
// vulkan_core.h; feature structures never change once released.
namespace device_features_detail {

inline constexpr const char* kCoreFeatureNames[] = {
    "robustBufferAccess", "fullDrawIndexUint32", "imageCubeArray", "independentBlend", "geometryShader",
    "tessellationShader", "sampleRateShading", "dualSrcBlend", "logicOp", "multiDrawIndirect",
    "drawIndirectFirstInstance", "depthClamp", "depthBiasClamp", "fillModeNonSolid", "depthBounds",
    "wideLines", "largePoints", "alphaToOne", "multiViewport", "samplerAnisotropy", "textureCompressionETC2",
    "textureCompressionASTC_LDR", "textureCompressionBC", "occlusionQueryPrecise", "pipelineStatisticsQuery",
    "vertexPipelineStoresAndAtomics", "fragmentStoresAndAtomics", "shaderTessellationAndGeometryPointSize",
    "shaderImageGatherExtended", "shaderStorageImageExtendedFormats", "shaderStorageImageMultisample",
    "shaderStorageImageReadWithoutFormat", "shaderStorageImageWriteWithoutFormat",
    "shaderUniformBufferArrayDynamicIndexing", "shaderSampledImageArrayDynamicIndexing",
    "shaderStorageBufferArrayDynamicIndexing", "shaderStorageImageArrayDynamicIndexing", "shaderClipDistance",
    "shaderCullDistance", "shaderFloat64", "shaderInt64", "shaderInt16", "shaderResourceResidency",
    "shaderResourceMinLod", "sparseBinding", "sparseResidencyBuffer", "sparseResidencyImage2D",
    "sparseResidencyImage3D", "sparseResidency2Samples", "sparseResidency4Samples", "sparseResidency8Samples",
    "sparseResidency16Samples", "sparseResidencyAliased", "variableMultisampleRate", "inheritedQueries",
};

inline constexpr const char* kVulkan11FeatureNames[] = {
    "storageBuffer16BitAccess", "uniformAndStorageBuffer16BitAccess", "storagePushConstant16",
    "storageInputOutput16", "multiview", "multiviewGeometryShader", "multiviewTessellationShader",
    "variablePointersStorageBuffer", "variablePointers", "protectedMemory", "samplerYcbcrConversion",
    "shaderDrawParameters",
};

inline constexpr const char* kVulkan12FeatureNames[] = {
    "samplerMirrorClampToEdge", "drawIndirectCount", "storageBuffer8BitAccess",
    "uniformAndStorageBuffer8BitAccess", "storagePushConstant8", "shaderBufferInt64Atomics",
    "shaderSharedInt64Atomics", "shaderFloat16", "shaderInt8", "descriptorIndexing",
    "shaderInputAttachmentArrayDynamicIndexing", "shaderUniformTexelBufferArrayDynamicIndexing",
    "shaderStorageTexelBufferArrayDynamicIndexing", "shaderUniformBufferArrayNonUniformIndexing",
    "shaderSampledImageArrayNonUniformIndexing", "shaderStorageBufferArrayNonUniformIndexing",
    "shaderStorageImageArrayNonUniformIndexing", "shaderInputAttachmentArrayNonUniformIndexing",
    "shaderUniformTexelBufferArrayNonUniformIndexing", "shaderStorageTexelBufferArrayNonUniformIndexing",
    "descriptorBindingUniformBufferUpdateAfterBind", "descriptorBindingSampledImageUpdateAfterBind",
    "descriptorBindingStorageImageUpdateAfterBind", "descriptorBindingStorageBufferUpdateAfterBind",
    "descriptorBindingUniformTexelBufferUpdateAfterBind",
    "descriptorBindingStorageTexelBufferUpdateAfterBind", "descriptorBindingUpdateUnusedWhilePending",
    "descriptorBindingPartiallyBound", "descriptorBindingVariableDescriptorCount", "runtimeDescriptorArray",
    "samplerFilterMinmax", "scalarBlockLayout", "imagelessFramebuffer", "uniformBufferStandardLayout",
    "shaderSubgroupExtendedTypes", "separateDepthStencilLayouts", "hostQueryReset", "timelineSemaphore",
    "bufferDeviceAddress", "bufferDeviceAddressCaptureReplay", "bufferDeviceAddressMultiDevice",
    "vulkanMemoryModel", "vulkanMemoryModelDeviceScope", "vulkanMemoryModelAvailabilityVisibilityChains",
    "shaderOutputViewportIndex", "shaderOutputLayer", "subgroupBroadcastDynamicId",
};

inline constexpr const char* kVulkan13FeatureNames[] = {
    "robustImageAccess", "inlineUniformBlock", "descriptorBindingInlineUniformBlockUpdateAfterBind",
    "pipelineCreationCacheControl", "privateData", "shaderDemoteToHelperInvocation",
    "shaderTerminateInvocation", "subgroupSizeControl", "computeFullSubgroups", "synchronization2",
    "textureCompressionASTC_HDR", "shaderZeroInitializeWorkgroupMemory", "dynamicRendering",
    "shaderIntegerDotProduct", "maintenance4",
};

// Extensions' features, enabled with their extension when any is requested
inline constexpr const char* kAtomicFloatExtension = "VK_EXT_shader_atomic_float";
inline constexpr const char* kAtomicFloatFeatureNames[] = {
    "shaderBufferFloat32Atomics", "shaderBufferFloat32AtomicAdd", "shaderBufferFloat64Atomics",
    "shaderBufferFloat64AtomicAdd", "shaderSharedFloat32Atomics", "shaderSharedFloat32AtomicAdd",
    "shaderSharedFloat64Atomics", "shaderSharedFloat64AtomicAdd", "shaderImageFloat32Atomics",
    "shaderImageFloat32AtomicAdd", "sparseImageFloat32Atomics", "sparseImageFloat32AtomicAdd",
};

// Each structure's VkBool32 members are contiguous, from its first feature to its last
#define DEVICE_FEATURES_CHECK_FIELDS(Struct, first, last, names) \
    static_assert(offsetof(Struct, last) - offsetof(Struct, first) == (std::size(names) - 1) * sizeof(VkBool32))
DEVICE_FEATURES_CHECK_FIELDS(VkPhysicalDeviceFeatures, robustBufferAccess, inheritedQueries, kCoreFeatureNames);
DEVICE_FEATURES_CHECK_FIELDS(VkPhysicalDeviceVulkan11Features, storageBuffer16BitAccess, shaderDrawParameters,
                             kVulkan11FeatureNames);
DEVICE_FEATURES_CHECK_FIELDS(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge, subgroupBroadcastDynamicId,
                             kVulkan12FeatureNames);
DEVICE_FEATURES_CHECK_FIELDS(VkPhysicalDeviceVulkan13Features, robustImageAccess, maintenance4, kVulkan13FeatureNames);
DEVICE_FEATURES_CHECK_FIELDS(VkPhysicalDeviceShaderAtomicFloatFeaturesEXT, shaderBufferFloat32Atomics,
                             sparseImageFloat32AtomicAdd, kAtomicFloatFeatureNames);
#undef DEVICE_FEATURES_CHECK_FIELDS

} // namespace device_features_detail

class DeviceFeatures {
public:
    DeviceFeatures() {
        link();
    }

    DeviceFeatures(const DeviceFeatures& other)
    : core(other.core), vulkan11(other.vulkan11), vulkan12(other.vulkan12), vulkan13(other.vulkan13),
      atomicFloat(other.atomicFloat) {
        link();
    }

    DeviceFeatures& operator=(const DeviceFeatures& other) {
        core = other.core;
        vulkan11 = other.vulkan11;
        vulkan12 = other.vulkan12;
        vulkan13 = other.vulkan13;
        atomicFloat = other.atomicFloat;
        link();
        return *this;
    }

    // The features a device supports (an extension's features are all unsupported without the
    // extension)
    static DeviceFeatures supported(VkPhysicalDevice device) {
        DeviceFeatures features;
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> extensions(count);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());
        const bool atomicFloat = std::any_of(extensions.begin(), extensions.end(), [](const VkExtensionProperties& e) {
            return std::string(e.extensionName) == device_features_detail::kAtomicFloatExtension;
        });
        // Extensions' structures may only be chained when the device has the extension
        features.vulkan13.pNext = atomicFloat ? &features.atomicFloat : nullptr;
        vkGetPhysicalDeviceFeatures2(device, &features.core);
        features.link();
        return features;
    }

    // The device extensions the features enabled need
    std::vector<const char*> extensions() const {
        std::vector<const char*> names;
        if (usesAtomicFloat()) {
            names.push_back(device_features_detail::kAtomicFloatExtension);
        }
        return names;
    }

    // The features the wrapper enables (all of which are required)
    static DeviceFeatures required() {
        DeviceFeatures features;
        features.core.features.samplerAnisotropy = VK_TRUE;
        features.core.features.shaderStorageImageReadWithoutFormat = VK_TRUE;
        features.core.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        features.vulkan12.timelineSemaphore = VK_TRUE;
        features.vulkan12.descriptorIndexing = VK_TRUE;
        features.vulkan12.runtimeDescriptorArray = VK_TRUE;
        // Shaders choosing among descriptors per invocation (e.g. through a DescriptorArray)
        features.vulkan12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        features.vulkan12.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
        features.vulkan12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
        features.vulkan12.descriptorBindingPartiallyBound = VK_TRUE;
        features.vulkan12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        features.vulkan12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
        features.vulkan12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
        features.vulkan12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
        features.vulkan13.shaderDemoteToHelperInvocation = VK_TRUE;
        features.vulkan13.synchronization2 = VK_TRUE;
        features.vulkan13.dynamicRendering = VK_TRUE;
        return features;
    }

    // The features the wrapper enables when the device supports them, adapting where it doesn't
    static DeviceFeatures optional() {
        DeviceFeatures features;
        features.vulkan12.drawIndirectCount = VK_TRUE;
        return features;
    }

    // The features enabled in both this set and other
    DeviceFeatures common(const DeviceFeatures& other) const {
        DeviceFeatures features = *this;
        features.forEachField(other, [](VkBool32& mine, VkBool32 theirs, const char*) {
            mine = mine && theirs;
        });
        return features;
    }

    // The wrapper's required features plus extra
    static DeviceFeatures requiredWith(const DeviceFeatures& extra) {
        DeviceFeatures features = required();
        features.add(extra);
        return features;
    }

    // Enables every feature other enables
    DeviceFeatures& add(const DeviceFeatures& other) {
        forEachField(other, [](VkBool32& mine, VkBool32 theirs, const char*) {
            mine = mine || theirs;
        });
        return *this;
    }

    // Names of the features needed enables but this set lacks; empty if it has them all
    std::vector<std::string> missing(const DeviceFeatures& needed) const {
        std::vector<std::string> names;
        DeviceFeatures self = *this;
        self.forEachField(needed, [&names](VkBool32& have, VkBool32 need, const char* name) {
            if (need && !have) {
                names.emplace_back(name);
            }
        });
        return names;
    }

    // Names of the wrapper's required features this set lacks; empty if it has them all
    std::vector<std::string> missingRequired() const {
        return missing(required());
    }

    // The head of the chain, e.g. for VkDeviceCreateInfo::pNext. Extensions' structures are in it
    // only when any of their features is enabled (enable their extensions(), too).
    const VkPhysicalDeviceFeatures2* chain() {
        link();
        return &core;
    }

    VkPhysicalDeviceFeatures2 core{};
    VkPhysicalDeviceVulkan11Features vulkan11{};
    VkPhysicalDeviceVulkan12Features vulkan12{};
    VkPhysicalDeviceVulkan13Features vulkan13{};
    // VK_EXT_shader_atomic_float: atomics on floats in buffers, shared memory and images
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomicFloat{};

private:
    void link() {
        core.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        vulkan11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        vulkan13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        core.pNext = &vulkan11;
        vulkan11.pNext = &vulkan12;
        vulkan12.pNext = &vulkan13;
        atomicFloat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT;
        atomicFloat.pNext = nullptr;
        vulkan13.pNext = usesAtomicFloat() ? &atomicFloat : nullptr;
    }

    bool usesAtomicFloat() const {
        const auto* fields = &atomicFloat.shaderBufferFloat32Atomics;
        return std::any_of(fields, fields + std::size(device_features_detail::kAtomicFloatFeatureNames),
                           [](VkBool32 f) { return f != VK_FALSE; });
    }

    // Calls fn(this set's field, other's field, name) for every feature
    template<typename Fn>
    void forEachField(const DeviceFeatures& other, Fn fn) {
        using namespace device_features_detail;
        auto visit = [&fn](auto& mine, const auto& theirs, size_t offset, const auto& names) {
            auto* mineFields = reinterpret_cast<VkBool32*>(reinterpret_cast<char*>(&mine) + offset);
            auto* theirFields = reinterpret_cast<const VkBool32*>(reinterpret_cast<const char*>(&theirs) + offset);
            for (size_t i = 0; i < std::size(names); ++i) {
                fn(mineFields[i], theirFields[i], names[i]);
            }
        };
        visit(core.features, other.core.features, 0, kCoreFeatureNames);
        visit(vulkan11, other.vulkan11, offsetof(VkPhysicalDeviceVulkan11Features, storageBuffer16BitAccess),
              kVulkan11FeatureNames);
        visit(vulkan12, other.vulkan12, offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge),
              kVulkan12FeatureNames);
        visit(vulkan13, other.vulkan13, offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess),
              kVulkan13FeatureNames);
        visit(atomicFloat, other.atomicFloat,
              offsetof(VkPhysicalDeviceShaderAtomicFloatFeaturesEXT, shaderBufferFloat32Atomics), kAtomicFloatFeatureNames);
    }
};

// Why a device can't be used (an API version, or features of needed it lacks); empty if it can
inline std::vector<std::string> deviceShortcomings(VkPhysicalDevice device,
                                                   const DeviceFeatures& needed = DeviceFeatures::required()) {
    std::vector<std::string> shortcomings;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_3) {
        shortcomings.emplace_back("Vulkan 1.3");
        return shortcomings; // Its features can't be queried through the 1.3 structures
    }
    for (auto& feature : DeviceFeatures::supported(device).missing(needed)) {
        shortcomings.push_back(feature);
    }
    return shortcomings;
}
