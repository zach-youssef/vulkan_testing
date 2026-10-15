#include "VkTestContext.h"
#include "TestShaders.h"
#include "graph/RenderGraph.h"
#include "graph/nodes/DispatchNode.h"
#include "graph/nodes/ReadbackNode.h"
#include "descriptors/DescriptorHeap.h"

#include <cmath>
#include <set>

namespace {

// Dispatches pattern.comp (8x8 workgroups) over a storage image, counting its updates
class PatternMaterial : public ComputeMaterial {
public:
    PatternMaterial(VulkanContext& context, std::vector<MaterialBinding> bindings, uint32_t width, uint32_t height)
    : ComputeMaterial(context, std::move(bindings), testShader(context, "pattern.comp")), width_(width), height_(height) {}

    void update(uint32_t, VkExtent2D) override { ++updates; }

    glm::vec3 getDispatchDimensions() override {
        return glm::vec3{(width_ + 7) / 8, (height_ + 7) / 8, 1};
    }

    int updates = 0;

private:
    uint32_t width_;
    uint32_t height_;
};

// Copies a sampled image into an rgba16f storage image (the example's test.comp, 32x32 workgroups)
class CopyMaterial : public ComputeMaterial {
public:
    CopyMaterial(VulkanContext& context, std::vector<MaterialBinding> bindings, uint32_t width, uint32_t height)
    : ComputeMaterial(context, std::move(bindings), testShader(context, "test.comp")), width_(width), height_(height) {}

    void update(uint32_t, VkExtent2D) override {}

    glm::vec3 getDispatchDimensions() override {
        return glm::vec3{width_ / 32, height_ / 32, 1};
    }

private:
    uint32_t width_;
    uint32_t height_;
};

constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 32;

// Writes pattern.comp's pattern to output
Handle<PatternMaterial> patternMaterial(VulkanContext& ctx, FrameBinding<DescriptorEntry> output) {
    return ctx.resources().create<PatternMaterial>(
        ctx, std::vector<MaterialBinding>{MaterialBinding::storageImage(std::move(output), Access::StorageWrite)},
        kWidth, kHeight);
}

Handle<Image> storageImage(VulkanContext& ctx) {
    Handle<Image> image;
    Image::createStorage(image, ctx, VK_FORMAT_R8G8B8A8_UNORM, kWidth, kHeight);
    return image;
}

bool hasPattern(const std::vector<uint8_t>& pixels) {
    if (pixels.size() != kWidth * kHeight * 4) {
        return false;
    }
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            const uint8_t* pixel = &pixels[(y * kWidth + x) * 4];
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

float halfToFloat(uint16_t half) {
    const int exponent = (half >> 10) & 0x1f;
    const int mantissa = half & 0x3ff;
    float value = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24)
                                : std::ldexp(static_cast<float>(mantissa | 0x400), exponent - 25);
    return (half >> 15) ? -value : value;
}

// Every texel of an rgba16f readback, as floats
std::vector<float> halves(const std::vector<uint8_t>& bytes) {
    std::vector<float> values(bytes.size() / 2);
    for (size_t i = 0; i < values.size(); ++i) {
        uint16_t half;
        memcpy(&half, &bytes[i * 2], 2);
        values[i] = halfToFloat(half);
    }
    return values;
}

} // namespace

TEST_CASE_FIXTURE(GpuTest, "RenderGraph runs a compute node and reads back its output across frames") {
    auto& ctx = context();
    auto image = storageImage(ctx);
    RenderGraph graph(ctx);
    // The material's entry refers to the imported image, so its write is declared on it
    ImageId output = graph.importImage("output", image, ImageState::general());
    graph.add<DispatchNode>("pattern", patternMaterial(ctx, ctx.descriptors().storageImage(image)));
    NodeId readback = graph.add<ReadbackNode>("readback", output);

    for (int frame = 0; frame < 4; ++frame) {
        CAPTURE(frame);
        CHECK_FALSE(graph.execute().skipped);
        CHECK(hasPattern(graph.nodeAs<ReadbackNode>(readback).pixels()));
    }
    // The imported image is back in its resting state, so code outside the graph can read it
    graph.waitIdle();
    CHECK(hasPattern(image->readback(VK_IMAGE_LAYOUT_GENERAL)));
}

