#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "VkUtil.h"
#include "DeviceSelection.h"
#include "QueueFamilyIndices.h"
#include "SwapChainSupportDetails.h"

#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <set>

const uint32_t WINDOW_WIDTH = 800;
const uint32_t WINDOW_HEIGHT = 600;

const std::vector<const char*> validationLayers = {
    "VK_LAYER_KHRONOS_validation"
};


#ifdef NDEBUG
    const bool enableValidationLayers = false;
#else
    const bool enableValidationLayers = true;
#endif


class HelloTriangleApplication {
public:
    void run() {
        initWindow();
        initVulkan();
        mainLoop();
    }

private:
    void logSupportedExtensions () {
        std::cout << "Available Extensions: " << std::endl;
        for (const auto& extension :
             readVkVector<VkExtensionProperties, const char*>(vkEnumerateInstanceExtensionProperties)) {
            std::cout << "\t" << extension.extensionName << std::endl;
        }
    }
    
    void logAvailableLayers() {
        std::cout << "Available Layers: " << std::endl;
        for (const auto& layerProperties :
             readVkVector<VkLayerProperties>(vkEnumerateInstanceLayerProperties)) {
            std::cout << "\t" << layerProperties.layerName << std::endl;
        }
    }
    
    bool checkValidationLayerSupport() {
        std::vector<VkLayerProperties> availableLayers = readVkVector<VkLayerProperties>(vkEnumerateInstanceLayerProperties);
        
        for (const auto layerName : validationLayers) {
            bool layerFound = false;
            for (const auto& layerProperties : availableLayers) {
                if (strcmp(layerName, layerProperties.layerName) == 0) {
                    layerFound = true;
                }
            }
            if (!layerFound) {
                return false;
            }
        }
        return true;
    }
    
    void createInstance() {
        //TODO: Figure out why validation layers aren't available
        //logSupportedExtensions();
        //logAvailableLayers();
        
        //if (enableValidationLayers && !checkValidationLayerSupport()) {
        //    throw std::runtime_error("Validation layers requested but not available!");
        //}
        
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "Hello Triangle";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "No Engine";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_0;
        
        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        
        uint32_t glfwExtensionCount = 0;
        const char** glfwExtensions;

        glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

        std::vector<const char*> requiredExtensions;

        for(uint32_t i = 0; i < glfwExtensionCount; i++) {
            requiredExtensions.emplace_back(glfwExtensions[i]);
        }

        requiredExtensions.emplace_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);

        createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;

        createInfo.enabledExtensionCount = (uint32_t) requiredExtensions.size();
        createInfo.ppEnabledExtensionNames = requiredExtensions.data();
        
        createInfo.enabledLayerCount = 0;
        
