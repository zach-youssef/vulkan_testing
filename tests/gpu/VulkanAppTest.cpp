#include "app/VulkanApp.h"
#include "graph/nodes/DispatchNode.h"
#include "descriptors/DescriptorHeap.h"
#include "TestShaders.h"

#include <doctest/doctest.h>

#include <mutex>

namespace {

// Dispatches pattern.comp (8x8 workgroups) over an image
class PatternMaterial : public ComputeMaterial {
public:
    PatternMaterial(VulkanContext& context, std::vector<MaterialBinding> bindings,
                    uint32_t width, uint32_t height)
    : ComputeMaterial(context, std::move(bindings), testShader(context, "pattern.comp")),
    width_(width), height_(height) {}

    void update(uint32_t, VkExtent2D) override {}

    glm::vec3 getDispatchDimensions() override {
        return glm::vec3{(width_ + 7) / 8, (height_ + 7) / 8, 1};
    }

private:
    uint32_t width_;
    uint32_t height_;
};

// Whether an image holds pattern.comp's output
bool hasPattern(Image& image) {
    auto pixels = image.readback(VK_IMAGE_LAYOUT_GENERAL);
    for (uint32_t y = 0; y < image.getHeight(); ++y) {
        for (uint32_t x = 0; x < image.getWidth(); ++x) {
            const uint8_t* pixel = &pixels[(y * image.getWidth() + x) * 4];
            const int expected[4] = {int(x % 256), int(y % 256), int((x + y) % 256), 255};
            for (int channel = 0; channel < 4; ++channel) {
                if (std::abs(pixel[channel] - expected[channel]) > 1) {
                    return false;
                }
            }
        }
    }
    return true;
}

// A headless, validated VulkanApp that records validation errors
struct HeadlessApp {
    std::mutex mutex;
    std::vector<std::string> errors;
    std::unique_ptr<VulkanApp> app = std::make_unique<VulkanApp>();

    explicit HeadlessApp(DeviceFeatures features = {}) {
        AppConfig config;
        config.features = features;
        config.applicationName = "VulkanApp tests";
        config.window = std::nullopt;
        config.enableValidation = true;
        config.logInstanceInfo = false;
        config.shaderSearchPaths = {testShaderDir()};
        config.onValidationMessage = [this](VkDebugUtilsMessageSeverityFlagBitsEXT severity, const char* message) {
            if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
                std::lock_guard<std::mutex> lock(mutex);
                errors.emplace_back(message);
            }
        };
        app->init(config);
    }

    ~HeadlessApp() {
        app.reset(); // Destruction may report errors too
        for (const auto& error : errors) {
            FAIL_CHECK("Vulkan validation error: " << error);
        }
    }
};

// Dispatches small_types.comp once
class SmallTypesMaterial : public ComputeMaterial {
public:
    SmallTypesMaterial(VulkanContext& context, std::vector<MaterialBinding> bindings)
    : ComputeMaterial(context, std::move(bindings), testShader(context, "small_types.comp")) {}

    void update(uint32_t, VkExtent2D) override {}

    glm::vec3 getDispatchDimensions() override {
        return glm::vec3{1, 1, 1};
    }
};

template<typename T>
Handle<Buffer<T>> hostBuffer(VulkanContext& ctx, const std::vector<T>& data) {
    Handle<Buffer<T>> buffer;
    VK_SUCCESS_OR_THROW(Buffer<T>::create(buffer, ctx, data.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
                        "Failed to create buffer");
    buffer->mapAndExecute(0, sizeof(T) * data.size(), [&data](void* mapped) {
        memcpy(mapped, data.data(), sizeof(T) * data.size());
    });
    return buffer;
}

} // namespace

TEST_CASE("A headless VulkanApp has no window, surface or render pass") {
    HeadlessApp headless;
    auto& app = *headless.app;

    CHECK_FALSE(app.hasWindow());
    CHECK_FALSE(app.getContext().hasSurface());
    CHECK_THROWS_AS(app.getSwapchain(), std::logic_error);
    CHECK_THROWS_AS(app.getSurfaceExtent(), std::logic_error);
    CHECK_THROWS_AS(app.setWindowSize(100, 100), std::logic_error);
    CHECK_THROWS_AS(app.window(), std::logic_error);
    CHECK_THROWS_AS(app.setInputHandlers({}), std::logic_error);
    // run() would never return without a window to close
    CHECK_THROWS_AS(app.run(), std::logic_error);
    // There is nothing to draw without an active render graph
    CHECK_THROWS_AS(app.drawFrame(), std::logic_error);
    // Graphs of a headless app have no surface
    RenderGraph& graph = app.createGraph();
    CHECK_FALSE(graph.hasSurface());
    CHECK_THROWS_AS(graph.surface(), std::logic_error);
    CHECK_THROWS_AS(graph.surfaceTarget(), std::logic_error);
}