TEST_CASE_FIXTURE(GpuTest, "RenderGraph renders into a graph image and samples it in a second pass") {
    auto& ctx = context();
    RenderGraph graph(ctx);
    ImageId cleared = graph.createImage("cleared", FrameMode::Shared, VK_FORMAT_R8G8B8A8_SRGB, FixedExtent{{kWidth, kHeight}});
    ImageId copied = graph.createImage("copied", FrameMode::Shared, VK_FORMAT_R16G16B16A16_SFLOAT, FixedExtent{{kWidth, kHeight}});

    // Pass 1 only clears its target; pass 2 samples it in a compute shader
    RenderTarget target;
    target.color.push_back({cleared, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {0.2f, 0.4f, 0.6f, 1.0f}});
    graph.addGraphics("clear", [&](NodeBuilder& b) { b.renderTo(target); }, nullptr);

    // The graph's entries for its images, which exist once it compiles
    Handle<VulkanSampler> sampler;
    REQUIRE(VulkanSampler::createWithModeAndFilter(sampler, ctx, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_FILTER_NEAREST) == VK_SUCCESS);
    auto material = ctx.resources().create<CopyMaterial>(ctx, std::vector<MaterialBinding>{
        MaterialBinding::sampled(graph.sampledEntry(cleared)),
        MaterialBinding::sampler(ctx.descriptors().sampler(sampler)),
        MaterialBinding::storageImage(graph.storageEntry(copied), Access::StorageWrite),
    }, kWidth, kHeight);
    graph.add<DispatchNode>("copy", material);
    NodeId readback = graph.add<ReadbackNode>("readback", copied);

    for (int frame = 0; frame < 3; ++frame) {
        graph.execute();
    }
    auto values = halves(graph.nodeAs<ReadbackNode>(readback).pixels());
    REQUIRE(values.size() == kWidth * kHeight * 4);
    // The clear color is linear; written to sRGB and sampled back it is (nearly) unchanged
    const float expected[4] = {0.2f, 0.4f, 0.6f, 1.0f};
    int mismatches = 0;
    for (size_t i = 0; i < values.size(); ++i) {
        if (std::abs(values[i] - expected[i % 4]) > 0.01f && mismatches++ < 5) {
            FAIL_CHECK("Component " << i << " = " << values[i] << ", expected " << expected[i % 4]);
        }
    }
    CHECK(mismatches == 0);
}

TEST_CASE_FIXTURE(GpuTest, "RenderGraph runs independent compute work on separate queues") {
    auto& ctx = context();
    std::vector<Handle<Image>> images;
    RenderGraph graph(ctx);
    std::vector<NodeId> dispatches, readbacks;
    for (int i = 0; i < 3; ++i) {
        images.push_back(storageImage(ctx));
        ImageId output = graph.importImage("output " + std::to_string(i), images.back(), ImageState::general());
        dispatches.push_back(graph.add<DispatchNode>("pattern " + std::to_string(i),
                                                     patternMaterial(ctx, ctx.descriptors().storageImage(images.back()))));
        readbacks.push_back(graph.add<ReadbackNode>("readback " + std::to_string(i), output));
    }

    std::set<int32_t> queues;
    for (NodeId dispatch : dispatches) {
        queues.insert(graph.queueOf(dispatch));
    }
    // With enough queues, each independent dispatch gets its own
    CHECK(queues.size() == std::min<uint32_t>(3, std::max<uint32_t>(1, ctx.queueCount() - 1)));

    for (int frame = 0; frame < 3; ++frame) {
        graph.execute();
    }
    for (NodeId readback : readbacks) {
        CHECK(hasPattern(graph.nodeAs<ReadbackNode>(readback).pixels()));
    }
}

