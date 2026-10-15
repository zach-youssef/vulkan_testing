#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "core/VkUtil.h"
#include "core/VulkanContext.h"
#include "graph/RenderGraph.h"
#include "targets/Swapchain.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <string>

struct WindowConfig {
    std::string title = "Vulkan";
    // Initial size in screen coordinates (which may differ from pixels on high DPI displays).
    // Can be changed later with VulkanApp::setWindowSize.
    uint32_t width = 800;
    uint32_t height = 600;
    // Whether the user can resize the window
    bool resizable = true;
};

struct AppConfig {
    std::string applicationName = "VulkanApp";
    // The window to present to. Without one the app is headless: there is no surface or swapchain,
    // and frames are driven by calling drawFrame() (run() requires a window).
    std::optional<WindowConfig> window = WindowConfig{};
    // Turns on the Khronos validation layer (when installed), which reports API misuse to
    // onValidationMessage (by default, std::cerr)
    bool enableValidation = false;
    // With validation, also reports synchronization hazards (slower)
    bool enableSynchronizationValidation = false;
    std::function<void(VkDebugUtilsMessageSeverityFlagBitsEXT, const char*)> onValidationMessage;
    // Prints the available instance extensions and layers on startup
    bool logInstanceInfo = true;
    // Initial number of frames in flight; see VulkanContext::setFramesInFlight
    uint32_t framesInFlight = 2;
};

/*
 Owns a VulkanContext, unless headless a window and its swapchain, and any number of render graphs.
 Each frame executes the active graph (setActiveGraph).

 The swapchain is recreated whenever the window's framebuffer changes size, whether the user
 resized it or setWindowSize() was called. Graphs recreate their surface-relative images on their
 next frame; anything else sized to the surface can follow along with addSurfaceResizeListener().

 Not thread safe: call from the thread that created the app (GLFW requires its main thread).
 */
class VulkanApp {
public:
    using SurfaceResizeListener = std::function<void(VkExtent2D)>;
    using ListenerId = uint64_t;

    VulkanApp() = default;

    VulkanApp(const VulkanApp&) = delete;
    VulkanApp& operator=(const VulkanApp&) = delete;

    ~VulkanApp() {
        // Nothing below may be destroyed while the GPU is still using it
        if (context_) {
            context_->waitIdleAll();
        }
        graphs_.clear();
    }

    void init(AppConfig config = {}) {
        config_ = std::move(config);
        if (config_.window) {
            initWindow(*config_.window);
        }
        createContext();
        if (hasWindow()) {
            swapchain_ = std::make_unique<Swapchain>(*context_, getFramebufferSize());
        }
    }

    // Draws frames until the window is closed. Requires a window.
    void run() {
        if (!hasWindow()) {
            throw std::logic_error("run() requires a window; headless apps call drawFrame()");
        }
        while (runFrame()) {}
        context_->waitIdleAll();
    }

    // Processes window events and, unless the window has been asked to close, draws a frame.
    // Returns false (without drawing) once the window should close. Headless apps always draw.
    bool runFrame() {
        if (hasWindow()) {
            glfwPollEvents();
            if (glfwWindowShouldClose(window_.get())) {
                return false;
            }
        }
        drawFrame();
        return true;
    }

    // Waits, without drawing, until the window is closed
    void holdWindow() {
        while (hasWindow() && !glfwWindowShouldClose(window_.get())) {
            glfwWaitEvents();
        }
        context_->waitIdleAll();
    }

    // Executes the active graph for one frame, recreating the swapchain afterwards if it no longer
    // matches the window
    void drawFrame() {
        if (!activeGraph_) {
            throw std::logic_error("drawFrame() requires an active render graph; see createGraph() and setActiveGraph()");
        }
        FrameResult result = activeGraph_->execute();
        if (hasWindow() && (result.surfaceOutOfDate || frameBufferResized_)) {
            frameBufferResized_ = false;
            recreateSwapChain();
        }
    }

public: // Render graphs
    // Creates a graph that presents to the app's window (if any). The app owns it until
    // destroyGraph() or the app is destroyed.
    RenderGraph& createGraph() {
        graphs_.push_back(std::make_unique<RenderGraph>(*context_, swapchain_.get()));
        return *graphs_.back();
    }

    // The graph drawFrame() executes. Switching needs no waiting.
    void setActiveGraph(RenderGraph& graph) {
        findGraph(graph);
        activeGraph_ = &graph;
    }

    RenderGraph* activeGraph() {
        return activeGraph_;
    }

    // Destroys a graph once its submitted work has finished. It must not be active.
    void destroyGraph(RenderGraph& graph) {
        if (activeGraph_ == &graph) {
            throw std::logic_error("The active graph can't be destroyed");
        }
        graphs_.erase(findGraph(graph));
    }

public: // Window & surface
    bool hasWindow() {
        return window_ != nullptr;
    }

    // Resizes the window, in screen coordinates. The swapchain follows once the window's
    // framebuffer has been resized, at the end of the next frame.
    void setWindowSize(uint32_t width, uint32_t height) {
        requireWindow();
        glfwSetWindowSize(window_.get(), static_cast<int>(width), static_cast<int>(height));
    }