TEST_CASE("A headless VulkanApp runs a compute-only render graph across frames in flight") {
    HeadlessApp headless;
    auto& app = *headless.app;
    auto& ctx = app.getContext();
    REQUIRE(ctx.framesInFlight() == 2);

    constexpr uint32_t width = 45;
    constexpr uint32_t height = 30;
    // Each frame writes an image of its own
    auto images = ctx.resources().create<PerFrameHandles<Image>>(ctx, [&ctx](uint32_t) {
        Handle<Image> image;
        Image::createStorage(image, ctx, VK_FORMAT_R8G8B8A8_UNORM, width, height);
        return image;
    });
    {
        // An entry per frame for those images; the graph imports them through the material
        auto entries = ctx.resources().create<PerFrameHandles<DescriptorEntry>>(ctx, [&ctx, images](uint32_t frame) {
            return ctx.descriptors().storageImage((*images)[frame]);
        });
        auto material = ctx.resources().create<PatternMaterial>(
            ctx, std::vector<MaterialBinding>{MaterialBinding::storageImage(entries, Access::StorageWrite)}, width, height);

        RenderGraph& graph = app.createGraph();
        graph.add<DispatchNode>("pattern", std::move(material));
        app.setActiveGraph(graph);
    }

    for (int frame = 0; frame < 3; ++frame) {
        CHECK(app.runFrame());
    }
    CHECK(ctx.currentFrame() == 1);

    // Grow to 3 frames: the new frame gets an image (and descriptor set, command buffer and
    // fence) of its own
    ctx.setFramesInFlight(3);
    app.drawFrame();
    REQUIRE(ctx.framesInFlight() == 3);
    REQUIRE(images->size() == 3);
    for (int frame = 0; frame < 3; ++frame) {
        app.drawFrame();
    }
    ctx.waitIdleAll();
    for (uint32_t frame = 0; frame < 3; ++frame) {
        CAPTURE(frame);
        CHECK(hasPattern(*(*images)[frame]));
    }

    // And back down to 1
    ctx.setFramesInFlight(1);
    app.drawFrame();
    app.drawFrame();
    CHECK(images->size() == 1);
    CHECK(ctx.currentFrame() == 0);

    images.reset();
}

TEST_CASE("A VulkanApp switches between graphs and destroys inactive ones") {
    HeadlessApp headless;
    auto& app = *headless.app;
    auto& ctx = app.getContext();

    // Two graphs counting their frames with CPU nodes
    int framesA = 0, framesB = 0;
    RenderGraph& a = app.createGraph();
    a.addCpu("count a", nullptr, [&framesA](FrameContext&) { ++framesA; });
    RenderGraph& b = app.createGraph();
    b.addCpu("count b", nullptr, [&framesB](FrameContext&) { ++framesB; });

    app.setActiveGraph(a);
    app.drawFrame();
    app.drawFrame();
    app.setActiveGraph(b);
    app.drawFrame();
    CHECK(framesA == 2);
    CHECK(framesB == 1);
    CHECK(ctx.frameNumber() == 3);

    CHECK_THROWS_AS(app.destroyGraph(b), std::logic_error);
    app.destroyGraph(a);
    app.drawFrame();
    CHECK(framesB == 2);

    RenderGraph other(ctx);
    CHECK_THROWS_AS(app.setActiveGraph(other), std::invalid_argument);
}

