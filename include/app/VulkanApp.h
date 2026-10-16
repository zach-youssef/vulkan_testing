#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "core/Contract.h"
#include "core/Result.h"
#include "core/VkUtil.h"
#include "core/VulkanContext.h"
#include "graph/RenderGraph.h"
#include "shaders/ShaderLibrary.h"
#include "targets/Swapchain.h"

#include <chrono>

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

// Window input callbacks, called from VulkanApp::runFrame's event polling (on the app's thread)
// with GLFW's arguments. Any may be empty.
struct InputHandlers {
    std::function<void(int key, int scancode, int action, int mods)> key;
    std::function<void(int button, int action, int mods)> mouseButton;
    std::function<void(double x, double y)> cursorPos;      // In screen coordinates
    std::function<void(double xOffset, double yOffset)> scroll;
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
    // Device features the application needs beyond the wrapper's; see VulkanContextConfig::features
    DeviceFeatures features;
    // Directories the shader library loads SPIR-V from (see ShaderLibrary)
    std::vector<std::string> shaderSearchPaths;
    // Reload shaders whose SPIR-V files change on disk, checking between frames (errors are printed
    // to std::cerr). Otherwise call reloadShaders().
    bool autoReloadShaders = false;
};

// What VulkanApp::runFrame did
enum class FrameOutcome {
    Drawn,          // Drew a frame
    WindowClosed,   // The window should close; nothing was drawn
};

