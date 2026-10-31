#include "VkTestContext.h"
#include "TestShaders.h"
#include "graph/RenderGraph.h"
#include "graph/nodes/DispatchNode.h"
#include "graph/nodes/BufferReadbackNode.h"
#include "graph/nodes/ReadbackNode.h"
#include "descriptors/DescriptorHeap.h"

#include <cmath>
#include <cstring>
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

// Dispatches a shader a fixed number of workgroups
class FixedComputeMaterial : public ComputeMaterial {
public:
    FixedComputeMaterial(VulkanContext& context, std::vector<MaterialBinding> bindings, Handle<Shader> shader, glm::vec3 groups)
    : ComputeMaterial(context, std::move(bindings), std::move(shader)), groups_(groups) {}

    void update(uint32_t, VkExtent2D) override {}

    glm::vec3 getDispatchDimensions() override { return groups_; }

private:
    glm::vec3 groups_;
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
    auto image = Image::createStorage(ctx, VK_FORMAT_R8G8B8A8_UNORM, kWidth, kHeight).value();
    return image;
}

bool hasPattern(const std::vector<uint8_t>& pixels, uint32_t width = kWidth, uint32_t height = kHeight) {
    if (pixels.size() != width * height * 4) {
        return false;
    }
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* pixel = &pixels[(y * width + x) * 4];
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
        CHECK_FALSE(graph.execute().value().skipped);
        CHECK(hasPattern(graph.nodeAs<ReadbackNode>(readback).pixels().value()));
    }
    // The imported image is back in its resting state, so code outside the graph can read it
    graph.waitIdle().value();
    CHECK(hasPattern(image->readback(VK_IMAGE_LAYOUT_GENERAL).value()));
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
    auto sampler = VulkanSampler::createWithModeAndFilter(ctx, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_FILTER_NEAREST).value();
    auto material = ctx.resources().create<CopyMaterial>(ctx, std::vector<MaterialBinding>{
        MaterialBinding::sampled(graph.sampledEntry(cleared)),
        MaterialBinding::sampler(ctx.descriptors().sampler(sampler)),
        MaterialBinding::storageImage(graph.storageEntry(copied), Access::StorageWrite),
    }, kWidth, kHeight);
    graph.add<DispatchNode>("copy", material);
    NodeId readback = graph.add<ReadbackNode>("readback", copied);

    for (int frame = 0; frame < 3; ++frame) {
        graph.execute().value();
    }
    auto values = halves(graph.nodeAs<ReadbackNode>(readback).pixels().value());
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
        queues.insert(graph.queueOf(dispatch).value());
    }
    // With enough queues, each independent dispatch gets its own
    CHECK(queues.size() == std::min<uint32_t>(3, std::max<uint32_t>(1, ctx.queueCount() - 1)));

    for (int frame = 0; frame < 3; ++frame) {
        graph.execute().value();
    }
    for (NodeId readback : readbacks) {
        CHECK(hasPattern(graph.nodeAs<ReadbackNode>(readback).pixels().value()));
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
        CHECK(graph.queueOf(dispatch).value() != graph.queueOf(readback).value());
    }
    for (int frame = 0; frame < 5; ++frame) {
        graph.execute().value();
    }
    CHECK(hasPattern(graph.nodeAs<ReadbackNode>(readback).pixels().value()));
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
        graph.execute().value();
        auto pixels = graph.nodeAs<ReadbackNode>(readback).pixels().value();
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
    graph.execute().value();
    graph.execute().value();
    CHECK(material->updates == 2);
    graph.waitIdle().value();
}

TEST_CASE_FIXTURE(GpuTest, "RenderGraph runs CPU nodes in graph order") {
    auto& ctx = context();
    RenderGraph graph(ctx);
    std::vector<std::string> ran;
    NodeId second = graph.addCpu("second", nullptr, [&](FrameContext&) { ran.push_back("second"); });
    NodeId first = graph.addCpu("first", nullptr, [&](FrameContext&) { ran.push_back("first"); });
    graph.addDependency(first, second);
    graph.execute().value();
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
        CHECK_ERROR(graph.compile(), ErrorCode::InvalidGraph, "cycle");
    }
    SUBCASE("missing usage") {
        auto sampledOnly = Image::createEmptyRGBA(ctx, 4, 4).value();
        RenderGraph graph(ctx);
        ImageId image = graph.importImage("sampled only", sampledOnly, ImageState::shaderReadOnly());
        graph.addCompute("write", [&](NodeBuilder& b) { b.write(image, Access::StorageWrite); }, nullptr);
        CHECK_ERROR(graph.compile(), ErrorCode::InvalidGraph, "wasn't created with");
    }
    SUBCASE("graphics node without a target") {
        RenderGraph graph(ctx);
        graph.addGraphics("draw", nullptr, nullptr);
        CHECK_ERROR(graph.compile(), ErrorCode::InvalidGraph, "no render target");
    }
    SUBCASE("read with a writing access") {
        RenderGraph graph(ctx);
        ImageId image = graph.createImage("image", FrameMode::Shared, VK_FORMAT_R8G8B8A8_UNORM, FixedExtent{{4, 4}});
        CHECK_THROWS_AS(graph.addCompute("bad", [&](NodeBuilder& b) { b.read(image, Access::StorageWrite); }, nullptr),
                        ContractViolation);
    }
    SUBCASE("surface relative images need a surface, or a headless surface extent") {
        RenderGraph graph(ctx);
        CHECK_THROWS_WITH_AS(graph.createImage("image", FrameMode::Shared, VK_FORMAT_R8G8B8A8_UNORM, SurfaceRelative{}),
                             doctest::Contains("setSurfaceExtent"), ContractViolation);
        CHECK_THROWS_AS(graph.setSurfaceExtent({0, 4}), ContractViolation);
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
        graph.execute().value();
        CHECK(graph.image(image, 0) != graph.image(image, 1));

        ctx.setFramesInFlight(3);
        for (int frame = 0; frame < 4; ++frame) {
            graph.execute().value();
        }
        CHECK(graph.image(image, 2));
        auto pixels = graph.nodeAs<ReadbackNode>(readback).pixels().value();
        CHECK(std::vector<uint8_t>(pixels.begin(), pixels.begin() + 4) == std::vector<uint8_t>{0, 0, 255, 255});
        ctx.setFramesInFlight(originalFrames);
        graph.execute().value();
    }
}