    // The window's size in screen coordinates
    VkExtent2D getWindowSize() {
        requireWindow();
        int width = 0, height = 0;
        glfwGetWindowSize(window_.get(), &width, &height);
        return {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    }

    // The size of the images presented to the window, in pixels
    VkExtent2D getSurfaceExtent() {
        requireWindow();
        return swapchain_->getExtent();
    }

    // Calls listener with the new extent whenever the swapchain is recreated at a new size, once
    // the device is idle. Returns an id for removeSurfaceResizeListener.
    ListenerId addSurfaceResizeListener(SurfaceResizeListener listener) {
        ListenerId id = nextListenerId_++;
        surfaceResizeListeners_.emplace(id, std::move(listener));
        return id;
    }

    void removeSurfaceResizeListener(ListenerId id) {
        surfaceResizeListeners_.erase(id);
    }

public: // Getters
    VulkanContext& getContext() {
        return *context_;
    }

    Swapchain& getSwapchain() {
        requireWindow();
        return *swapchain_;
    }

private:
    void initWindow(const WindowConfig& windowConfig) {
        if (!glfwInit()) {
            throw std::runtime_error("Failed to initialize GLFW");
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, windowConfig.resizable ? GLFW_TRUE : GLFW_FALSE);

        GLFWwindow* window = glfwCreateWindow(static_cast<int>(windowConfig.width),
                                              static_cast<int>(windowConfig.height),
                                              windowConfig.title.c_str(), nullptr, nullptr);
        if (!window) {
            glfwTerminate();
            throw std::runtime_error("Failed to create window");
        }
        window_ = std::unique_ptr<GLFWwindow, std::function<void(GLFWwindow*)>>(window, [](GLFWwindow* w){
            glfwDestroyWindow(w);
            glfwTerminate();
        });
        glfwSetWindowUserPointer(window_.get(), this);
        glfwSetFramebufferSizeCallback(window_.get(), onFrameBufferResizeCallback);
    }

    static void onFrameBufferResizeCallback(GLFWwindow* window, int /*width*/, int /*height*/) {
        auto app = reinterpret_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
        app->frameBufferResized_ = true;
    }

    void createContext() {
        if (config_.logInstanceInfo) {
            logSupportedExtensions();
            logAvailableLayers();
        }

        VulkanContextConfig config;
        config.applicationName = config_.applicationName;
        config.enableValidation = config_.enableValidation;
        config.enableSynchronizationValidation = config_.enableSynchronizationValidation;
        config.onValidationMessage = config_.onValidationMessage;
        config.framesInFlight = config_.framesInFlight;

        if (hasWindow()) {
            uint32_t glfwExtensionCount = 0;
            const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
            config.instanceExtensions.assign(glfwExtensions, glfwExtensions + glfwExtensionCount);

            config.createSurface = [window = window_.get()](VkInstance instance, VkSurfaceKHR* surface) {
                return glfwCreateWindowSurface(instance, window, nullptr, surface);
            };
        }

        context_ = std::make_unique<VulkanContext>(std::move(config));
    }

    void requireWindow() {
        if (!hasWindow()) {
            throw std::logic_error("Headless apps have no window or surface");
        }
    }

    // The window's framebuffer size, in pixels
    VkExtent2D getFramebufferSize() {
        int width = 0, height = 0;
        glfwGetFramebufferSize(window_.get(), &width, &height);
        return {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    }

    void recreateSwapChain() {
        // A minimized window has no area to present to; wait until it is restored
        VkExtent2D size = getFramebufferSize();
        while (size.width == 0 || size.height == 0) {
            glfwWaitEvents(); // Sleep the thread until something happens to the window
            size = getFramebufferSize();
        }
        // The old swapchain's images must no longer be in use
        context_->waitIdleAll();

        const VkExtent2D previousExtent = swapchain_->getExtent();
        swapchain_->recreate(size);

        const VkExtent2D extent = swapchain_->getExtent();
        if (extent.width != previousExtent.width || extent.height != previousExtent.height) {
            for (auto& [id, listener] : surfaceResizeListeners_) {
                listener(extent);
            }
        }
    }

    std::vector<std::unique_ptr<RenderGraph>>::iterator findGraph(RenderGraph& graph) {
        auto it = std::find_if(graphs_.begin(), graphs_.end(), [&graph](auto& owned) { return owned.get() == &graph; });
        if (it == graphs_.end()) {
            throw std::invalid_argument("The graph wasn't created by this app");
        }
        return it;
    }

    void logSupportedExtensions() {
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

    AppConfig config_;

    // GLFW Variables
    std::unique_ptr<GLFWwindow, std::function<void(GLFWwindow*)>> window_;
    bool frameBufferResized_ = false;

    // Instance, device, queues & managed resources. Declared after the window (which its
    // surface depends on) and before everything below, which must be destroyed first.
    std::unique_ptr<VulkanContext> context_;

    // Present only with a window. Declared before the graphs, which present to it.
    std::unique_ptr<Swapchain> swapchain_;

    std::map<ListenerId, SurfaceResizeListener> surfaceResizeListeners_;
    ListenerId nextListenerId_ = 0;

    std::vector<std::unique_ptr<RenderGraph>> graphs_;
    RenderGraph* activeGraph_ = nullptr;
};
