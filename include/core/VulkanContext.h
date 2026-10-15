#pragma once

#include "core/VkTypes.h"
#include "core/VkUtil.h"
#include "core/ResourceManager.h"
#include "core/DeviceSelection.h"
#include "core/DeviceFeatures.h"
#include "core/FrameResources.h"

#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <iostream>
#include <list>
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
    - Holds the number of frames in flight and the current frame, and keeps PerFrame collections
      (see FrameResources.h) sized to match

 Thread safety:
    - Queues are externally synchronized in Vulkan. Submit, present and wait on queues through
      submit() / present() / waitIdle() / waitIdleAll(), which lock the underlying VkQueue.
      Graphics, compute and present may share a VkQueue, so locks are per VkQueue, not per role.
    - createCommandPool() may be called from any thread. Each CommandPool is NOT thread safe; see
      CommandPool below.
    - submitImmediate() may be called from any thread.
    - The ResourceManager is thread safe.
    - PerFrame collections may be created and destroyed on any thread. advanceFrame() must only be
      called by the thread driving frames.

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

// How many descriptors of each type the universal set holds. Clamped to the device's limits.
struct DescriptorCapacities {
    uint32_t sampledImages = 4096;
    uint32_t samplers = 256;
    uint32_t storageImages = 1024;
    uint32_t storageBuffers = 4096;
};

class DescriptorHeap;

struct VulkanContextConfig {
    std::string applicationName = "VulkanApp";

    // Instance extensions required by the application (e.g. glfwGetRequiredInstanceExtensions).
    // Portability enumeration and, with validation, debug utils are added automatically.
    std::vector<const char*> instanceExtensions;

    // Enables the Khronos validation layer. If the layer is unavailable a warning is printed
    // and the context is created without it.
    bool enableValidation = false;

    // With validation, also enables synchronization validation, which reports missing or
    // insufficient barriers and semaphores (hazards) as errors. Slower.
    bool enableSynchronizationValidation = false;

    // Receives validation messages (warnings and errors). Defaults to printing to std::cerr.
    // May be called from any thread that calls into Vulkan.
    std::function<void(VkDebugUtilsMessageSeverityFlagBitsEXT, const char*)> onValidationMessage;

    // Size of the universal descriptor set (see DescriptorHeap)
    DescriptorCapacities descriptorCapacities;

    // Number of frames the CPU may record while the GPU is still executing earlier ones. Can be
    // changed later with VulkanContext::setFramesInFlight.
    uint32_t framesInFlight = 2;

    // Creates the surface to present to. Without one the context is headless: no present
    // queue is available and devices are not required to support presentation.
    std::function<VkResult(VkInstance, VkSurfaceKHR*)> createSurface;
};