        VK_SUCCESS_OR_THROW(vkCreateInstance(&createInfo, nullptr, this->instance.get()),
                            "Failed to create Instance!");
    }
    
    void choosePhysicalDevice() {
        this->physicalDevice = pickPhysicalDevice(this->instance, this->surface);
        
        if (this->physicalDevice == VK_NULL_HANDLE) {
            throw std::runtime_error("Failed to find a suitable GPU.");
        }
    }
    
    void createLogicalDevice() {
        QueueFamilyIndices indices = findQueueFamilies(this->physicalDevice, this->surface);
        
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        
        float queuePriority = 1.0f; // Required even if only 1 queue
        for (uint32_t queueFamily : indices.uniqueQueueFamilies()) {
            VkDeviceQueueCreateInfo queueCreateInfo{};
            queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCreateInfo.queueFamilyIndex = queueFamily;
            queueCreateInfo.queueCount = 1;
            queueCreateInfo.pQueuePriorities = &queuePriority;
            
            queueCreateInfos.push_back(queueCreateInfo);
        }
        
        VkPhysicalDeviceFeatures deviceFeatures{}; // empty for now;
        
        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.pEnabledFeatures = &deviceFeatures;
        
        createInfo.enabledLayerCount = 0;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
        createInfo.ppEnabledExtensionNames = deviceExtensions.data();
        
        VK_SUCCESS_OR_THROW(vkCreateDevice(this->physicalDevice, &createInfo, nullptr, this->device.get()),
                            "Failed to create logical device.");
        
        vkGetDeviceQueue(*this->device, indices.graphicsFamily.value(), 0, &this->graphicsQueue);
        vkGetDeviceQueue(*this->device, indices.presentFamily.value(), 0, &this->presentQueue);
    }
    
    void createSurface() {
        VK_SUCCESS_OR_THROW(glfwCreateWindowSurface(*this->instance, this->window.get(), nullptr, this->surface.get()),
                            "Failed to create window surface");
    }
    
    void createSwapChain() {
        SwapChainSupportDetails swapChainSupport = querySwapChainSupport(this->physicalDevice, this->surface);
        
        auto surfaceFormat = swapChainSupport.chooseSwapSurfaceFormat();
        auto presentMode = swapChainSupport.chooseSwapPresentMode();
        auto extent = swapChainSupport.chooseSwapExtent(this->window.get());
        
        uint32_t imageCount = std::min(swapChainSupport.capabilities.minImageCount + 1,
                                       swapChainSupport.capabilities.maxImageCount);
        
        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = *this->surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1; // Always 1 unless steroscopic 3D
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        
        // Set sharing mode based on if a single present/graphics queue will be accessing the swapchain or 2 separate queues
        QueueFamilyIndices indices = findQueueFamilies(physicalDevice, this->surface);
        uint32_t queueFamilyIndices[] = {indices.graphicsFamily.value(), indices.presentFamily.value()};
        if (indices.graphicsFamily != indices.presentFamily) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        } else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            createInfo.queueFamilyIndexCount = 0; // Optional
            createInfo.pQueueFamilyIndices = nullptr; // Optional
        }
        
        // Don't apply any rotation, flips, etc
        createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
        // Make the swapchain image opaque
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        
        createInfo.presentMode = presentMode;
        createInfo.clipped = true;
        
        // Advanced
        createInfo.oldSwapchain = VK_NULL_HANDLE;
        
        VK_SUCCESS_OR_THROW(vkCreateSwapchainKHR(*this->device, &createInfo, nullptr, this->swapChain.get()),
                            "Failed to create swap chain.");
        
        // Retrieve swapchain images
        this->swapChainImages = readVkVector<VkImage, VkDevice, VkSwapchainKHR>(*this->device, *this->swapChain,
                                                                                vkGetSwapchainImagesKHR);
        
        // Cache swapchain properties
        this->swapChainImageFormat = surfaceFormat.format;
        this->swapChainExtent = extent;
    }
    
    void createSwapChainImageViews() {
        this->swapChainImageViews->resize(this->swapChainImages.size());
        
        for (size_t i = 0; i < swapChainImages.size(); ++i) {
            VkImageViewCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            createInfo.image = this->swapChainImages[i];
            createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            createInfo.format = this->swapChainImageFormat;
            // here is where you could make channels constant or remapped
            createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            // Interesting part, specifies which part of image to be accessed
            createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            createInfo.subresourceRange.baseMipLevel = 0;
            createInfo.subresourceRange.levelCount = 1;
            createInfo.subresourceRange.baseArrayLayer = 0;
            createInfo.subresourceRange.layerCount = 1;
            VK_SUCCESS_OR_THROW(vkCreateImageView(*this->device, &createInfo, nullptr,
                                                  &this->swapChainImageViews->at(i)),
                                "Failed to create image view for swapchain");
        }
    }
    
    void initVulkan() {
        createInstance();
        createSurface();
        choosePhysicalDevice();
        createLogicalDevice();
        createSwapChain();
        createSwapChainImageViews();
    }
    
    void initWindow() {
        glfwInit();
        
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
        
        this->window = std::unique_ptr<GLFWwindow, std::function<void(GLFWwindow*)>>(glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Vulkan", nullptr, nullptr), [](GLFWwindow* w){
            glfwDestroyWindow(w);
            glfwTerminate();
        });
    }

    void mainLoop() {
        while(!glfwWindowShouldClose(this->window.get())) {
            glfwPollEvents();
        }
    }
private:
    std::unique_ptr<GLFWwindow, std::function<void(GLFWwindow*)>> window;
    Handle<VkInstance> instance{[](VkInstance* i){
        vkDestroyInstance(*i, nullptr);
    }};
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    Handle<VkDevice> device{[](VkDevice* d){
        vkDestroyDevice(*d, nullptr);
    }};
    VkQueue graphicsQueue;
    VkQueue presentQueue;
    Handle<VkSurfaceKHR> surface{[this](VkSurfaceKHR* s){
        vkDestroySurfaceKHR(*this->instance, *s, nullptr);
    }};
    Handle<VkSwapchainKHR> swapChain{[this](VkSwapchainKHR* s){
        vkDestroySwapchainKHR(*this->device, *s, nullptr);
    }};
    
    std::vector<VkImage> swapChainImages;
    VkFormat swapChainImageFormat;
    VkExtent2D swapChainExtent;
    
    Handle<std::vector<VkImageView>> swapChainImageViews{[this](std::vector<VkImageView>* v){
        for (auto i : *v) {
            vkDestroyImageView(*this->device, i, nullptr);
        }
    }};
};

int main() {
    HelloTriangleApplication app;

    try {
        app.run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