TEST_CASE("Apps can enable device features beyond the wrapper's, e.g. 8- and 16-bit storage") {
    DeviceFeatures features;
    features.core.features.shaderInt16 = VK_TRUE;
    features.vulkan11.storageBuffer16BitAccess = VK_TRUE;
    features.vulkan12.storageBuffer8BitAccess = VK_TRUE;
    features.vulkan12.shaderInt8 = VK_TRUE;
    HeadlessApp headless(features);
    auto& app = *headless.app;
    auto& ctx = app.getContext();
    CHECK(ctx.enabledFeatures().missing(features).empty());
    CHECK(ctx.enabledFeatures().missingRequired().empty());

    std::vector<uint8_t> bytes(64);
    std::vector<uint16_t> halves(16);
    for (uint32_t i = 0; i < 64; ++i) {
        bytes[i] = static_cast<uint8_t>(200 + i);   // Up to 263: wraps past 255 as a byte
    }
    for (uint32_t i = 0; i < 16; ++i) {
        halves[i] = static_cast<uint16_t>(60000 + i);
    }
    auto byteBuffer = hostBuffer(ctx, bytes);
    auto halfBuffer = hostBuffer(ctx, halves);
    auto sumBuffer = hostBuffer(ctx, std::vector<uint32_t>(16, 0));
    {
        auto& heap = ctx.descriptors();
        auto material = ctx.resources().create<SmallTypesMaterial>(ctx, std::vector<MaterialBinding>{
            MaterialBinding::storageBuffer(heap.storageBuffer(byteBuffer)),
            MaterialBinding::storageBuffer(heap.storageBuffer(halfBuffer)),
            MaterialBinding::storageBuffer(heap.storageBuffer(sumBuffer), Access::StorageBufferWrite),
        });
        RenderGraph& graph = app.createGraph();
        graph.add<DispatchNode>("sum", std::move(material));
        app.setActiveGraph(graph);
        app.drawFrame();
        ctx.waitIdleAll();
    }

    std::vector<uint32_t> sums(16);
    sumBuffer->mapAndExecute(0, sizeof(uint32_t) * sums.size(), [&sums](void* mapped) {
        memcpy(sums.data(), mapped, sizeof(uint32_t) * sums.size());
    });
    for (uint32_t i = 0; i < 16; ++i) {
        uint32_t expected = halves[i];
        for (uint32_t j = 0; j < 4; ++j) {
            expected += bytes[i * 4 + j];
        }
        CAPTURE(i);
        CHECK(sums[i] == expected);
    }
}

TEST_CASE("A context requesting a feature the device lacks names it") {
    // geometryShader is unsupported by MoltenVK; skip on devices that have it
    VulkanContextConfig probeConfig;
    VulkanContext probe(probeConfig);
    if (DeviceFeatures::supported(probe.physicalDevice()).core.features.geometryShader) {
        MESSAGE("Skipped: the device supports geometryShader");
        return;
    }

    VulkanContextConfig config;
    config.features.core.features.geometryShader = VK_TRUE;
    CHECK_THROWS_WITH(VulkanContext{config}, doctest::Contains("geometryShader"));
}

TEST_CASE("A windowed VulkanApp passes window input to its handlers") {
    AppConfig config;
    config.applicationName = "VulkanApp tests";
    config.window = WindowConfig{"Input test", 64, 64, false};
    config.enableValidation = true;
    config.logInstanceInfo = false;
    std::vector<std::string> errors;
    config.onValidationMessage = [&errors](VkDebugUtilsMessageSeverityFlagBitsEXT severity, const char* message) {
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
            errors.emplace_back(message);
        }
    };
    {
        VulkanApp app;
        try {
            app.init(config);
        } catch (const std::runtime_error& e) {
            // E.g. no display to open a window on
            MESSAGE("Skipped: " << e.what());
            return;
        }
        GLFWwindow* window = app.window();
        REQUIRE(window != nullptr);

        std::vector<std::string> events;
        app.setInputHandlers(InputHandlers{
            .key = [&events](int key, int, int action, int) {
                events.push_back("key " + std::to_string(key) + " " + std::to_string(action));
            },
            .scroll = [&events](double, double y) { events.push_back("scroll " + std::to_string(int(y))); },
        });

        // GLFW can't synthesize input, so call the callbacks the app installed. Setting a callback
        // returns the previous one, which is restored afterwards.
        auto keyCallback = glfwSetKeyCallback(window, nullptr);
        glfwSetKeyCallback(window, keyCallback);
        auto scrollCallback = glfwSetScrollCallback(window, nullptr);
        glfwSetScrollCallback(window, scrollCallback);
        auto mouseButtonCallback = glfwSetMouseButtonCallback(window, nullptr);
        glfwSetMouseButtonCallback(window, mouseButtonCallback);
        REQUIRE(keyCallback);
        REQUIRE(scrollCallback);
        REQUIRE(mouseButtonCallback);

        keyCallback(window, GLFW_KEY_SPACE, 0, GLFW_PRESS, 0);
        scrollCallback(window, 0.0, -2.0);
        // Handlers left empty are ignored
        mouseButtonCallback(window, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
        CHECK(events == std::vector<std::string>{"key " + std::to_string(GLFW_KEY_SPACE) + " " + std::to_string(GLFW_PRESS),
                                                 "scroll -2"});
    }
    for (const auto& error : errors) {
        FAIL_CHECK("Vulkan validation error: " << error);
    }
}