class VulkanContext {
public:
    explicit VulkanContext(VulkanContextConfig config)
    : config_(std::move(config)), frames_(config_.framesInFlight) {
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
        frameEnds_.resize(frames_.framesInFlight());
    }

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    ~VulkanContext() {
        waitIdleAll();
        // Retired work may release resources, so it runs before they are destroyed
        while (true) {
            std::list<std::pair<TimelinePoints, std::function<void()>>> retired;
            {
                std::lock_guard<std::mutex> lock(retireMutex_);
                retired.swap(retired_);
            }
            if (retired.empty()) {
                break;
            }
            for (auto& [points, fn] : retired) {
                fn();
            }
        }
        // Destroy in reverse dependency order. Descriptor entries (managed resources) free their
        // slots in the heap, so the heap goes after them.
        resourceManager_.reset();
        descriptors_.reset();
        immediatePool_.reset();
        queues_.clear();
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

    // The universal descriptor set and pipeline layout, created on first use. Defined in
    // descriptors/DescriptorHeap.h, which must be included to use it.
    DescriptorHeap& descriptors();

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

public: // Queues
    // The primary queue supports graphics and compute (and presentation, when the device allows
    // it on the same queue). QueueType::Graphics and QueueType::Compute refer to it.
    static constexpr uint32_t kPrimaryQueue = 0;
    // At most this many queues are created, the primary queue included
    static constexpr uint32_t kMaxQueues = 4;

    struct QueueDescription {
        uint32_t family;
        uint32_t indexInFamily;
        VkQueueFlags flags;
        bool present;   // Can present to the surface
    };

    // Number of queues the context created: the primary queue, then any other queues the device
    // offers that support compute (from other families, or further queues of the same family),
    // up to kMaxQueues, and a present queue if the primary queue can't present
    uint32_t queueCount() { return static_cast<uint32_t>(queues_.size()); }

    const QueueDescription& queueDescription(uint32_t queue) { return queues_.at(queue).description; }

    // The queue a QueueType refers to. Throws std::logic_error for Present without a surface.
    uint32_t queueIndex(QueueType type) {
        if (type != QueueType::Present) {
            return kPrimaryQueue;
        }
        if (!presentQueue_) {
            throw std::logic_error("Queue type unavailable (a present queue requires a surface)");
        }
        return *presentQueue_;
    }

    uint32_t queueFamily(QueueType type) {
        return queues_.at(queueIndex(type)).description.family;
    }

    // The distinct queue families of the context's queues. Resources are shared between them
    // concurrently (VK_SHARING_MODE_CONCURRENT) when there is more than one.
    const std::vector<uint32_t>& queueFamilies() { return queueFamilies_; }

    // The raw queue. Host access to it must be externally synchronized: prefer submit(),
    // present() and waitIdle(), which hold the queue's lock.
    VkQueue queue(QueueType type) {
        return queues_.at(queueIndex(type)).queue;
    }

    VkQueue queue(uint32_t queue) {
        return queues_.at(queue).queue;
    }

public: // Timelines (thread safe)
    // Each queue has a timeline semaphore that the context signals with an increasing value on
    // every submission. A queue's values are complete in order, so a value identifies all work
    // submitted to that queue up to and including the submission that signaled it.

    // A timeline value per queue (0 for none), e.g. everything submitted up to some point
    using TimelinePoints = std::vector<uint64_t>;

    struct SubmitBatch {
        std::vector<VkCommandBufferSubmitInfo> commandBuffers;
        std::vector<VkSemaphoreSubmitInfo> waits;
        std::vector<VkSemaphoreSubmitInfo> signals;   // In addition to the queue's timeline
        VkFence fence = VK_NULL_HANDLE;
    };

    // Submits batch to a queue, also signaling the queue's timeline semaphore with its next
    // value, which is returned. Throws on failure.
    uint64_t submit(uint32_t queue, const SubmitBatch& batch) {
        uint64_t value = 0;
        VK_SUCCESS_OR_THROW(submitAndSignal(queue, batch, value), "Failed to submit to queue");
        return value;
    }

    VkSemaphore timelineSemaphore(uint32_t queue) {
        return **queues_.at(queue).timeline;
    }

    // The last value submitted to a queue's timeline
    uint64_t submittedValue(uint32_t queue) {
        auto& entry = queues_.at(queue);
        std::lock_guard<std::mutex> lock(*entry.lock);
        return entry.submitted;
    }

    // The value a queue's timeline has reached
    uint64_t completedValue(uint32_t queue) {
        uint64_t value = 0;
        vkGetSemaphoreCounterValue(**device_, **queues_.at(queue).timeline, &value);
        return value;
    }

    // Everything submitted to any queue so far
    TimelinePoints submittedPoints() {
        TimelinePoints points(queues_.size());
        for (uint32_t queue = 0; queue < queues_.size(); ++queue) {
            points[queue] = submittedValue(queue);
        }
        return points;
    }

    bool isComplete(const TimelinePoints& points) {
        for (uint32_t queue = 0; queue < points.size(); ++queue) {
            if (points[queue] != 0 && completedValue(queue) < points[queue]) {
                return false;
            }
        }
        return true;
    }

    // Blocks until every point has been reached
    void wait(const TimelinePoints& points) {
        std::vector<VkSemaphore> semaphores;
        std::vector<uint64_t> values;
        for (uint32_t queue = 0; queue < points.size(); ++queue) {
            if (points[queue] != 0) {
                semaphores.push_back(**queues_.at(queue).timeline);
                values.push_back(points[queue]);
            }
        }
        if (semaphores.empty()) {
            return;
        }
        VkSemaphoreWaitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        waitInfo.semaphoreCount = static_cast<uint32_t>(semaphores.size());
        waitInfo.pSemaphores = semaphores.data();
        waitInfo.pValues = values.data();
        VK_SUCCESS_OR_THROW(vkWaitSemaphores(**device_, &waitInfo, UINT64_MAX), "Failed to wait for timelines");
    }

    void wait(uint32_t queue, uint64_t value) {
        TimelinePoints points(queues_.size());
        points.at(queue) = value;
        wait(points);
    }

public: // Deferred destruction (thread safe)
    // Runs fn once all work submitted so far has completed, e.g. to destroy resources that work
    // may use. Called from beginFrame() (or collectRetired()), or when the context is destroyed.
    void retire(std::function<void()> fn) {
        auto points = submittedPoints();
        std::lock_guard<std::mutex> lock(retireMutex_);
        retired_.push_back({std::move(points), std::move(fn)});
    }

    // Runs the retired functions whose work has completed
    void collectRetired() {
        std::vector<std::function<void()>> ready;
        {
            std::lock_guard<std::mutex> lock(retireMutex_);
            for (auto it = retired_.begin(); it != retired_.end();) {
                if (isComplete(it->first)) {
                    ready.push_back(std::move(it->second));
                    it = retired_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        // Outside the lock: functions may retire more work
        for (auto& fn : ready) {
            fn();
        }
    }

public: // Frames in flight
    uint32_t framesInFlight() { return frames_.framesInFlight(); }

    // Index of the frame being recorded, in [0, framesInFlight())
    uint32_t currentFrame() { return frames_.currentFrame(); }

    // Number of frames ended with advanceFrame() so far
    uint64_t frameNumber() { return frameNumber_; }

    // Tracks the PerFrame collections created from this context
    FrameRegistry& frames() { return frames_; }

    // Requests a new number of frames in flight. It takes effect at the next advanceFrame(), so it
    // may be called mid-frame (e.g. from a callback while a frame is recorded). Thread safe.
    void setFramesInFlight(uint32_t count) {
        if (count == 0) {
            throw std::invalid_argument("At least one frame must be in flight");
        }
        pendingFramesInFlight_ = count;
    }

    // Starts recording the current frame: waits until the work submitted the last time this frame
    // slot was used has completed (so the slot's PerFrame elements are free to reuse), then runs
    // deferred destruction that has become safe. Only waits for that one slot.
    void beginFrame() {
        wait(frameEnds_.at(currentFrame()));
        collectRetired();
    }

    // Ends the current frame, recording the work submitted so far as the slot's, and moves on to
    // the next. If a new number of frames in flight was requested it is applied now: shrinking
    // waits only for the frame slots being removed, growing doesn't wait at all. Either way,
    // every PerFrame collection is resized and frames restart at slot 0.
    void advanceFrame() {
        frameEnds_.at(currentFrame()) = submittedPoints();
        ++frameNumber_;

        uint32_t pending = pendingFramesInFlight_.exchange(0);
        if (pending != 0 && pending != frames_.framesInFlight()) {
            for (uint32_t slot = pending; slot < frameEnds_.size(); ++slot) {
                wait(frameEnds_[slot]);
            }
            frames_.resize(pending);
            frameEnds_.resize(pending);
        } else {
            frames_.advance();
        }
    }

public: // Queue access (thread safe)
    // Submits with the original VkSubmitInfo structures. Each submission also signals the queue's
    // timeline, so frames and deferred destruction account for it.
    VkResult submit(QueueType type, uint32_t submitCount, const VkSubmitInfo* submits, VkFence fence) {
        for (uint32_t i = 0; i < submitCount; ++i) {
            const VkSubmitInfo& info = submits[i];
            assert(info.pNext == nullptr && "Extension structures are not supported; use submit(queue, SubmitBatch)");
            SubmitBatch batch;
            for (uint32_t c = 0; c < info.commandBufferCount; ++c) {
                VkCommandBufferSubmitInfo commandBuffer{};
                commandBuffer.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
                commandBuffer.commandBuffer = info.pCommandBuffers[c];
                batch.commandBuffers.push_back(commandBuffer);
            }
            for (uint32_t w = 0; w < info.waitSemaphoreCount; ++w) {
                VkSemaphoreSubmitInfo wait{};
                wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
                wait.semaphore = info.pWaitSemaphores[w];
                // The original stage flags are valid synchronization2 stage flags
                wait.stageMask = info.pWaitDstStageMask[w];
                batch.waits.push_back(wait);
            }
            for (uint32_t s = 0; s < info.signalSemaphoreCount; ++s) {
                VkSemaphoreSubmitInfo signal{};
                signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
                signal.semaphore = info.pSignalSemaphores[s];
                signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                batch.signals.push_back(signal);
            }
            batch.fence = i + 1 == submitCount ? fence : VK_NULL_HANDLE;
            uint64_t value = 0;
            VkResult result = submitAndSignal(queueIndex(type), batch, value);
            if (result != VK_SUCCESS) {
                return result;
            }
        }
        return VK_SUCCESS;
    }

    VkResult submit(QueueType type, const VkSubmitInfo& submitInfo, VkFence fence) {
        return submit(type, 1, &submitInfo, fence);
    }

    VkResult present(const VkPresentInfoKHR& presentInfo) {
        auto& entry = queues_.at(queueIndex(QueueType::Present));
        std::lock_guard<std::mutex> lock(*entry.lock);
        return vkQueuePresentKHR(entry.queue, &presentInfo);
    }

    VkResult waitIdle(QueueType type) {
        auto& entry = queues_.at(queueIndex(type));
        std::lock_guard<std::mutex> lock(*entry.lock);
        return vkQueueWaitIdle(entry.queue);
    }

    // Waits for all queues. vkDeviceWaitIdle requires every queue to be externally synchronized,
    // so this takes all queue locks.
    VkResult waitIdleAll() {
        std::vector<std::unique_lock<std::mutex>> locks;
        for (auto& entry : queues_) {
            locks.emplace_back(*entry.lock); // Always locked in the same order, avoiding deadlocks
        }
        return vkDeviceWaitIdle(**device_);
    }

public: // Command recording
    // Creates a command pool for the given queue's family, owned by the caller. Thread safe.
    // The returned pool is not; see CommandPool.
    std::unique_ptr<CommandPool> createCommandPool(QueueType type,
                                                   VkCommandPoolCreateFlags flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) {
        return createCommandPool(queueIndex(type), flags);
    }

    std::unique_ptr<CommandPool> createCommandPool(uint32_t queue,
                                                   VkCommandPoolCreateFlags flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) {
        return std::make_unique<CommandPool>(**device_, queues_.at(queue).description.family, flags);
    }

    // Records commands with op, submits them to the primary queue and blocks until they have
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

        SubmitBatch batch;
        VkCommandBufferSubmitInfo commandBufferInfo{};
        commandBufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        commandBufferInfo.commandBuffer = commandBuffer;
        batch.commandBuffers.push_back(commandBufferInfo);
        const uint64_t value = submit(kPrimaryQueue, batch);

        // Wait on the timeline rather than the queue, so the queue's lock is not held while waiting
        wait(kPrimaryQueue, value);
        immediatePool_->free(commandBuffer);
    }

private:
    struct QueueEntry {
        QueueDescription description{};
        VkQueue queue = VK_NULL_HANDLE;
        std::unique_ptr<std::mutex> lock;
        std::unique_ptr<VulkanSemaphore> timeline;
        uint64_t submitted = 0;   // Guarded by lock
    };

    VkResult submitAndSignal(uint32_t queue, const SubmitBatch& batch, uint64_t& outValue) {
        auto& entry = queues_.at(queue);
        std::lock_guard<std::mutex> lock(*entry.lock);

        std::vector<VkSemaphoreSubmitInfo> signals = batch.signals;
        VkSemaphoreSubmitInfo timelineSignal{};
        timelineSignal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        timelineSignal.semaphore = **entry.timeline;
        timelineSignal.value = entry.submitted + 1;
        timelineSignal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signals.push_back(timelineSignal);

        VkSubmitInfo2 submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submitInfo.waitSemaphoreInfoCount = static_cast<uint32_t>(batch.waits.size());
        submitInfo.pWaitSemaphoreInfos = batch.waits.data();
        submitInfo.commandBufferInfoCount = static_cast<uint32_t>(batch.commandBuffers.size());
        submitInfo.pCommandBufferInfos = batch.commandBuffers.data();
        submitInfo.signalSemaphoreInfoCount = static_cast<uint32_t>(signals.size());
        submitInfo.pSignalSemaphoreInfos = signals.data();

        VkResult result = vkQueueSubmit2(entry.queue, 1, &submitInfo, batch.fence);
        if (result == VK_SUCCESS) {
            outValue = ++entry.submitted;
        }
        return result;
    }

    static bool hasInstanceLayer(const char* name) {
        for (const auto& layer : readVkVector<VkLayerProperties>(vkEnumerateInstanceLayerProperties)) {
            if (strcmp(layer.layerName, name) == 0) {
                return true;
            }
        }
        return false;
    }

    // Whether the instance (or, with layer, that layer) provides an extension
    static bool hasInstanceExtension(const char* name, const char* layer = nullptr) {
        uint32_t count = 0;
        vkEnumerateInstanceExtensionProperties(layer, &count, nullptr);
        std::vector<VkExtensionProperties> extensions(count);
        vkEnumerateInstanceExtensionProperties(layer, &count, extensions.data());
        for (const auto& extension : extensions) {
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
        appInfo.apiVersion = VK_API_VERSION_1_3;

        // Synchronization validation is enabled through VK_EXT_validation_features, which the
        // validation layer provides
        VkValidationFeatureEnableEXT enabledValidationFeatures[] = {
            VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
        };
        VkValidationFeaturesEXT validationFeatures{};
        validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
        validationFeatures.enabledValidationFeatureCount = 1;
        validationFeatures.pEnabledValidationFeatures = enabledValidationFeatures;
        const bool useSyncValidation = useValidation && config_.enableSynchronizationValidation
                                       && hasInstanceExtension(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME, validationLayer);
        if (useValidation && config_.enableSynchronizationValidation && !useSyncValidation) {
            std::cerr << "Synchronization validation requested but " << VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME
                      << " is unavailable; continuing without it" << std::endl;
        }
        if (useSyncValidation) {
            extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
        }

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pNext = useSyncValidation ? &validationFeatures : nullptr;
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

    void pickPhysicalDevice() {
        std::string rejections;
        for (auto device : readVkVector<VkPhysicalDevice, VkInstance>(**instance_, vkEnumeratePhysicalDevices)) {
            auto shortcomings = deviceShortcomings(device);
            if (!shortcomings.empty()) {
                VkPhysicalDeviceProperties properties{};
                vkGetPhysicalDeviceProperties(device, &properties);
                rejections += std::string("\n  ") + properties.deviceName + " lacks:";
                for (auto& shortcoming : shortcomings) {
                    rejections += " " + shortcoming;
                }
                continue;
            }
            if (hasSurface() && !isDeviceSuitable(device, **surface_)) {
                continue;
            }
            auto families = readVkVectorVoid<VkQueueFamilyProperties, VkPhysicalDevice>(device, vkGetPhysicalDeviceQueueFamilyProperties);
            auto supportsPresent = [&](uint32_t family) {
                VkBool32 support = VK_FALSE;
                if (hasSurface()) {
                    vkGetPhysicalDeviceSurfaceSupportKHR(device, family, **surface_, &support);
                }
                return support == VK_TRUE;
            };
            // The primary family supports graphics and compute, preferably presentation too
            std::optional<uint32_t> graphicsFamily;
            for (uint32_t i = 0; i < families.size(); ++i) {
                const bool graphicsCompute = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                                             && (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT);
                if (graphicsCompute && (!graphicsFamily || (!supportsPresent(*graphicsFamily) && supportsPresent(i)))) {
                    graphicsFamily = i;
                }
            }
            if (!graphicsFamily) {
                continue;
            }
            physicalDevice_ = device;
            graphicsFamily_ = *graphicsFamily;
            presentFamily_.reset();
            if (hasSurface()) {
                for (uint32_t i = 0; i < families.size() && !presentFamily_; ++i) {
                    if (supportsPresent(i)) {
                        presentFamily_ = i;
                    }
                }
                if (supportsPresent(graphicsFamily_)) {
                    presentFamily_ = graphicsFamily_;
                }
            }
            return;
        }
        throw std::runtime_error("Failed to find a suitable GPU." + rejections);
    }

    void createDevice() {
        auto families = readVkVectorVoid<VkQueueFamilyProperties, VkPhysicalDevice>(physicalDevice_, vkGetPhysicalDeviceQueueFamilyProperties);

        // The queues to create: the primary queue, a separate present queue if the primary can't
        // present, then other compute-capable queues, up to kMaxQueues working queues
        std::vector<QueueDescription> planned;
        std::vector<uint32_t> usedPerFamily(families.size(), 0);
        auto plan = [&](uint32_t family, bool present) {
            planned.push_back({family, usedPerFamily[family]++, families[family].queueFlags, present});
        };
        const bool primaryPresents = presentFamily_ && *presentFamily_ == graphicsFamily_;
        plan(graphicsFamily_, primaryPresents);
        uint32_t working = 1;
        if (presentFamily_ && !primaryPresents) {
            plan(*presentFamily_, true);
            presentQueue_ = 1;
            if (families[*presentFamily_].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                ++working;
            }
        } else if (presentFamily_) {
            presentQueue_ = kPrimaryQueue;
        }
        for (uint32_t family = 0; family < families.size() && working < kMaxQueues; ++family) {
            if (!(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                continue;
            }
            while (usedPerFamily[family] < families[family].queueCount && working < kMaxQueues) {
                plan(family, false);
                ++working;
            }
        }

        std::vector<std::vector<float>> priorities(families.size());
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        for (uint32_t family = 0; family < families.size(); ++family) {
            if (usedPerFamily[family] == 0) {
                continue;
            }
            priorities[family].assign(usedPerFamily[family], 1.0f);
            VkDeviceQueueCreateInfo queueInfo{};
            queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo.queueFamilyIndex = family;
            queueInfo.queueCount = usedPerFamily[family];
            queueInfo.pQueuePriorities = priorities[family].data();
            queueCreateInfos.push_back(queueInfo);
            queueFamilies_.push_back(family);
        }

        // Every required feature was checked when picking the physical device
        const DeviceFeatures features = DeviceFeatures::required();

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
        createInfo.pNext = features.chain();
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        VK_SUCCESS_OR_THROW(VulkanDevice::create(device_, physicalDevice_, createInfo),
                            "Failed to create logical device");

        // Each queue gets a lock (queues are externally synchronized) and a timeline semaphore
        for (const auto& description : planned) {
            QueueEntry entry;
            entry.description = description;
            vkGetDeviceQueue(**device_, description.family, description.indexInFamily, &entry.queue);
            entry.lock = std::make_unique<std::mutex>();

            VkSemaphoreTypeCreateInfo typeInfo{};
            typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
            typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            typeInfo.initialValue = 0;
            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            semaphoreInfo.pNext = &typeInfo;
            VK_SUCCESS_OR_THROW(VulkanSemaphore::create(entry.timeline, **device_, semaphoreInfo),
                                "Failed to create timeline semaphore");
            queues_.push_back(std::move(entry));
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

    // Declared first so it is destroyed last: PerFrame collections unregister when destroyed
    FrameRegistry frames_;
    std::atomic<uint32_t> pendingFramesInFlight_{0};

    std::unique_ptr<VulkanInstance> instance_;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    std::unique_ptr<VulkanSurface> surface_;

    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    uint32_t graphicsFamily_ = 0;
    std::optional<uint32_t> presentFamily_;
    std::unique_ptr<VulkanDevice> device_;

    std::vector<QueueEntry> queues_;
    std::optional<uint32_t> presentQueue_;
    std::vector<uint32_t> queueFamilies_;

    // Work submitted by each frame slot, waited for before the slot is reused
    std::vector<TimelinePoints> frameEnds_;
    uint64_t frameNumber_ = 0;

    std::mutex retireMutex_;
    std::list<std::pair<TimelinePoints, std::function<void()>>> retired_;

    std::mutex immediateMutex_;
    std::unique_ptr<CommandPool> immediatePool_;

    std::unique_ptr<ResourceManager> resourceManager_;

    // Type-erased deleter: DescriptorHeap is incomplete here
    std::once_flag descriptorsCreated_;
    std::unique_ptr<DescriptorHeap, void (*)(DescriptorHeap*)> descriptors_{nullptr, nullptr};
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

template<typename T>
PerFrame<T>::PerFrame(VulkanContext& context, Create create, Destroy destroy)
: PerFrame(context.frames(), std::move(create), std::move(destroy)) {}