TEST_CASE_FIXTURE(GpuTest, "RenderGraph serializes a shared image used on two queues across overlapping frames") {
    auto& ctx = context();
    RenderGraph graph(ctx);
    // A shared graph image written on one queue and copied on another, every frame. Synchronization
    // validation fails the test if frame N+1's write isn't ordered after frame N's copy.
    ImageId shared = graph.createImage("shared", FrameMode::Shared, VK_FORMAT_R8G8B8A8_UNORM, FixedExtent{{kWidth, kHeight}});
    NodeId dispatch = graph.add<DispatchNode>("pattern", patternMaterial(ctx, graph.storageEntry(shared)));
    NodeId readback = graph.add<ReadbackNode>("readback", shared);
    graph.node(readback).onPrimaryQueue();

    if (ctx.queueCount() > 1) {
        CHECK(graph.queueOf(dispatch) != graph.queueOf(readback));
    }
    for (int frame = 0; frame < 5; ++frame) {
        graph.execute();
    }
    CHECK(hasPattern(graph.nodeAs<ReadbackNode>(readback).pixels()));
}

TEST_CASE_FIXTURE(GpuTest, "RenderGraph caches plans per set of enabled nodes") {
    auto& ctx = context();
    RenderGraph graph(ctx);
    ImageId color = graph.createImage("color", FrameMode::Shared, VK_FORMAT_R8G8B8A8_UNORM, FixedExtent{{4, 4}});
    auto clearTo = [&](std::array<float, 4> rgba) {
        RenderTarget target;
        target.color.push_back({color, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, rgba});
        return target;
    };
    // Both clear the same image; the later one wins when enabled
    graph.addGraphics("red", [&](NodeBuilder& b) { b.renderTo(clearTo({1, 0, 0, 1})); }, nullptr);
    NodeId green = graph.addGraphics("green", [&](NodeBuilder& b) { b.renderTo(clearTo({0, 1, 0, 1})); }, nullptr);
    NodeId readback = graph.add<ReadbackNode>("readback", color);
    auto firstPixel = [&] {
        graph.execute();
        auto pixels = graph.nodeAs<ReadbackNode>(readback).pixels();
        return std::vector<uint8_t>(pixels.begin(), pixels.begin() + 4);
    };

    CHECK(firstPixel() == std::vector<uint8_t>{0, 255, 0, 255});
    CHECK(graph.compileCount() == 1);
    graph.setEnabled(green, false);
    CHECK(firstPixel() == std::vector<uint8_t>{255, 0, 0, 255});
    CHECK(graph.compileCount() == 2);
    // Back to a combination compiled before: no compile
    graph.setEnabled(green, true);
    CHECK(firstPixel() == std::vector<uint8_t>{0, 255, 0, 255});
    graph.setEnabled(green, false);
    CHECK(firstPixel() == std::vector<uint8_t>{255, 0, 0, 255});
    CHECK(graph.compileCount() == 2);
    CHECK(graph.cachedPlanCount() == 2);

    // Changing the structure clears the cache
    graph.addCpu("cpu", nullptr, [](FrameContext&) {});
    firstPixel();
    CHECK(graph.cachedPlanCount() == 1);
}

TEST_CASE_FIXTURE(GpuTest, "RenderGraph updates a material shared by several nodes once per frame") {
    auto& ctx = context();
    auto image = storageImage(ctx);
    auto material = patternMaterial(ctx, ctx.descriptors().storageImage(image));
    RenderGraph graph(ctx);
    for (int i = 0; i < 3; ++i) {
        graph.add<DispatchNode>("pattern " + std::to_string(i), material);
    }
    graph.execute();
    graph.execute();
    CHECK(material->updates == 2);
    graph.waitIdle();
}

TEST_CASE_FIXTURE(GpuTest, "RenderGraph runs CPU nodes in graph order") {
    auto& ctx = context();
    RenderGraph graph(ctx);
    std::vector<std::string> ran;
    NodeId second = graph.addCpu("second", nullptr, [&](FrameContext&) { ran.push_back("second"); });
    NodeId first = graph.addCpu("first", nullptr, [&](FrameContext&) { ran.push_back("first"); });
    graph.addDependency(first, second);
    graph.execute();
    CHECK(ran == std::vector<std::string>{"first", "second"});
}