/*
 Owns a VulkanContext, unless headless a window and its swapchain, and any number of render graphs.
 Each frame executes the active graph (setActiveGraph).

 init(), drawing frames and run() return Results (see Result.h): run() returns the first error that
 stops it, so a program can end with `return exitStatus(app.run());`. Misuse (drawing without an
 active graph, asking a headless app for its window, ...) is a contract violation.

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

    // Creates the window (unless headless), the context and the swapchain
    Result<void> init(AppConfig config = {}) {
        config_ = std::move(config);
        if (config_.window) {
            TRY(initWindow(*config_.window));
        }
        TRY(createContext());
        if (hasWindow()) {
            TRY_ASSIGN(swapchain_, Swapchain::create(*context_, getFramebufferSize()));
        }
        return {};
    }

    // Draws frames until the window is closed, or a frame fails (returning its error). Requires a
    // window.
    Result<void> run() {
        if (!hasWindow()) {
            contractViolation("run() requires a window; headless apps call drawFrame()");
        }
        while (true) {
            TRY_ASSIGN(const FrameOutcome outcome, runFrame());
            if (outcome == FrameOutcome::WindowClosed) {
                break;
            }
        }
        return vkCheck(context_->waitIdleAll(), "Failed to wait for the device");
    }

    // Processes window events and, unless the window has been asked to close, draws a frame.
    // Headless apps always draw.
    Result<FrameOutcome> runFrame() {
        if (hasWindow()) {
            glfwPollEvents();
            if (glfwWindowShouldClose(window_.get())) {
                return FrameOutcome::WindowClosed;
            }
        }
        TRY(drawFrame());
        return FrameOutcome::Drawn;
    }

    // Waits, without drawing, until the window is closed
    Result<void> holdWindow() {
        while (hasWindow() && !glfwWindowShouldClose(window_.get())) {
            glfwWaitEvents();
        }
        return vkCheck(context_->waitIdleAll(), "Failed to wait for the device");
    }

    // Executes the active graph for one frame, recreating the swapchain afterwards if it no longer
    // matches the window. Requires an active graph.
    Result<void> drawFrame() {
        if (!activeGraph_) {
            contractViolation("drawFrame() requires an active render graph; see createGraph() and setActiveGraph()");
        }
        TRY_ASSIGN(const FrameResult result, activeGraph_->execute());
        if (hasWindow() && (result.surfaceOutOfDate || frameBufferResized_)) {
            frameBufferResized_ = false;
            TRY(recreateSwapChain());
        }
        if (config_.autoReloadShaders) {
            autoReloadShaders();
        }
        return {};
    }

    // Reloads shaders whose SPIR-V changed on disk, rebuilding the pipelines of materials using
    // them. Shaders (and pipelines) that fail to load or build keep their previous versions, and
    // the result lists the errors. Call between frames.
    ShaderLibrary::ReloadResult reloadShaders() {
        return context_->shaders().reload();
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
            contractViolation("The active graph can't be destroyed");
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

    // The GLFW window, for polling input (e.g. glfwGetKey) or other GLFW calls. Its user pointer
    // and callbacks belong to the app: receive input through setInputHandlers.
    GLFWwindow* window() {
        requireWindow();
        return window_.get();
    }

    // Replaces the window's input callbacks
    void setInputHandlers(InputHandlers handlers) {
        requireWindow();
        inputHandlers_ = std::move(handlers);
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
    Result<void> initWindow(const WindowConfig& windowConfig) {
        if (!glfwInit()) {
            return Error(ErrorCode::Window, "Failed to initialize GLFW");
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, windowConfig.resizable ? GLFW_TRUE : GLFW_FALSE);

        GLFWwindow* window = glfwCreateWindow(static_cast<int>(windowConfig.width),
                                              static_cast<int>(windowConfig.height),
                                              windowConfig.title.c_str(), nullptr, nullptr);
        if (!window) {
            glfwTerminate();
            return Error(ErrorCode::Window, "Failed to create window");
        }
        window_ = std::unique_ptr<GLFWwindow, std::function<void(GLFWwindow*)>>(window, [](GLFWwindow* w){
            glfwDestroyWindow(w);
            glfwTerminate();
        });
        glfwSetWindowUserPointer(window_.get(), this);
        glfwSetFramebufferSizeCallback(window_.get(), onFrameBufferResizeCallback);
        glfwSetKeyCallback(window_.get(), [](GLFWwindow* window, int key, int scancode, int action, int mods) {
            if (auto& handler = appOf(window).inputHandlers_.key) handler(key, scancode, action, mods);
        });
        glfwSetMouseButtonCallback(window_.get(), [](GLFWwindow* window, int button, int action, int mods) {
            if (auto& handler = appOf(window).inputHandlers_.mouseButton) handler(button, action, mods);
        });
        glfwSetCursorPosCallback(window_.get(), [](GLFWwindow* window, double x, double y) {
            if (auto& handler = appOf(window).inputHandlers_.cursorPos) handler(x, y);
        });
        glfwSetScrollCallback(window_.get(), [](GLFWwindow* window, double xOffset, double yOffset) {
            if (auto& handler = appOf(window).inputHandlers_.scroll) handler(xOffset, yOffset);
        });
        return {};
    }

    static VulkanApp& appOf(GLFWwindow* window) {
        return *reinterpret_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
    }

    static void onFrameBufferResizeCallback(GLFWwindow* window, int /*width*/, int /*height*/) {
        appOf(window).frameBufferResized_ = true;
    }

    Result<void> createContext() {
        if (config_.logInstanceInfo) {
            TRY(logSupportedExtensions());
            TRY(logAvailableLayers());
        }

        VulkanContextConfig config;
        config.applicationName = config_.applicationName;
        config.enableValidation = config_.enableValidation;
        config.enableSynchronizationValidation = config_.enableSynchronizationValidation;
        config.onValidationMessage = config_.onValidationMessage;
        config.framesInFlight = config_.framesInFlight;
        config.features = config_.features;
        config.shaderSearchPaths = config_.shaderSearchPaths;

        if (hasWindow()) {
            uint32_t glfwExtensionCount = 0;
            const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
            config.instanceExtensions.assign(glfwExtensions, glfwExtensions + glfwExtensionCount);

            config.createSurface = [window = window_.get()](VkInstance instance, VkSurfaceKHR* surface) {
                return glfwCreateWindowSurface(instance, window, nullptr, surface);
            };
        }

        TRY_ASSIGN(context_, VulkanContext::create(std::move(config)));
        return {};
    }

    void requireWindow() {
        if (!hasWindow()) {
            contractViolation("Headless apps have no window or surface");
        }
    }

    // The window's framebuffer size, in pixels
    VkExtent2D getFramebufferSize() {
        int width = 0, height = 0;
        glfwGetFramebufferSize(window_.get(), &width, &height);
        return {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    }

    Result<void> recreateSwapChain() {
        // A minimized window has no area to present to; wait until it is restored
        VkExtent2D size = getFramebufferSize();
        while (size.width == 0 || size.height == 0) {
            glfwWaitEvents(); // Sleep the thread until something happens to the window
            size = getFramebufferSize();
        }
        // The old swapchain's images must no longer be in use
        VK_TRY(context_->waitIdleAll(), "Failed to wait for the device");

        const VkExtent2D previousExtent = swapchain_->getExtent();
        TRY(swapchain_->recreate(size));

        const VkExtent2D extent = swapchain_->getExtent();
        if (extent.width != previousExtent.width || extent.height != previousExtent.height) {
            for (auto& [id, listener] : surfaceResizeListeners_) {
                listener(extent);
            }
        }
        return {};
    }

    // Checks for modified shaders a few times a second
    void autoReloadShaders() {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastShaderCheck_ < std::chrono::milliseconds(250)) {
            return;
        }
        lastShaderCheck_ = now;
        auto result = context_->shaders().reloadModified();
        for (const auto& name : result.reloaded) {
            std::cerr << "Reloaded shader " << name << std::endl;
        }
        for (const auto& error : result.errors) {
            std::cerr << "Shader reload failed: " << error << std::endl;
        }
    }

    std::vector<std::unique_ptr<RenderGraph>>::iterator findGraph(RenderGraph& graph) {
        auto it = std::find_if(graphs_.begin(), graphs_.end(), [&graph](auto& owned) { return owned.get() == &graph; });
        if (it == graphs_.end()) {
            contractViolation("The graph wasn't created by this app");
        }
        return it;
    }

    Result<void> logSupportedExtensions() {
        TRY_ASSIGN(auto extensions, (readVkVector<VkExtensionProperties, const char*>(vkEnumerateInstanceExtensionProperties)));
        std::cout << "Available Extensions: " << std::endl;
        for (const auto& extension : extensions) {
            std::cout << "\t" << extension.extensionName << std::endl;
        }
        return {};
    }

    Result<void> logAvailableLayers() {
        TRY_ASSIGN(auto layers, readVkVector<VkLayerProperties>(vkEnumerateInstanceLayerProperties));
        std::cout << "Available Layers: " << std::endl;
        for (const auto& layerProperties : layers) {
            std::cout << "\t" << layerProperties.layerName << std::endl;
        }
        return {};
    }

    AppConfig config_;

    // GLFW Variables
    std::unique_ptr<GLFWwindow, std::function<void(GLFWwindow*)>> window_;
    bool frameBufferResized_ = false;
    InputHandlers inputHandlers_;

    // Instance, device, queues & managed resources. Declared after the window (which its
    // surface depends on) and before everything below, which must be destroyed first.
    std::unique_ptr<VulkanContext> context_;

    // Present only with a window. Declared before the graphs, which present to it.
    std::unique_ptr<Swapchain> swapchain_;

    std::map<ListenerId, SurfaceResizeListener> surfaceResizeListeners_;
    ListenerId nextListenerId_ = 0;

    std::vector<std::unique_ptr<RenderGraph>> graphs_;
    RenderGraph* activeGraph_ = nullptr;

    std::chrono::steady_clock::time_point lastShaderCheck_{};
};
