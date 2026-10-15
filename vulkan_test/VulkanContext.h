#pragma once

#include "VkTypes.h"
#include "VkUtil.h"
#include "ResourceManager.h"
#include "DeviceSelection.h"

#include <array>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

/*
 Overview of Structure:

 VulkanContext:
    - Owns the Vulkan state shared by the rest of the wrapper: the instance (and optional
      validation messenger), an optional presentation surface, the physical & logical
      device, the device's queues and a ResourceManager
    - Serializes access to each queue (see "Thread safety" below)
    - Hands out CommandPools, and runs one-off commands with submitImmediate()

 Thread safety:
    - Queues are externally synchronized in Vulkan. Submit, present and wait on queues through
      submit() / present() / waitIdle() / waitIdleAll(), which lock the underlying VkQueue.
      Graphics, compute and present may share a VkQueue, so locks are per VkQueue, not per role.
    - createCommandPool() may be called from any thread. Each CommandPool is NOT thread safe; see
      CommandPool below.
    - submitImmediate() may be called from any thread.
    - The ResourceManager is thread safe.

 Lifetime:
    Everything created from the context (CommandPools, managed resources, wrappers holding the
    device) must be destroyed before the context.
 */

enum class QueueType {
    Graphics,
    Compute,
    Present,
};

/*
 A VkCommandPool and the command buffers allocated from it.

 Command pools are externally synchronized in Vulkan: a pool, and every command buffer allocated
 from it, must only be used by one thread at a time. That includes allocating, recording,
 resetting and freeing command buffers. The owner of a CommandPool is responsible for this,
 typically by giving each thread that records commands its own pool.

 See https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html (Command Pools).

 Command buffers allocated from the pool are freed when the pool is destroyed.
 */
class CommandPool {
public:
    CommandPool(VkDevice device, uint32_t queueFamilyIndex, VkCommandPoolCreateFlags flags)
    : device_(device), queueFamilyIndex_(queueFamilyIndex) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = flags;
        poolInfo.queueFamilyIndex = queueFamilyIndex;
        VK_SUCCESS_OR_THROW(VulkanCommandPool::create(pool_, device, poolInfo),
                            "Failed to create command pool");
    }

    CommandPool(const CommandPool&) = delete;
    CommandPool& operator=(const CommandPool&) = delete;

    VkCommandPool get() {
        return **pool_;
    }

    uint32_t getQueueFamilyIndex() {
        return queueFamilyIndex_;
    }

    std::vector<VkCommandBuffer> allocate(uint32_t count, VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
        std::vector<VkCommandBuffer> commandBuffers(count);
        allocateInto(commandBuffers.data(), count, level);
        return commandBuffers;
    }

    template<size_t N>
    std::array<VkCommandBuffer, N> allocateArray(VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
        std::array<VkCommandBuffer, N> commandBuffers{};
        allocateInto(commandBuffers.data(), static_cast<uint32_t>(N), level);
        return commandBuffers;
    }

    VkCommandBuffer allocateOne(VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        allocateInto(&commandBuffer, 1, level);
        return commandBuffer;
    }

    void free(VkCommandBuffer commandBuffer) {
        vkFreeCommandBuffers(device_, **pool_, 1, &commandBuffer);
    }

private:
    void allocateInto(VkCommandBuffer* out, uint32_t count, VkCommandBufferLevel level) {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = **pool_;
        allocInfo.level = level;
        allocInfo.commandBufferCount = count;
        VK_SUCCESS_OR_THROW(vkAllocateCommandBuffers(device_, &allocInfo, out),
                            "Failed to allocate command buffers");
    }

    VkDevice device_;
    uint32_t queueFamilyIndex_;
    std::unique_ptr<VulkanCommandPool> pool_;
};

struct VulkanContextConfig {
    std::string applicationName = "VulkanApp";

    // Instance extensions required by the application (e.g. glfwGetRequiredInstanceExtensions).
    // Portability enumeration and, with validation, debug utils are added automatically.
    std::vector<const char*> instanceExtensions;

    // Enables the Khronos validation layer. If the layer is unavailable a warning is printed
    // and the context is created without it.
    bool enableValidation = false;

    // Receives validation messages (warnings and errors). Defaults to printing to std::cerr.
    // May be called from any thread that calls into Vulkan.
    std::function<void(VkDebugUtilsMessageSeverityFlagBitsEXT, const char*)> onValidationMessage;

    // Creates the surface to present to. Without one the context is headless: no present
    // queue is available and devices are not required to support presentation.
    std::function<VkResult(VkInstance, VkSurfaceKHR*)> createSurface;
};

