#include "app/VulkanApp.h"
#include "graph/nodes/ComputeNode.h"
#include "TestShaders.h"

#include <doctest/doctest.h>

#include <mutex>

namespace {

// Dispatches pattern.comp (8x8 workgroups) over an image
class PatternMaterial : public ComputeMaterial {
public:
    PatternMaterial(VulkanContext& context, std::vector<Handle<Descriptor>> descriptors,
                    uint32_t width, uint32_t height)
    : ComputeMaterial(context, descriptors, loadTestShader("pattern.comp")),
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

    HeadlessApp() {
        AppConfig config;
        config.applicationName = "VulkanApp tests";
        config.window = std::nullopt;
        config.enableValidation = true;
        config.logInstanceInfo = false;
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

} // namespace

TEST_CASE("A headless VulkanApp has no window, surface or render pass") {
    HeadlessApp headless;
    auto& app = *headless.app;

    CHECK_FALSE(app.hasWindow());
    CHECK_FALSE(app.getContext().hasSurface());
    CHECK_THROWS_AS(app.getRenderPass(), std::logic_error);
    CHECK_THROWS_AS(app.getSwapchain(), std::logic_error);
    CHECK_THROWS_AS(app.getSurfaceExtent(), std::logic_error);
    CHECK_THROWS_AS(app.setWindowSize(100, 100), std::logic_error);
    // run() would never return without a window to close
    CHECK_THROWS_AS(app.run(), std::logic_error);
    // There is nothing to draw without a render graph
    CHECK_THROWS_AS(app.drawFrame(), std::logic_error);
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
        std::vector<Handle<Descriptor>> descriptors{
            ctx.resources().create<StorageImageDescriptor>(VK_SHADER_STAGE_COMPUTE_BIT, images),
        };
        auto material = ctx.resources().create<PatternMaterial>(ctx, descriptors, width, height);

        auto graph = std::make_unique<RenderGraph>(ctx);
        auto node = graph->addNode(std::make_unique<ComputeNode>(std::move(material), ctx));
        REQUIRE(graph->flagNodeAsFrameBlocking(node) == 0);
        app.setRenderGraph(std::move(graph));
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