TEST_CASE_FIXTURE(GpuTest, "RenderGraph derives uses from materials, importing resources it doesn't know") {
    auto& ctx = context();
    auto image = storageImage(ctx);
    RenderGraph graph(ctx);
    // Not imported: the material's entry imports it (as a storage image, resting in GENERAL)
    graph.add<DispatchNode>("pattern", patternMaterial(ctx, ctx.descriptors().storageImage(image)));
    for (int frame = 0; frame < 2; ++frame) {
        graph.execute().value();
    }
    graph.waitIdle().value();
    CHECK(hasPattern(image->readback(VK_IMAGE_LAYOUT_GENERAL).value()));
}

TEST_CASE_FIXTURE(GpuTest, "RenderGraph rejects an image it knows twice") {
    auto& ctx = context();
    auto image = storageImage(ctx);
    RenderGraph graph(ctx);
    graph.importImage("first", image, ImageState::general());
    graph.importImage("second", image, ImageState::general());
    graph.addCpu("cpu", nullptr, nullptr);
    CHECK_ERROR(graph.compile(), ErrorCode::InvalidGraph, "are the same image");
}

TEST_CASE_FIXTURE(GpuTest, "RenderGraph keeps its descriptor entries current as images are recreated") {
    auto& ctx = context();
    const uint32_t originalFrames = ctx.framesInFlight();
    RenderGraph graph(ctx);
    ImageId image = graph.createImage("image", FrameMode::PerFrame, VK_FORMAT_R8G8B8A8_UNORM, FixedExtent{{kWidth, kHeight}});
    auto entries = graph.storageEntry(image);
    graph.add<DispatchNode>("pattern", patternMaterial(ctx, entries));
    NodeId readback = graph.add<ReadbackNode>("readback", image);

    graph.execute().value();
    graph.execute().value();
    for (uint32_t frame = 0; frame < originalFrames; ++frame) {
        REQUIRE(entries.get(frame));
        CHECK(entries.get(frame)->image() == graph.image(image, frame));
    }
    CHECK(hasPattern(graph.nodeAs<ReadbackNode>(readback).pixels().value()));

    // A new frame slot gets its own image, and an entry for it when the slot first runs
    ctx.setFramesInFlight(originalFrames + 1);
    for (uint32_t frame = 0; frame <= originalFrames + 1; ++frame) {
        graph.execute().value();
    }
    CHECK(entries.get(originalFrames)->image() == graph.image(image, originalFrames));
    CHECK(hasPattern(graph.nodeAs<ReadbackNode>(readback).pixels().value()));
    ctx.setFramesInFlight(originalFrames);
    graph.execute().value();
}

TEST_CASE_FIXTURE(GpuTest, "RenderGraph recreates buffers sized each frame when their size changes") {
    auto& ctx = context();
    RenderGraph graph(ctx);
    uint32_t count = 4;
    int sized = 0;
    BufferId buffer = graph.createBuffer("values", FrameMode::Shared, [&count, &sized](const FrameContext&) {
        ++sized;
        return VkDeviceSize(count) * sizeof(uint32_t);
    });   // Its entries, which no node uses here, give it storage usage
    auto entries = graph.storageBufferEntry(buffer);
    graph.addTransfer("fill", [buffer](NodeBuilder& builder) { builder.write(buffer, Access::TransferDst); },
                      [buffer](RecordContext& record) {
                          vkCmdFillBuffer(record.commandBuffer, record.graph.buffer(buffer, record.frameIndex), 0,
                                          VK_WHOLE_SIZE, 7);
                      });
    NodeId readback = graph.add<BufferReadbackNode>("readback", buffer);
    auto contents = [&]() { return graph.nodeAs<BufferReadbackNode>(readback).contents<uint32_t>().value(); };

    graph.execute().value();
    CHECK(contents() == std::vector<uint32_t>(4, 7));
    CHECK(sized >= 1);
    const VkBuffer first = graph.buffer(buffer, ctx.currentFrame());

    // Unchanged: the same buffer
    graph.execute().value();
    CHECK(graph.buffer(buffer, ctx.currentFrame()) == first);

    // Grown: a new buffer, which the graph's entries follow
    count = 10;
    graph.execute().value();
    CHECK(graph.bufferSize(buffer, ctx.currentFrame()) == 10 * sizeof(uint32_t));
    CHECK(contents() == std::vector<uint32_t>(10, 7));
    const uint32_t frame = ctx.currentFrame();
    graph.execute().value();
    REQUIRE(entries.get(frame));
    CHECK(entries.get(frame)->buffer() == graph.buffer(buffer, frame));
    CHECK(entries.get(frame)->buffer() != first);

    // Empty, then shrunk
    count = 0;
    graph.execute().value();
    CHECK(graph.bufferSize(buffer, ctx.currentFrame()) == 0);
    CHECK(contents().empty());
    count = 3;
    graph.execute().value();
    CHECK(contents() == std::vector<uint32_t>(3, 7));
    // Replaced buffers are destroyed once the frames using them finish
    ctx.waitIdleAll();
    ctx.collectRetired();
}

