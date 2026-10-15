#pragma once

#include "VkTypes.h"
#include "VkUtil.h"
#include "ResourceManager.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

/*
 Headless Vulkan context shared by all GPU tests.

 Creates an instance, physical device, logical device, a single graphics + compute
 queue, a command pool and a ResourceManager, without a window or surface.

 When available, the Khronos validation layer is enabled and every validation error
 is recorded, so that tests can fail on API misuse (see GpuTest below). Set
 VK_TEST_DISABLE_VALIDATION=1 to run without it.
 */
class VkTestContext {
public:
    // Created on first use. Destroyed by shutdown() at the end of main(), rather than during
    // static destruction, where the validation layer's own globals may already be gone.
    static VkTestContext& get() {
        auto& context = storage();
        if (!context) {
            context.reset(new VkTestContext());
        }
        return *context;
    }

    static void shutdown() {
        storage().reset();
    }

    VkInstance instance() { return **instance_; }
    VkPhysicalDevice physicalDevice() { return physicalDevice_; }
    VkDevice device() { return **device_; }
    VkQueue queue() { return queue_; }
    uint32_t queueFamily() { return queueFamily_; }
    VkCommandPool commandPool() { return **commandPool_; }
    ResourceManager& resources() { return *resourceManager_; }
    bool validationEnabled() { return messenger_ != VK_NULL_HANDLE; }

    std::string deviceName() {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
        return properties.deviceName;
    }

    // Returns and clears the validation errors recorded so far
    std::vector<std::string> takeValidationErrors() {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::exchange(validationErrors_, {});
    }

    ~VkTestContext() {
        vkDeviceWaitIdle(**device_);
        // Destroy in reverse dependency order: managed resources, pool, device, messenger, instance
        resourceManager_.reset();
        commandPool_.reset();
        device_.reset();
        if (messenger_ != VK_NULL_HANDLE) {
            auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(**instance_, "vkDestroyDebugUtilsMessengerEXT"));
            destroyMessenger(**instance_, messenger_, nullptr);
        }
        instance_.reset();
    }

private:
    static std::unique_ptr<VkTestContext>& storage() {
        static std::unique_ptr<VkTestContext> context;
        return context;
    }

    VkTestContext() {
        createInstance();
        pickPhysicalDevice();
        createDevice();

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily_;
        VK_SUCCESS_OR_THROW(VulkanCommandPool::create(commandPool_, **device_, poolInfo),
                            "Failed to create test command pool");

        resourceManager_ = std::make_unique<ResourceManager>();
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
        const char* disableValidation = std::getenv("VK_TEST_DISABLE_VALIDATION");
        const bool useValidation = !(disableValidation && strcmp(disableValidation, "1") == 0)
                                   && hasInstanceLayer(validationLayer)
                                   && hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        std::vector<const char*> extensions;
        std::vector<const char*> layers;
        VkInstanceCreateFlags flags = 0;

        if (hasInstanceExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
        if (useValidation) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            layers.push_back(validationLayer);
        }

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "vulkan_test GPU tests";
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
                            "Failed to create test instance");

        if (useValidation) {
            VkDebugUtilsMessengerCreateInfoEXT messengerInfo{};
            messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
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

    void pickPhysicalDevice() {
        for (auto device : readVkVector<VkPhysicalDevice, VkInstance>(**instance_, vkEnumeratePhysicalDevices)) {
            auto families = readVkVectorVoid<VkQueueFamilyProperties, VkPhysicalDevice>(device, vkGetPhysicalDeviceQueueFamilyProperties);
            for (uint32_t i = 0; i < families.size(); ++i) {
                if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                    physicalDevice_ = device;
                    queueFamily_ = i;
                    return;
                }
            }
        }
        throw std::runtime_error("No Vulkan device with a graphics + compute queue found");
    }

    void createDevice() {
        float queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = queueFamily_;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;

        // Samplers created by the wrapper always enable anisotropy
        VkPhysicalDeviceFeatures supported{};
        vkGetPhysicalDeviceFeatures(physicalDevice_, &supported);
        VkPhysicalDeviceFeatures features{};
        features.samplerAnisotropy = supported.samplerAnisotropy;

        // Portability (e.g. MoltenVK) implementations require this extension to be enabled
        std::vector<const char*> extensions;
        if (hasDeviceExtension(physicalDevice_, "VK_KHR_portability_subset")) {
            extensions.push_back("VK_KHR_portability_subset");
        }

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = 1;
        createInfo.pQueueCreateInfos = &queueInfo;
        createInfo.pEnabledFeatures = &features;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        VK_SUCCESS_OR_THROW(VulkanDevice::create(device_, physicalDevice_, createInfo),
                            "Failed to create test device");
        vkGetDeviceQueue(**device_, queueFamily_, 0, &queue_);
    }

    static VKAPI_ATTR VkBool32 VKAPI_CALL onValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                              VkDebugUtilsMessageTypeFlagsEXT,
                                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                              void* userData) {
        auto* context = static_cast<VkTestContext*>(userData);
        std::lock_guard<std::mutex> lock(context->mutex_);
        context->validationErrors_.emplace_back(data->pMessage);
        return VK_FALSE;
    }

    std::unique_ptr<VulkanInstance> instance_;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    std::unique_ptr<VulkanDevice> device_;
    uint32_t queueFamily_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::unique_ptr<VulkanCommandPool> commandPool_;
    std::unique_ptr<ResourceManager> resourceManager_;

    std::mutex mutex_;
    std::vector<std::string> validationErrors_;
};

/*
 doctest fixture for GPU tests. Fails the test if it produced validation errors or
 left resources alive in the shared ResourceManager.

     TEST_CASE_FIXTURE(GpuTest, "...") { auto& ctx = context(); ... }
 */
class GpuTest {
public:
    GpuTest() : resourcesBefore_(context().resources().size()) {
        context().takeValidationErrors();
    }

    ~GpuTest() {
        vkDeviceWaitIdle(context().device());
        for (const auto& error : context().takeValidationErrors()) {
            FAIL_CHECK("Vulkan validation error: " << error);
        }
        const size_t resourcesAfter = context().resources().size();
        if (resourcesAfter != resourcesBefore_) {
            FAIL_CHECK("Test leaked handles to managed resources: " << resourcesBefore_
                       << " before, " << resourcesAfter << " after");
        }
    }

protected:
    static VkTestContext& context() {
        return VkTestContext::get();
    }

private:
    size_t resourcesBefore_;
};