class VulkanContext {
public:
    explicit VulkanContext(VulkanContextConfig config) : config_(std::move(config)) {
        if (!config_.onValidationMessage) {
            config_.onValidationMessage = [](VkDebugUtilsMessageSeverityFlagBitsEXT severity, const char* message) {
                const char* label = severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning";
                std::cerr << "[Vulkan validation " << label << "] " << message << std::endl;
            };
        }
        createInstance();
        createSurface();
        pickPhysicalDevice();
        createDevice();

        immediatePool_ = createCommandPool(QueueType::Graphics, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT);
        resourceManager_ = std::make_unique<ResourceManager>();
    }

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    ~VulkanContext() {
        waitIdleAll();
        // Destroy in reverse dependency order
        resourceManager_.reset();
        immediatePool_.reset();
        device_.reset();
        surface_.reset();
        if (messenger_ != VK_NULL_HANDLE) {
            auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(**instance_, "vkDestroyDebugUtilsMessengerEXT"));
            destroyMessenger(**instance_, messenger_, nullptr);
        }
        instance_.reset();
    }

public: // Accessors
    VkInstance instance() { return **instance_; }
    VkPhysicalDevice physicalDevice() { return physicalDevice_; }
    VkDevice device() { return **device_; }
    ResourceManager& resources() { return *resourceManager_; }

    bool hasSurface() { return surface_ != nullptr; }
    VkSurfaceKHR surface() {
        assert(hasSurface() && "Context was created without a surface");
        return **surface_;
    }

    bool validationEnabled() { return messenger_ != VK_NULL_HANDLE; }

    std::string deviceName() {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
        return properties.deviceName;
    }

    uint32_t queueFamily(QueueType type) {
        return queueFor(type).familyIndex;
    }

    // The raw queue. Host access to it must be externally synchronized: prefer submit(),
    // present() and waitIdle(), which hold the queue's lock.
    VkQueue queue(QueueType type) {
        return queueFor(type).queue;
    }

public: // Queue access (thread safe)
    VkResult submit(QueueType type, uint32_t submitCount, const VkSubmitInfo* submits, VkFence fence) {
        auto& queue = queueFor(type);
        std::lock_guard<std::mutex> lock(*queue.lock);
        return vkQueueSubmit(queue.queue, submitCount, submits, fence);
    }

    VkResult submit(QueueType type, const VkSubmitInfo& submitInfo, VkFence fence) {
        return submit(type, 1, &submitInfo, fence);
    }

    VkResult present(const VkPresentInfoKHR& presentInfo) {
        auto& queue = queueFor(QueueType::Present);
        std::lock_guard<std::mutex> lock(*queue.lock);
        return vkQueuePresentKHR(queue.queue, &presentInfo);
    }

    VkResult waitIdle(QueueType type) {
        auto& queue = queueFor(type);
        std::lock_guard<std::mutex> lock(*queue.lock);
        return vkQueueWaitIdle(queue.queue);
    }

    // Waits for all queues. vkDeviceWaitIdle requires every queue to be externally synchronized,
    // so this takes all queue locks.
    VkResult waitIdleAll() {
        std::vector<std::unique_lock<std::mutex>> locks;
        for (auto& lock : queueLocks_) {
            locks.emplace_back(*lock); // Always locked in the same order, avoiding deadlocks
        }
        return vkDeviceWaitIdle(**device_);
    }

public: // Command recording
    // Creates a command pool for the given queue's family, owned by the caller. Thread safe.
    // The returned pool is not; see CommandPool.
    std::unique_ptr<CommandPool> createCommandPool(QueueType type,
                                                   VkCommandPoolCreateFlags flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) {
        return std::make_unique<CommandPool>(**device_, queueFamily(type), flags);
    }

    // Records commands with op, submits them to the graphics queue and blocks until they have
    // executed. Intended for one-off work such as uploads, readbacks and layout transitions.
    // Thread safe: uses a command pool private to the context, guarded by a lock.
    void submitImmediate(const std::function<void(VkCommandBuffer)>& op) {
        std::lock_guard<std::mutex> lock(immediateMutex_);

        VkCommandBuffer commandBuffer = immediatePool_->allocateOne();

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commandBuffer, &beginInfo);
        op(commandBuffer);
        vkEndCommandBuffer(commandBuffer);

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        std::unique_ptr<VulkanFence> fence;
        VK_SUCCESS_OR_THROW(VulkanFence::create(fence, **device_, fenceInfo),
                            "Failed to create fence");

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        VK_SUCCESS_OR_THROW(submit(QueueType::Graphics, submitInfo, **fence),
                            "Failed to submit immediate commands");

        // Wait on the fence rather than the queue, so the queue's lock is not held while waiting
        vkWaitForFences(**device_, 1, fence->get(), VK_TRUE, UINT64_MAX);
        immediatePool_->free(commandBuffer);
    }