namespace {
// Fills buffers of size bytes with a pattern and frees them, so that a buffer allocated next whose
// memory isn't zeroed likely shows it (lavapipe reuses freed host memory)
void leaveGarbage(VulkanContext& ctx, VkDeviceSize size, VkMemoryPropertyFlags memory) {
    std::vector<std::unique_ptr<Buffer<uint8_t>>> scratch;
    for (int i = 0; i < 8; ++i) {
        scratch.push_back(Buffer<uint8_t>::createScoped(ctx, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, memory).value());
        if (memory & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
            scratch.back()->mapAndExecute(0, VK_WHOLE_SIZE, [size](void* data) { std::memset(data, 0xA5, size); }).value();
        } else {
            ctx.submitImmediate([buffer = scratch.back()->getBuffer()](VkCommandBuffer commandBuffer) {
                vkCmdFillBuffer(commandBuffer, buffer, 0, VK_WHOLE_SIZE, 0xA5A5A5A5u);
            }).value();
        }
    }
}
} // namespace

TEST_CASE_FIXTURE(GpuTest, "RenderGraph's buffers start zeroed, when created and when recreated") {
    auto& ctx = context();
    const uint32_t originalFrames = ctx.framesInFlight();
    {
        RenderGraph graph(ctx);
        uint32_t count = 256;
        auto size = [&count](const FrameContext&) { return VkDeviceSize(count) * sizeof(uint32_t); };
        leaveGarbage(ctx, count * sizeof(uint32_t), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        const BufferId shared = graph.createBuffer("shared", FrameMode::Shared, size);
        const BufferId perFrame = graph.createBuffer("perFrame", FrameMode::PerFrame, size);
        // Read before anything writes them; a fill (of 7s) enabled later shows the reads see writes
        const NodeId readShared = graph.add<BufferReadbackNode>("readShared", shared);
        const NodeId readPerFrame = graph.add<BufferReadbackNode>("readPerFrame", perFrame);
        const NodeId fill = graph.addTransfer("fill", [&](NodeBuilder& builder) {
            builder.write(shared, Access::TransferDst);
            builder.write(perFrame, Access::TransferDst);
        }, [&](RecordContext& record) {
            for (BufferId buffer : {shared, perFrame}) {
                vkCmdFillBuffer(record.commandBuffer, record.graph.buffer(buffer, record.frameIndex), 0, VK_WHOLE_SIZE, 7);
            }
        });
        graph.setEnabled(fill, false);
        auto check = [&](uint32_t value) {
            const std::vector<uint32_t> expected(count, value);
            CHECK(graph.nodeAs<BufferReadbackNode>(readShared).contents<uint32_t>().value() == expected);
            CHECK(graph.nodeAs<BufferReadbackNode>(readPerFrame).contents<uint32_t>().value() == expected);
        };

        // Created: every frame slot's copy is zero when first read
        for (uint32_t frame = 0; frame < originalFrames; ++frame) {
            CAPTURE(frame);
            graph.execute().value();
            check(0);
        }

        // Written: every copy holds 7s
        graph.setEnabled(fill, true);
        for (uint32_t frame = 0; frame < originalFrames; ++frame) {
            graph.execute().value();
        }
        graph.setEnabled(fill, false);
        for (uint32_t frame = 0; frame < originalFrames; ++frame) {
            graph.execute().value();
            check(7);
        }

        // Recreated (resized): zero again, nothing of the old contents or of freed memory
        count = 384;
        graph.waitIdle().value();
        ctx.collectRetired();
        leaveGarbage(ctx, count * sizeof(uint32_t), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        for (uint32_t frame = 0; frame < originalFrames; ++frame) {
            CAPTURE(frame);
            graph.execute().value();
            check(0);
        }

        // A frame slot added later gets a zeroed copy too, while the others keep what was written
        graph.setEnabled(fill, true);
        for (uint32_t frame = 0; frame < originalFrames; ++frame) {
            graph.execute().value();
        }
        graph.setEnabled(fill, false);
        graph.waitIdle().value();
        leaveGarbage(ctx, count * sizeof(uint32_t), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        ctx.setFramesInFlight(originalFrames + 1);
        for (uint32_t frame = 0; frame <= originalFrames; ++frame) {
            const uint32_t slot = ctx.currentFrame();
            CAPTURE(slot);
            graph.execute().value();
            CHECK(graph.nodeAs<BufferReadbackNode>(readShared).contents<uint32_t>().value() == std::vector<uint32_t>(count, 7));
            CHECK(graph.nodeAs<BufferReadbackNode>(readPerFrame).contents<uint32_t>().value()
                  == std::vector<uint32_t>(count, slot < originalFrames ? 7 : 0));
        }
        graph.waitIdle().value();
        ctx.setFramesInFlight(originalFrames);

        // Host-visible buffers are zeroed when created, before any frame
        const VkMemoryPropertyFlags hostMemory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        leaveGarbage(ctx, 1024, hostMemory);
        const BufferId host = graph.createBuffer("host", FrameMode::Shared, 1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostMemory);
        graph.compile().value();
        std::vector<uint8_t> hostBytes(1024, 1);
        graph.createdBuffer(host, ctx.currentFrame())->mapAndExecute(0, VK_WHOLE_SIZE, [&](void* data) {
            std::memcpy(hostBytes.data(), data, hostBytes.size());
        }).value();
        CHECK(hostBytes == std::vector<uint8_t>(1024, 0));
        graph.execute().value();
        graph.waitIdle().value();
    }
    ctx.waitIdleAll();
    ctx.collectRetired();
}

TEST_CASE_FIXTURE(GpuTest, "RenderGraph zeroes buffers before uses in later submissions, and keeps what code writes first") {
    auto& ctx = context();
    {
        leaveGarbage(ctx, 8, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        RenderGraph graph(ctx);
        const BufferId other = graph.createBuffer("other", FrameMode::Shared, 16);
        // Not whole words: every byte is zeroed all the same
        const BufferId late = graph.createBuffer("late", FrameMode::Shared, 6);
        const BufferId uploaded = graph.createBuffer("uploaded", FrameMode::Shared, 16 * sizeof(uint32_t),
                                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        // The frame's first submission (which zeroes the buffers), then a CPU node, then the reads in
        // a later submission (on another queue, if the device has one)
        const NodeId first = graph.addTransfer("first", [&](NodeBuilder& builder) { builder.write(other, Access::TransferDst); },
                                               [&](RecordContext& record) {
                                                   vkCmdFillBuffer(record.commandBuffer, record.graph.buffer(other, record.frameIndex),
                                                                   0, VK_WHOLE_SIZE, 3);
                                               });
        const NodeId between = graph.addCpu("between", nullptr, [](FrameContext&) {});
        const NodeId readLate = graph.add<BufferReadbackNode>("readLate", late);
        const NodeId readUploaded = graph.add<BufferReadbackNode>("readUploaded", uploaded);
        graph.addDependency(first, between);
        graph.addDependency(between, readLate);
        graph.addDependency(between, readUploaded);
        graph.compile().value();
        CAPTURE(graph.queueOf(first).value());
        CAPTURE(graph.queueOf(readLate).value());

        // Code writing a graph buffer before the first frame keeps what it wrote
        auto handle = graph.createdBuffer(uploaded, ctx.currentFrame());
        ctx.submitImmediate([buffer = handle->getBuffer()](VkCommandBuffer commandBuffer) {
            vkCmdFillBuffer(commandBuffer, buffer, 0, VK_WHOLE_SIZE, 9);
        }).value();

        for (int frame = 0; frame < 2; ++frame) {
            CAPTURE(frame);
            graph.execute().value();
            CHECK(graph.nodeAs<BufferReadbackNode>(readLate).bytes().value() == std::vector<uint8_t>(6, 0));
            CHECK(graph.nodeAs<BufferReadbackNode>(readUploaded).contents<uint32_t>().value() == std::vector<uint32_t>(16, 9));
        }
        graph.waitIdle().value();
    }
    ctx.waitIdleAll();
    ctx.collectRetired();
}

TEST_CASE_FIXTURE(GpuTest, "Materials bind arrays of entries, whose elements a node uses") {
    auto& ctx = context();
    {
        RenderGraph graph(ctx);
        // Graph buffers only the array reads: their usage and order come from its uses
        std::vector<BufferId> sources;
        std::vector<FrameBinding<DescriptorEntry>> elements;
        for (uint32_t i = 0; i < 2; ++i) {
            sources.push_back(graph.createBuffer("source" + std::to_string(i), FrameMode::Shared, sizeof(uint32_t)));
            elements.push_back(graph.storageBufferEntry(sources.back()));
        }
        // An element can repeat
        elements.push_back(elements[0]);
        BufferId target = graph.createBuffer("target", FrameMode::Shared, 3 * sizeof(uint32_t));
        for (uint32_t i = 0; i < 2; ++i) {
            graph.addTransfer("fill" + std::to_string(i), [source = sources[i]](NodeBuilder& builder) {
                builder.write(source, Access::TransferDst);
            }, [source = sources[i], value = i + 1](RecordContext& record) {
                vkCmdFillBuffer(record.commandBuffer, record.graph.buffer(source, record.frameIndex), 0, VK_WHOLE_SIZE, value);
            });
        }
        auto array = DescriptorArray::create(ctx, elements).value();
        auto material = ctx.resources().create<FixedComputeMaterial>(ctx, std::vector<MaterialBinding>{
            MaterialBinding::array(array, Access::StorageBufferRead),
            MaterialBinding::storageBuffer(graph.storageBufferEntry(target), Access::StorageBufferWrite),
        }, testShader(ctx, "descriptor_array.comp"), glm::vec3{1, 1, 1});
        graph.add<DispatchNode>("read", material);
        NodeId readback = graph.add<BufferReadbackNode>("readback", target);

        graph.execute().value();
        CHECK(graph.nodeAs<BufferReadbackNode>(readback).contents<uint32_t>().value() == std::vector<uint32_t>{301, 302, 301});
        // The indices are written once per frame slot, and again only when they change
        for (uint32_t frame = 0; frame < 2 * ctx.framesInFlight(); ++frame) {
            graph.execute().value();
        }
        CHECK(array->writes() == ctx.framesInFlight());
        CHECK(graph.nodeAs<BufferReadbackNode>(readback).contents<uint32_t>().value() == std::vector<uint32_t>{301, 302, 301});
        ctx.waitIdleAll();
    }
    {
        // Elements changed at runtime: more of them than the buffers held, including a buffer only
        // the new elements use, which the graph synchronizes with its fill (its uses are declared
        // again; synchronization validation would report the hazard otherwise)
        RenderGraph graph(ctx);
        std::vector<BufferId> sources;
        std::vector<FrameBinding<DescriptorEntry>> entries;
        for (uint32_t i = 0; i < 3; ++i) {
            sources.push_back(graph.createBuffer("source" + std::to_string(i), FrameMode::Shared, sizeof(uint32_t)));
            entries.push_back(graph.storageBufferEntry(sources.back()));
            graph.addTransfer("fill" + std::to_string(i), [source = sources[i]](NodeBuilder& builder) {
                builder.write(source, Access::TransferDst);
            }, [source = sources[i], value = i + 1](RecordContext& record) {
                vkCmdFillBuffer(record.commandBuffer, record.graph.buffer(source, record.frameIndex), 0, VK_WHOLE_SIZE, value);
            });
        }
        BufferId target = graph.createBuffer("target", FrameMode::Shared, 8 * sizeof(uint32_t));
        auto array = DescriptorArray::create(ctx, {entries[0]}).value();
        auto material = ctx.resources().create<FixedComputeMaterial>(ctx, std::vector<MaterialBinding>{
            MaterialBinding::array(array, Access::StorageBufferRead),
            MaterialBinding::storageBuffer(graph.storageBufferEntry(target), Access::StorageBufferWrite),
        }, testShader(ctx, "descriptor_array.comp"), glm::vec3{1, 1, 1});
        graph.add<DispatchNode>("read", material);
        NodeId readback = graph.add<BufferReadbackNode>("readback", target);
        auto contents = [&]() {
            auto values = graph.nodeAs<BufferReadbackNode>(readback).contents<uint32_t>().value();
            values.resize(array->size());
            return values;
        };

        graph.execute().value();
        CHECK(contents() == std::vector<uint32_t>{101});

        const uint64_t version = array->version();
        array->setElements({entries[2], entries[0], entries[1], entries[2]});
        CHECK(array->version() != version);
        for (uint32_t frame = 0; frame < ctx.framesInFlight() + 1; ++frame) {
            graph.execute().value();
            CHECK(contents() == std::vector<uint32_t>{403, 401, 402, 403});
        }

        // Fewer again: the buffers keep their size
        array->setElements({entries[1]});
        graph.execute().value();
        CHECK(contents() == std::vector<uint32_t>{102});

        // Chosen each frame: the array changes (its version with it) only when the choice does
        std::vector<uint32_t> chosen{0, 2};
        graph.setElementSource(array, [&](const FrameContext&) {
            std::vector<FrameBinding<DescriptorEntry>> elements;
            for (uint32_t i : chosen) {
                elements.push_back(entries[i]);
            }
            return elements;
        });
        graph.execute().value();
        CHECK(contents() == std::vector<uint32_t>{201, 203});
        const uint64_t chosenVersion = array->version();
        graph.execute().value();
        CHECK(array->version() == chosenVersion);
        chosen = {1, 1, 0, 2, 2};
        graph.execute().value();
        CHECK(contents() == std::vector<uint32_t>{502, 502, 501, 503, 503});
        chosen = {};
        graph.execute().value();
        CHECK(array->size() == 0);
        CHECK_THROWS_AS(graph.setElementSource(array, nullptr), ContractViolation);
        ctx.waitIdleAll();
        ctx.collectRetired();
    }
    {
        // An empty array holds a count of 0
        auto array = DescriptorArray::create(ctx, {}).value();
        CHECK(array->size() == 0);
        array->update(0);
        CHECK(array->writes() == 1);
    }
    CHECK_THROWS_AS((void)MaterialBinding::array(nullptr, Access::SampledRead), ContractViolation);
}

TEST_CASE_FIXTURE(GpuTest, "RenderGraph enables and runs once a node's followers with it") {
    auto& ctx = context();
    RenderGraph graph(ctx);
    int leaderRuns = 0;
    int followerRuns = 0;
    int lastRuns = 0;
    NodeId leader = graph.addCpu("leader", nullptr, [&leaderRuns](FrameContext&) { ++leaderRuns; });
    NodeId follower = graph.addCpu("follower", nullptr, [&followerRuns](FrameContext&) { ++followerRuns; });
    NodeId last = graph.addCpu("last", nullptr, [&lastRuns](FrameContext&) { ++lastRuns; });

    // A follower takes its leader's state when it starts following
    graph.setEnabled(leader, false);
    graph.enableWith(follower, leader);
    CHECK_FALSE(graph.isEnabled(follower));
    // Followers of followers follow too
    graph.enableWith(last, follower);
    CHECK_FALSE(graph.isEnabled(last));
    graph.execute().value();
    CHECK(followerRuns == 0);

    graph.setEnabled(leader, true);
    CHECK(graph.isEnabled(follower));
    CHECK(graph.isEnabled(last));
    graph.execute().value();
    CHECK(followerRuns == 1);
    CHECK(lastRuns == 1);

    graph.setEnabled(leader, false);
    graph.runOnce(leader);
    CHECK(graph.isPendingOnce(follower));
    graph.execute().value();
    graph.execute().value();
    CHECK(leaderRuns == 2);
    CHECK(followerRuns == 2);
    CHECK(lastRuns == 2);
    CHECK_FALSE(graph.isEnabled(follower));

    // A follower can still be toggled alone
    graph.setEnabled(follower, true);
    CHECK_FALSE(graph.isEnabled(leader));
    CHECK(graph.isEnabled(last));

    // One leader each, and no cycles
    CHECK_THROWS_AS(graph.enableWith(follower, last), ContractViolation);
    CHECK_THROWS_AS(graph.enableWith(leader, last), ContractViolation);
    CHECK_THROWS_AS(graph.enableWith(leader, leader), ContractViolation);
    graph.waitIdle().value();
}

TEST_CASE_FIXTURE(GpuTest, "RenderGraph runs a node once, and buffer readbacks give finished copies without waiting") {
    auto& ctx = context();
    {
        RenderGraph graph(ctx);
        BufferId buffer = graph.createBuffer("values", FrameMode::Shared, 2 * sizeof(uint32_t));
        uint32_t value = 1;
        NodeId fill = graph.addTransfer("fill", [buffer](NodeBuilder& builder) { builder.write(buffer, Access::TransferDst); },
                                        [buffer, &value](RecordContext& record) {
                                            vkCmdFillBuffer(record.commandBuffer, record.graph.buffer(buffer, record.frameIndex), 0,
                                                            VK_WHOLE_SIZE, value);
                                        });
        int runs = 0;
        NodeId count = graph.addCpu("count", nullptr, [&runs](FrameContext&) { ++runs; });
        NodeId readback = graph.add<BufferReadbackNode>("readback", buffer);
        auto& node = graph.nodeAs<BufferReadbackNode>(readback);

        // Nothing has run, so nothing has finished
        CHECK_FALSE(node.takeFinished<uint32_t>().value());

        graph.runOnce(count);
        CHECK(graph.isPendingOnce(count));
        graph.execute().value();
        CHECK(runs == 1);
        CHECK_FALSE(graph.isEnabled(count));
        CHECK_FALSE(graph.isPendingOnce(count));
        graph.execute().value();
        CHECK(runs == 1);
        // Again, and cancelled by setEnabled
        graph.runOnce(count);
        graph.execute().value();
        CHECK(runs == 2);
        graph.runOnce(count);
        graph.setEnabled(count, true);
        graph.execute().value();
        graph.execute().value();
        CHECK(runs == 4);

        // The newest finished copy, once
        ctx.waitIdleAll();
        auto finished = node.takeFinished<uint32_t>().value();
        REQUIRE(finished);
        CHECK(*finished == std::vector<uint32_t>{1, 1});
        CHECK_FALSE(node.takeFinished<uint32_t>().value());

        // Copies arrive as their frames finish, the newest superseding older ones
        value = 2;
        graph.execute().value();
        value = 3;
        graph.execute().value();
        graph.waitIdle().value();
        auto latest = node.takeFinished<uint32_t>().value();
        REQUIRE(latest);
        CHECK(*latest == std::vector<uint32_t>{3, 3});
        CHECK_FALSE(node.takeFinished<uint32_t>().value());
        graph.setEnabled(fill, false);
        ctx.waitIdleAll();
    }
}

TEST_CASE_FIXTURE(GpuTest, "Image readbacks give finished copies without waiting, as texels with their extent") {
    auto& ctx = context();
    RenderGraph graph(ctx);
    ImageId image = graph.createImage("picture", FrameMode::Shared, VK_FORMAT_R8G8B8A8_UNORM, FixedExtent{{2, 1}});
    float red = 1.0f;
    graph.addTransfer("clear", [image](NodeBuilder& builder) { builder.write(image, Access::TransferDst); },
                      [image, &red](RecordContext& record) {
                          const VkClearColorValue color{{red, 0.0f, 0.0f, 1.0f}};
                          VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                          vkCmdClearColorImage(record.commandBuffer, record.graph.image(image, record.frameIndex)->getImage(),
                                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
                      });
    NodeId readback = graph.add<ReadbackNode>("readback", image);
    auto& node = graph.nodeAs<ReadbackNode>(readback);
    CHECK_FALSE(node.takeFinished<uint32_t>().value());

    graph.execute().value();
    red = 0.0f;
    graph.execute().value();
    graph.waitIdle().value();
    // The newest, once: a texel holds a pixel's bytes (RGBA, so red is the low byte)
    auto latest = node.takeFinished<uint32_t>().value();
    REQUIRE(latest);
    CHECK(latest->texels == std::vector<uint32_t>{0xFF000000u, 0xFF000000u});
    CHECK(latest->extent.width == 2);
    CHECK(latest->extent.height == 1);
    CHECK_FALSE(node.takeFinished<uint32_t>().value());
    // A texel type of another size than the format's pixels is misuse
    CHECK_THROWS_AS(node.takeFinished<uint8_t>(), ContractViolation);
}

namespace {

// Runs frames as fast as the CPU can, after heavy GPU work so the GPU lags behind, polling a
// readback (take(): the frame number its newest finished copy holds, if any) before each frame
// numbered k from 0, as apps do. Each copy is checked to be newer than the last, and to be at most
// frames in flight + 1 frames old, i.e. no copy is dropped while waiting for its slot.
template<typename Take>
void checkFastLoopDelivery(RenderGraph& graph, uint32_t& frameValue, Take take) {
    // Work slower than recording a frame: filling a large buffer
    BufferId heavy = graph.createBuffer("heavy", FrameMode::Shared, 128u << 20);
    graph.addTransfer("heavy", [heavy](NodeBuilder& builder) { builder.write(heavy, Access::TransferDst); },
                      [heavy](RecordContext& record) {
                          vkCmdFillBuffer(record.commandBuffer, record.graph.buffer(heavy, record.frameIndex), 0, VK_WHOLE_SIZE, 1);
                      });
    const int frames = static_cast<int>(graph.context().framesInFlight());
    std::optional<int> newest;
    int delivered = 0;
    for (int k = 0; k < 60; ++k) {
        CAPTURE(k);
        if (const std::optional<int> copy = take()) {
            if (newest) {
                CHECK(*copy > *newest);
            }
            newest = copy;
            ++delivered;
        }
        // Frame k - 1 - frames finished when frame k - 1 waited for its slot
        if (k > frames) {
            REQUIRE(newest);
            CHECK(*newest >= k - 1 - frames);
        }
        frameValue = static_cast<uint32_t>(k);
        graph.execute().value();
    }
    MESSAGE("polls that delivered a copy: " << delivered << " of 60");
    graph.waitIdle().value();
}

} // namespace

TEST_CASE_FIXTURE(GpuTest, "Buffer readbacks deliver a copy whose frame slot is reused before it's polled") {
    RenderGraph graph(context());
    BufferId buffer = graph.createBuffer("values", FrameMode::Shared, 2 * sizeof(uint32_t));
    uint32_t value = 0;
    graph.addTransfer("fill", [buffer](NodeBuilder& builder) { builder.write(buffer, Access::TransferDst); },
                      [buffer, &value](RecordContext& record) {
                          vkCmdFillBuffer(record.commandBuffer, record.graph.buffer(buffer, record.frameIndex), 0, VK_WHOLE_SIZE, value);
                      });
    auto& node = graph.nodeAs<BufferReadbackNode>(graph.add<BufferReadbackNode>("readback", buffer));
    checkFastLoopDelivery(graph, value, [&node]() -> std::optional<int> {
        auto copy = node.takeFinished<uint32_t>().value();
        if (!copy) {
            return std::nullopt;
        }
        REQUIRE(copy->size() == 2);
        CHECK((*copy)[0] == (*copy)[1]);
        return static_cast<int>((*copy)[0]);
    });
}

TEST_CASE_FIXTURE(GpuTest, "Image readbacks deliver a copy whose frame slot is reused before it's polled") {
    RenderGraph graph(context());
    ImageId image = graph.createImage("picture", FrameMode::Shared, VK_FORMAT_R32_UINT, FixedExtent{{2, 1}});
    uint32_t value = 0;
    graph.addTransfer("clear", [image](NodeBuilder& builder) { builder.write(image, Access::TransferDst); },
                      [image, &value](RecordContext& record) {
                          VkClearColorValue color{};
                          color.uint32[0] = value;
                          VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                          vkCmdClearColorImage(record.commandBuffer, record.graph.image(image, record.frameIndex)->getImage(),
                                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
                      });
    auto& node = graph.nodeAs<ReadbackNode>(graph.add<ReadbackNode>("readback", image));
    checkFastLoopDelivery(graph, value, [&node]() -> std::optional<int> {
        auto copy = node.takeFinished<uint32_t>().value();
        if (!copy) {
            return std::nullopt;
        }
        REQUIRE(copy->texels.size() == 2);
        CHECK(copy->texels[0] == copy->texels[1]);
        return static_cast<int>(copy->texels[0]);
    });
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
        graph.execute().value();
        CAPTURE(frame);
        CHECK(cpuRuns == frame);
        CHECK(earlyUpdatesSeen == frame);
        CHECK(lateUpdatesSeen == frame - 1);
        CHECK(late->updates == frame);
    }

    // Disabled nodes' materials aren't updated
    graph.setEnabled(lateNode, false);
    graph.execute().value();
    CHECK(late->updates == 3);
    CHECK(early->updates == 4);
    ctx.waitIdleAll();
}

TEST_CASE_FIXTURE(GpuTest, "Headless graphs resize surface relative images to the surface extent they're given") {
    auto& ctx = context();
    RenderGraph graph(ctx);
    CHECK(graph.surfaceExtent().width == 0);
    graph.setSurfaceExtent({16, 8});
    ImageId image = graph.createImage("half", FrameMode::PerFrame, VK_FORMAT_R8G8B8A8_UNORM, SurfaceRelative{0.5f});

    // Covers the largest extent below; pattern.comp skips texels outside the image
    auto material = patternMaterial(ctx, graph.storageEntry(image));
    VkExtent2D seenExtent{};
    graph.addCpu("extent", nullptr, [&](FrameContext& frame) { seenExtent = frame.surfaceExtent; });
    graph.add<DispatchNode>("pattern", material);
    NodeId readback = graph.add<ReadbackNode>("readback", image);

    graph.execute().value();
    CHECK(seenExtent.width == 16);
    CHECK(seenExtent.height == 8);
    CHECK(hasPattern(graph.nodeAs<ReadbackNode>(readback).pixels().value(), 8, 4));

    // Every frame's image is recreated at the new size, and the material's entries follow
    graph.setSurfaceExtent({kWidth * 2, kHeight * 2});
    for (uint32_t frame = 0; frame < ctx.framesInFlight(); ++frame) {
        graph.execute().value();
    }
    for (uint32_t frame = 0; frame < ctx.framesInFlight(); ++frame) {
        CHECK(graph.imageExtent(image, frame).width == kWidth);
        CHECK(graph.imageExtent(image, frame).height == kHeight);
    }
    CHECK(seenExtent.width == kWidth * 2);
    CHECK(hasPattern(graph.nodeAs<ReadbackNode>(readback).pixels().value(), kWidth, kHeight));
    ctx.waitIdleAll();
}

namespace {

// A node whose per-frame preparation fails while `failing` is set
class FlakyNode : public TransferNode {
public:
    explicit FlakyNode(const bool& failing) : failing_(&failing) {}

    Result<void> prepareFrame(FrameContext&) override {
        if (*failing_) {
            return Error(ErrorCode::OutOfMemory, "flaky failure");
        }
        return {};
    }

private:
    const bool* failing_;
};

} // namespace

TEST_CASE_FIXTURE(GpuTest, "A node failing to prepare fails the frame before anything is submitted, and the frame can be retried") {
    auto& ctx = context();
    auto image = storageImage(ctx);
    RenderGraph graph(ctx);
    graph.importImage("output", image, ImageState::general());
    bool failing = true;
    graph.add<FlakyNode>("flaky", failing);
    graph.add<DispatchNode>("pattern", patternMaterial(ctx, ctx.descriptors().storageImage(image)));

    const uint64_t frameNumber = ctx.frameNumber();
    const auto submitted = ctx.submittedPoints();
    CHECK_ERROR(graph.execute(), ErrorCode::OutOfMemory, "preparing node 'flaky': flaky failure");
    CHECK(ctx.frameNumber() == frameNumber);
    CHECK(ctx.submittedPoints() == submitted);

    failing = false;
    CHECK_OK(graph.execute());
    CHECK(ctx.frameNumber() == frameNumber + 1);
    graph.waitIdle().value();
    CHECK(hasPattern(image->readback(VK_IMAGE_LAYOUT_GENERAL).value()));
}

TEST_CASE_FIXTURE(GpuTest, "Compiling builds the materials' pipelines") {
    auto& ctx = context();
    auto image = storageImage(ctx);
    auto material = patternMaterial(ctx, ctx.descriptors().storageImage(image));
    CHECK(material->getPipeline() == VK_NULL_HANDLE);
    {
        RenderGraph graph(ctx);
        graph.add<DispatchNode>("pattern", material);
        CHECK_OK(graph.compile());
    }
    CHECK(material->getPipeline() != VK_NULL_HANDLE);
}

TEST_CASE_FIXTURE(GpuTest, "RenderGraph measures its nodes' GPU time when asked") {
    auto& ctx = context();
    auto image = storageImage(ctx);
    RenderGraph graph(ctx);
    ImageId output = graph.importImage("output", image, ImageState::general());
    NodeId pattern = graph.add<DispatchNode>("pattern", patternMaterial(ctx, ctx.descriptors().storageImage(image)));
    NodeId readback = graph.add<ReadbackNode>("readback", output);
    graph.addCpu("cpu", [](NodeBuilder&) {}, [](FrameContext&) {});

    // Off by default
    CHECK_FALSE(graph.timingEnabled());
    graph.execute().value();
    CHECK(graph.timings().value().empty());

    graph.setTimingEnabled(true);
    for (int frame = 0; frame < 3; ++frame) {
        graph.execute().value();
    }
    // The GPU nodes of the last frame, in recording order; CPU nodes have no GPU time
    const auto timings = graph.timings().value();
    REQUIRE(timings.size() == 2);
    CHECK(timings[0].node.index == pattern.index);
    CHECK(timings[0].name == "pattern");
    CHECK(timings[1].node.index == readback.index);
    CHECK(timings[1].queue == uint32_t(graph.queueOf(readback).value()));
    for (const auto& timing : timings) {
        CHECK(timing.milliseconds >= 0.0);
        CHECK(timing.milliseconds < 1000.0);
    }
    CHECK(hasPattern(graph.nodeAs<ReadbackNode>(readback).pixels().value()));

    // Frames in flight can change while timing; turned off, frames aren't timed
    ctx.setFramesInFlight(3);
    graph.execute().value();
    CHECK(graph.timings().value().size() == 2);
    graph.setTimingEnabled(false);
    for (int frame = 0; frame < 3; ++frame) {
        graph.execute().value();
    }
    CHECK(graph.timings().value().empty());
    ctx.setFramesInFlight(2);
    graph.execute().value();
    graph.waitIdle().value();
}