TEST_CASE_FIXTURE(GpuTest, "RenderGraph reports invalid graphs when compiling") {
    auto& ctx = context();
    SUBCASE("cycles") {
        RenderGraph graph(ctx);
        NodeId a = graph.addCpu("a", nullptr, nullptr);
        NodeId b = graph.addCpu("b", nullptr, nullptr);
        graph.addDependency(a, b);
        graph.addDependency(b, a);
        CHECK_THROWS_AS(graph.compile(), GraphError);
    }
    SUBCASE("missing usage") {
        Handle<Image> sampledOnly;
        Image::createEmptyRGBA(sampledOnly, ctx, 4, 4);
        RenderGraph graph(ctx);
        ImageId image = graph.importImage("sampled only", sampledOnly, ImageState::shaderReadOnly());
        graph.addCompute("write", [&](NodeBuilder& b) { b.write(image, Access::StorageWrite); }, nullptr);
        CHECK_THROWS_WITH_AS(graph.compile(), doctest::Contains("wasn't created with"), GraphError);
    }
    SUBCASE("graphics node without a target") {
        RenderGraph graph(ctx);
        graph.addGraphics("draw", nullptr, nullptr);
        CHECK_THROWS_WITH_AS(graph.compile(), doctest::Contains("no render target"), GraphError);
    }
    SUBCASE("read with a writing access") {
        RenderGraph graph(ctx);
        ImageId image = graph.createImage("image", FrameMode::Shared, VK_FORMAT_R8G8B8A8_UNORM, FixedExtent{{4, 4}});
        CHECK_THROWS_AS(graph.addCompute("bad", [&](NodeBuilder& b) { b.read(image, Access::StorageWrite); }, nullptr),
                        GraphError);
    }
    SUBCASE("surface relative images need a surface") {
        RenderGraph graph(ctx);
        CHECK_THROWS_AS(graph.createImage("image", FrameMode::Shared, VK_FORMAT_R8G8B8A8_UNORM, SurfaceRelative{}),
                        GraphError);
    }
}

TEST_CASE_FIXTURE(GpuTest, "RenderGraph keeps per-frame images per frame in flight as the count changes") {
    auto& ctx = context();
    const uint32_t originalFrames = ctx.framesInFlight();
    {
        RenderGraph graph(ctx);
        ImageId image = graph.createImage("image", FrameMode::PerFrame, VK_FORMAT_R8G8B8A8_UNORM, FixedExtent{{4, 4}});
        RenderTarget target;
        target.color.push_back({image, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {0, 0, 1, 1}});
        graph.addGraphics("clear", [&](NodeBuilder& b) { b.renderTo(target); }, nullptr);
        NodeId readback = graph.add<ReadbackNode>("readback", image);
        graph.execute();
        CHECK(graph.image(image, 0) != graph.image(image, 1));

        ctx.setFramesInFlight(3);
        for (int frame = 0; frame < 4; ++frame) {
            graph.execute();
        }
        CHECK(graph.image(image, 2));
        auto pixels = graph.nodeAs<ReadbackNode>(readback).pixels();
        CHECK(std::vector<uint8_t>(pixels.begin(), pixels.begin() + 4) == std::vector<uint8_t>{0, 0, 255, 255});
        ctx.setFramesInFlight(originalFrames);
        graph.execute();
    }
}

TEST_CASE_FIXTURE(GpuTest, "RenderGraph derives uses from materials, importing resources it doesn't know") {
    auto& ctx = context();
    auto image = storageImage(ctx);
    RenderGraph graph(ctx);
    // Not imported: the material's entry imports it (as a storage image, resting in GENERAL)
    graph.add<DispatchNode>("pattern", patternMaterial(ctx, ctx.descriptors().storageImage(image)));
    for (int frame = 0; frame < 2; ++frame) {
        graph.execute();
    }
    graph.waitIdle();
    CHECK(hasPattern(image->readback(VK_IMAGE_LAYOUT_GENERAL)));
}