private:
    struct QueueInfo {
        uint32_t familyIndex = 0;
        VkQueue queue = VK_NULL_HANDLE;
        std::mutex* lock = nullptr;
    };

    QueueInfo& queueFor(QueueType type) {
        auto& queue = queues_[static_cast<size_t>(type)];
        if (queue.queue == VK_NULL_HANDLE) {
            throw std::logic_error("Queue type unavailable (a present queue requires a surface)");
        }
        return queue;
    }

    static bool hasInstanceLayer(const char* name) {
        for (const auto& layer : readVkVector<VkLayerProperties>(vkEnumerateInstanceLayerProperties)) {
            if (strcmp(layer.layerName, name) == 0) {
                return true;
            }
        }
        return false;
    }

    static bool hasInstanceExtension(const char* name) {
        for (const auto& extension :
             readVkVector<VkExtensionProperties, const char*>(vkEnumerateInstanceExtensionProperties)) {
            if (strcmp(extension.extensionName, name) == 0) {
                return true;
            }
        }
        return false;
    }

    static bool hasDeviceExtension(VkPhysicalDevice device, const char* name) {
        for (const auto& extension :
             readVkVector<VkExtensionProperties, VkPhysicalDevice, const char*>(device, vkEnumerateDeviceExtensionProperties)) {
            if (strcmp(extension.extensionName, name) == 0) {
                return true;
            }
        }
        return false;
    }

    void createInstance() {
        const char* validationLayer = "VK_LAYER_KHRONOS_validation";
        bool useValidation = false;
        if (config_.enableValidation) {
            useValidation = hasInstanceLayer(validationLayer) && hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            if (!useValidation) {
                std::cerr << "Validation requested but " << validationLayer
                          << " / " << VK_EXT_DEBUG_UTILS_EXTENSION_NAME << " is unavailable; continuing without it" << std::endl;
            }
        }

        std::vector<const char*> extensions = config_.instanceExtensions;
        std::vector<const char*> layers;
        VkInstanceCreateFlags flags = 0;

        // Required to enumerate portability implementations such as MoltenVK. Their devices
        // require VK_KHR_portability_subset, which in turn depends on
        // VK_KHR_get_physical_device_properties2 (a Vulkan 1.1 core feature) at the instance level.
        if (hasInstanceExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            if (hasInstanceExtension(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
                extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
            }
        }
        if (useValidation) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            layers.push_back(validationLayer);
        }

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = config_.applicationName.c_str();
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "No Engine";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.flags = flags;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
        createInfo.ppEnabledLayerNames = layers.data();

        VK_SUCCESS_OR_THROW(VulkanInstance::create(instance_, createInfo),
                            "Failed to create instance");

        if (useValidation) {
            VkDebugUtilsMessengerCreateInfoEXT messengerInfo{};
            messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                                            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                                        | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                                        | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            messengerInfo.pfnUserCallback = onValidationMessage;
            messengerInfo.pUserData = this;

            auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(**instance_, "vkCreateDebugUtilsMessengerEXT"));
            VK_SUCCESS_OR_THROW(createMessenger(**instance_, &messengerInfo, nullptr, &messenger_),
                                "Failed to create debug messenger");
        }
    }

    void createSurface() {
        if (!config_.createSurface) {
            return;
        }
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VK_SUCCESS_OR_THROW(config_.createSurface(**instance_, &surface),
                            "Failed to create surface");
        surface_ = std::make_unique<VulkanSurface>(**instance_, surface);
    }

    // Finds a family supporting both graphics and compute, so the two never need to synchronize
    // across queue families
    static std::optional<uint32_t> findGraphicsComputeFamily(VkPhysicalDevice device) {
        auto families = readVkVectorVoid<VkQueueFamilyProperties, VkPhysicalDevice>(device, vkGetPhysicalDeviceQueueFamilyProperties);
        for (uint32_t i = 0; i < families.size(); ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                return i;
            }
        }
        return {};
    }

    void pickPhysicalDevice() {
        for (auto device : readVkVector<VkPhysicalDevice, VkInstance>(**instance_, vkEnumeratePhysicalDevices)) {
            if (hasSurface()) {
                if (isDeviceSuitable(device, **surface_)) {
                    auto indices = findQueueFamilies(device, **surface_);
                    physicalDevice_ = device;
                    graphicsFamily_ = indices.graphicsFamily.value();
                    presentFamily_ = indices.presentFamily;
                    return;
                }
            } else if (getSupportedFeatures(device).samplerAnisotropy) {
                if (auto family = findGraphicsComputeFamily(device)) {
                    physicalDevice_ = device;
                    graphicsFamily_ = *family;
                    return;
                }
            }
        }
        throw std::runtime_error("Failed to find a suitable GPU.");
    }

    void createDevice() {
        std::vector<uint32_t> families = {graphicsFamily_};
        if (presentFamily_ && *presentFamily_ != graphicsFamily_) {
            families.push_back(*presentFamily_);
        }

        float queuePriority = 1.0f; // Required even if only 1 queue
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        for (uint32_t family : families) {
            VkDeviceQueueCreateInfo queueInfo{};
            queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo.queueFamilyIndex = family;
            queueInfo.queueCount = 1;
            queueInfo.pQueuePriorities = &queuePriority;
            queueCreateInfos.push_back(queueInfo);
        }

        // Samplers are always created with anisotropic filtering enabled; support for it is
        // required when picking the physical device
        VkPhysicalDeviceFeatures features{};
        features.samplerAnisotropy = VK_TRUE;

        std::vector<const char*> extensions;
        if (hasSurface()) {
            extensions.insert(extensions.end(), deviceExtensions.begin(), deviceExtensions.end());
        }
        // Portability implementations (e.g. MoltenVK) require this extension to be enabled
        if (hasDeviceExtension(physicalDevice_, "VK_KHR_portability_subset")) {
            extensions.push_back("VK_KHR_portability_subset");
        }

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.pEnabledFeatures = &features;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        VK_SUCCESS_OR_THROW(VulkanDevice::create(device_, physicalDevice_, createInfo),
                            "Failed to create logical device");

        // One lock per distinct VkQueue. Compute uses the graphics family's queue.
        auto setQueue = [this](QueueType type, uint32_t family) {
            VkQueue queue = VK_NULL_HANDLE;
            vkGetDeviceQueue(**device_, family, 0, &queue);
            std::mutex* lock = nullptr;
            for (auto& existing : queues_) {
                if (existing.queue == queue) {
                    lock = existing.lock;
                }
            }
            if (!lock) {
                queueLocks_.push_back(std::make_unique<std::mutex>());
                lock = queueLocks_.back().get();
            }
            queues_[static_cast<size_t>(type)] = QueueInfo{family, queue, lock};
        };
        setQueue(QueueType::Graphics, graphicsFamily_);
        setQueue(QueueType::Compute, graphicsFamily_);
        if (presentFamily_) {
            setQueue(QueueType::Present, *presentFamily_);
        }
    }

    static VKAPI_ATTR VkBool32 VKAPI_CALL onValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                              VkDebugUtilsMessageTypeFlagsEXT,
                                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                              void* userData) {
        auto* context = static_cast<VulkanContext*>(userData);
        context->config_.onValidationMessage(severity, data->pMessage);
        return VK_FALSE;
    }

    VulkanContextConfig config_;

    std::unique_ptr<VulkanInstance> instance_;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    std::unique_ptr<VulkanSurface> surface_;

    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    uint32_t graphicsFamily_ = 0;
    std::optional<uint32_t> presentFamily_;
    std::unique_ptr<VulkanDevice> device_;

    std::array<QueueInfo, 3> queues_{};
    std::vector<std::unique_ptr<std::mutex>> queueLocks_;

    std::mutex immediateMutex_;
    std::unique_ptr<CommandPool> immediatePool_;

    std::unique_ptr<ResourceManager> resourceManager_;
};

// VulkanSampler's managed factories, defined here since VkTypes.h cannot depend on VulkanContext
inline VkResult VulkanSampler::createWithAddressMode(Handle<VulkanSampler>& outSampler,
                                                     VulkanContext& context,
                                                     VkSamplerAddressMode addressMode) {
    return createWithModeAndFilter(outSampler, context, addressMode, VK_FILTER_LINEAR);
}

inline VkResult VulkanSampler::createWithModeAndFilter(Handle<VulkanSampler>& outSampler,
                                                       VulkanContext& context,
                                                       VkSamplerAddressMode addressMode,
                                                       VkFilter filter) {
    std::unique_ptr<VulkanSampler> sampler;
    VkResult result = createWithModeAndFilter(sampler, addressMode, filter, context.device(), context.physicalDevice());
    outSampler = context.resources().adopt(std::move(sampler));
    return result;
}
