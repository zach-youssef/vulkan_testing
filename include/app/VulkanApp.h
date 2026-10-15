#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "app/Swapchain.h"
#include "core/VkUtil.h"
#include "core/DeviceSelection.h"
#include "resources/Buffer.h"
#include "graph/RenderGraph.h"
#include "resources/Image.h"
#include "core/ResourceManager.h"
#include "core/VulkanContext.h"

#include <glm/glm.hpp>

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
    // The window to present to. Without one the app is headless: there is no surface, swapchain or
    // render pass, and frames are driven by calling drawFrame() (run() requires a window).
    std::optional<WindowConfig> window = WindowConfig{};
    // Turns on the Khronos validation layer (when installed), which reports API misuse to
    // onValidationMessage (by default, std::cerr)
    bool enableValidation = false;
    std::function<void(VkDebugUtilsMessageSeverityFlagBitsEXT, const char*)> onValidationMessage;
    // Prints the available instance extensions and layers on startup
    bool logInstanceInfo = true;
    // Initial number of frames in flight; see VulkanContext::setFramesInFlight
    uint32_t framesInFlight = 2;
    // Creates the render pass with a 32-bit float color attachment instead of the swapchain's format
    bool forceFullFloatColor = false;
};

/*
 Owns a VulkanContext and, unless headless, a window with its swapchain, depth image, render pass
 and framebuffers. Draws frames by running a RenderGraph.

 The swapchain, depth image and framebuffers are recreated whenever the window's framebuffer
 changes size, whether the user resized it or setWindowSize() was called. Anything else sized to
 the surface can follow along with addSurfaceResizeListener().

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
    }

    void init(AppConfig config = {}) {
        config_ = std::move(config);
        if (config_.window) {
            initWindow(*config_.window);
        }
        initVulkan();
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

    void drawFrame() {
        if (!renderGraph_) {
            throw std::logic_error("drawFrame() requires a render graph; see setRenderGraph()");
        }
        const uint32_t frameIndex = context_->currentFrame();

        // Wait for previous frame to complete
        renderGraph_->waitUntilComplete(frameIndex);

        // Perform any pre-draw actions
        for (auto& callback : preDrawCallbacks_) {
            (*callback)(*this, frameIndex);
        }

        // Construct our evaluation context
        RenderEvalContext ctx {
            frameIndex,
            swapchain_ ? swapchain_->getExtent() : VkExtent2D{0, 0},
            0, {},
            swapchain_ ? swapchain_->get() : VK_NULL_HANDLE,
            frameBufferResized_,
        };

        for (auto& framebuffer : swapChainFramebuffers_) {
            ctx.frameBuffers.push_back(**framebuffer);
        }
        if (swapchain_) {
            ctx.renderFinishedSemaphores = swapchain_->getRenderFinishedSemaphores();
        }

        // Attempt to exectue the render graph
        renderGraph_->submit(ctx);

        // The graph might have exited early if the swapchain is out of date
        if (ctx.shouldRecreateSwapChain && hasWindow()) {
            frameBufferResized_ = false;
            recreateSwapChain();
            return;
        }

        // Move on to the next frame (applying any change to the number of frames in flight)
        context_->advanceFrame();
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

private: // Main initialize functions
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

    void initVulkan() {
        createContext();
        if (hasWindow()) {
            swapchain_ = std::make_unique<Swapchain>(*context_, getFramebufferSize());
            createDepthTexture();
            createRenderPass();
            createFramebuffers();
        }
    }

private: // Vulkan Initialization Functions
    void createContext() {
        if (config_.logInstanceInfo) {
            logSupportedExtensions();
            logAvailableLayers();
        }

        VulkanContextConfig config;
        config.applicationName = config_.applicationName;
        config.enableValidation = config_.enableValidation;
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

    void createRenderPass() {
        VK_SUCCESS_OR_THROW(VulkanRenderPass::createColorDepth(renderPass_,
                                                               context_->device(),
                                                               config_.forceFullFloatColor ? VK_FORMAT_R32G32B32A32_SFLOAT
                                                                                           : swapchain_->getFormat(),
                                                               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
                            "Failed to create render pass");
    }

    void createFramebuffers() {
        const VkExtent2D extent = swapchain_->getExtent();
        swapChainFramebuffers_.clear();
        swapChainFramebuffers_.resize(swapchain_->getImageCount());
        for (uint32_t i = 0; i < swapchain_->getImageCount(); ++i) {
            VkImageView attachments[] = {
                swapchain_->getImageView(i),
                depthTexture_->getImageView()
            };

            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = **renderPass_;
            framebufferInfo.attachmentCount = 2;
            framebufferInfo.pAttachments = attachments;
            framebufferInfo.width = extent.width;
            framebufferInfo.height = extent.height;
            framebufferInfo.layers = 1;

            VK_SUCCESS_OR_THROW(VulkanFramebuffer::create(swapChainFramebuffers_[i], context_->device(), framebufferInfo),
                                "Failed to create framebuffer");
        }
    }

    void createDepthTexture() {
        const VkExtent2D extent = swapchain_->getExtent();
        VK_SUCCESS_OR_THROW(Image::create(depthTexture_,
                                          *context_,
                                          extent.width,
                                          extent.height,
                                          VK_FORMAT_D32_SFLOAT,
                                          VK_IMAGE_TILING_OPTIMAL,
                                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                          VK_IMAGE_ASPECT_DEPTH_BIT),
                            "Failed to create depth texture");
    }

private: // Additional helper functions
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
        context_->waitIdleAll();

        const VkExtent2D previousExtent = swapchain_->getExtent();
        swapchain_->recreate(size);
        // Everything sized to the swapchain must follow it
        createDepthTexture();
        createFramebuffers();

        const VkExtent2D extent = swapchain_->getExtent();
        if (extent.width != previousExtent.width || extent.height != previousExtent.height) {
            for (auto& [id, listener] : surfaceResizeListeners_) {
                listener(extent);
            }
        }
    }

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

public:
    // TODO: Replace with CPU render graph nodes
    // Add callback to be called before drawing each frame
    void addPreDrawCallback(std::shared_ptr<std::function<void(VulkanApp&, uint32_t)>>& callback) {
        preDrawCallbacks_.push_back(callback);
    }

    void setRenderGraph(std::unique_ptr<RenderGraph>&& renderGraph) {
        if (renderGraph_) {
            // The previous graph's nodes may still be in use
            context_->waitIdleAll();
        }
        renderGraph_ = std::move(renderGraph);
    }

public: // Public getters
    VulkanContext& getContext() {
        return *context_;
    }

    VkRenderPass getRenderPass() {
        requireWindow();
        return **renderPass_;
    }

    Swapchain& getSwapchain() {
        requireWindow();
        return *swapchain_;
    }
private: // Member variables
    AppConfig config_;

    // GLFW Variables
    std::unique_ptr<GLFWwindow, std::function<void(GLFWwindow*)>> window_;
    bool frameBufferResized_ = false;

    // Instance, device, queues & managed resources. Declared after the window (which its
    // surface depends on) and before everything below, which must be destroyed first.
    std::unique_ptr<VulkanContext> context_;

    // Present only with a window
    std::unique_ptr<Swapchain> swapchain_;
    Handle<Image> depthTexture_;
    // TODO: Move into RenderableNode
    std::unique_ptr<VulkanRenderPass> renderPass_;
    std::vector<std::unique_ptr<VulkanFramebuffer>> swapChainFramebuffers_;

    std::map<ListenerId, SurfaceResizeListener> surfaceResizeListeners_;
    ListenerId nextListenerId_ = 0;

    std::vector<std::shared_ptr<std::function<void(VulkanApp&,uint32_t)>>> preDrawCallbacks_{};
    std::unique_ptr<RenderGraph> renderGraph_{};
};