TEST_CASE_FIXTURE(GpuTest, "RenderGraph rejects an image it knows twice") {
    auto& ctx = context();
    auto image = storageImage(ctx);
    RenderGraph graph(ctx);
    graph.importImage("first", image, ImageState::general());
    graph.importImage("second", image, ImageState::general());
    graph.addCpu("cpu", nullptr, nullptr);
    CHECK_THROWS_WITH_AS(graph.compile(), doctest::Contains("are the same image"), GraphError);
}

TEST_CASE_FIXTURE(GpuTest, "RenderGraph keeps its descriptor entries current as images are recreated") {
    auto& ctx = context();
    const uint32_t originalFrames = ctx.framesInFlight();
    RenderGraph graph(ctx);
    ImageId image = graph.createImage("image", FrameMode::PerFrame, VK_FORMAT_R8G8B8A8_UNORM, FixedExtent{{kWidth, kHeight}});
    auto entries = graph.storageEntry(image);
    graph.add<DispatchNode>("pattern", patternMaterial(ctx, entries));
    NodeId readback = graph.add<ReadbackNode>("readback", image);

    graph.execute();
    graph.execute();
    for (uint32_t frame = 0; frame < originalFrames; ++frame) {
        REQUIRE(entries.get(frame));
        CHECK(entries.get(frame)->image() == graph.image(image, frame));
    }
    CHECK(hasPattern(graph.nodeAs<ReadbackNode>(readback).pixels()));

    // A new frame slot gets its own image, and an entry for it when the slot first runs
    ctx.setFramesInFlight(originalFrames + 1);
    for (uint32_t frame = 0; frame <= originalFrames + 1; ++frame) {
        graph.execute();
    }
    CHECK(entries.get(originalFrames)->image() == graph.image(image, originalFrames));
    CHECK(hasPattern(graph.nodeAs<ReadbackNode>(readback).pixels()));
    ctx.setFramesInFlight(originalFrames);
    graph.execute();
}

TEST_CASE_FIXTURE(GpuTest, "Materials update after the CPU nodes ordered before their nodes") {
    auto& ctx = context();
    RenderGraph graph(ctx);
    ImageId first = graph.createImage("first", FrameMode::Shared, VK_FORMAT_R8G8B8A8_UNORM, FixedExtent{{kWidth, kHeight}});
    ImageId second = graph.createImage("second", FrameMode::Shared, VK_FORMAT_R8G8B8A8_UNORM, FixedExtent{{kWidth, kHeight}});
    auto early = patternMaterial(ctx, graph.storageEntry(first));
    auto late = patternMaterial(ctx, graph.storageEntry(second));

    // A CPU node between two dispatches: the first's material updates before it runs, the
    // second's (which depends on it) after
    int cpuRuns = 0;
    int earlyUpdatesSeen = -1, lateUpdatesSeen = -1;
    graph.add<DispatchNode>("early", early);
    NodeId cpu = graph.addCpu("input", nullptr, [&](FrameContext&) {
        ++cpuRuns;
        earlyUpdatesSeen = early->updates;
        lateUpdatesSeen = late->updates;
    });
    NodeId lateNode = graph.add<DispatchNode>("late", late);
    graph.addDependency(cpu, lateNode);

    for (int frame = 1; frame <= 3; ++frame) {
        graph.execute();
        CAPTURE(frame);
        CHECK(cpuRuns == frame);
        CHECK(earlyUpdatesSeen == frame);
        CHECK(lateUpdatesSeen == frame - 1);
        CHECK(late->updates == frame);
    }

    // Disabled nodes' materials aren't updated
    graph.setEnabled(lateNode, false);
    graph.execute();
    CHECK(late->updates == 3);
    CHECK(early->updates == 4);
    ctx.waitIdleAll();
}
