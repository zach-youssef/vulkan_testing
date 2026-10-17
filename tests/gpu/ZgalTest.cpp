// The C++ ZGAL generates (from zgal/interfaces.zgal), checked against what its shaders do on the
// device
#include "VkTestContext.h"
#include "graph/RenderGraph.h"
#include "graph/nodes/DispatchNode.h"
#include "graph/nodes/ReadbackNode.h"
#include "graph/nodes/RenderablesNode.h"
#include "materials/Renderable.h"

#include "interfaces.zgal.h"

#include <cstring>
#include <mutex>
#include <numeric>

namespace {

class Probe : public LayoutProbe {
public:
    Probe(VulkanContext& context, const Shaders& shaders, Bindings bindings, Push push)
    : LayoutProbe(context, shaders, std::move(bindings)), push_(push) {}

    void update(uint32_t, VkExtent2D) override {}

    glm::vec3 getDispatchDimensions() override {
        return workgroupsFor(1);
    }

protected:
    void writePush(uint32_t, Push& push) override {
        push = push_;
    }

private:
    Push push_;
};

class TintedMaterial : public Tinted {
public:
    TintedMaterial(VulkanContext& context, const Shaders& shaders, AttachmentFormats formats, glm::vec4 tint)
    : Tinted(context, shaders, std::move(formats), GraphicsPipelineOptions{.cullMode = VK_CULL_MODE_NONE}), tint_(tint) {}

protected:
    void writePush(uint32_t, Push& push) override {
        push.tint = tint_;
    }

private:
    glm::vec4 tint_;
};

AttachmentFormats rgba8() {
    return AttachmentFormats{{VK_FORMAT_R8G8B8A8_UNORM}};
}

} // namespace

TEST_CASE_FIXTURE(GpuTest, "ZGAL's structs and push constants have the shaders' std430 layout") {
    auto& ctx = context();
    auto& heap = ctx.descriptors();

    std::vector<float> values(36);
    std::iota(values.begin(), values.end(), 1.0f);
    auto inputs = Buffer<float>::createAndInitialize(ctx, values, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT).value();
    auto output = Buffer<Tricky>::create(ctx, 1, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT).value();
    {
        RenderGraph graph(ctx);
        graph.importBuffer<float>("inputs", inputs);
        graph.importBuffer<Tricky>("output", output);
        auto shaders = LayoutProbe::loadShaders(ctx).value();
        LayoutProbe::Push push{.seed = 0.5f, .shift = {2.0f, 3.0f, 4.0f}, .tail = 40};
        graph.add<DispatchNode>("probe", ctx.resources().create<Probe>(
            ctx, shaders, LayoutProbe::Bindings{.inputs = heap.storageBuffer(inputs), .result = heap.storageBuffer(output)},
            push));
        graph.execute().value();
        ctx.waitIdleAll();
    }

    Tricky t{};
    output->mapAndExecute(0, sizeof(Tricky), [&t](void* data) { memcpy(&t, data, sizeof(Tricky)); }).value();
    CHECK(t.s == 0.5f);
    CHECK(t.v3 == glm::vec3(2.0f, 3.0f, 4.0f));
    CHECK(t.f == 1.0f);
    CHECK(t.v2 == glm::vec2(2.0f, 3.0f));
    CHECK(t.i == -4);
    CHECK(t.u == glm::uvec3(40, 41, 42));
    CHECK(t.m2 == glm::mat2(5.0f, 6.0f, 7.0f, 8.0f));
    CHECK(t.inner.a == glm::vec2(9.0f, 10.0f));
    CHECK(t.inner.b == 11.0f);
    CHECK(t.scalars[0] == 12.0f);
    CHECK(t.scalars[2] == 14.0f);
    CHECK(t.inners[0].a == glm::vec2(15.0f, 16.0f));
    CHECK(t.inners[0].b == 17.0f);
    CHECK(t.inners[1].a == glm::vec2(18.0f, 19.0f));
    CHECK(t.inners[1].b == 20.0f);
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            CHECK(t.m4[column][row] == values[20 + 4 * column + row]);
        }
    }
    CHECK(t.last == 80);
}

TEST_CASE_FIXTURE(GpuTest, "ZGAL graphics materials read their vertex format and pass varyings") {
    auto& ctx = context();
    // Two triangles covering the target, coloured, with the id the fragment shader looks for
    const glm::vec3 color{0.5f, 0.25f, 1.0f};
    const std::vector<ColoredVertex> vertices{
        {{-1.0f, -1.0f}, color, 7}, {{1.0f, -1.0f}, color, 7}, {{1.0f, 1.0f}, color, 7}, {{-1.0f, 1.0f}, color, 7}};
    const std::vector<uint16_t> indices{0, 1, 2, 2, 3, 0};
    auto mesh = Mesh::create(ctx, vertices, indices).value();

    RenderGraph graph(ctx);
    ImageId image = graph.createImage("color", FrameMode::Shared, VK_FORMAT_R8G8B8A8_UNORM, FixedExtent{{4, 4}});
    RenderTarget target;
    target.color.push_back({image, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {0.0f, 0.0f, 0.0f, 1.0f}});
    auto shaders = Tinted::loadShaders(ctx).value();
    auto material = ctx.resources().create<TintedMaterial>(ctx, shaders, graph.formats(target),
                                                           glm::vec4(1.0f, 0.5f, 0.5f, 1.0f));
    graph.add<RenderablesNode>("draw", std::vector<Handle<Renderable>>{ctx.resources().create<MeshRenderable>(mesh, material)},
                               target);
    NodeId readback = graph.add<ReadbackNode>("readback", image);
    graph.execute().value();
    auto pixels = graph.nodeAs<ReadbackNode>(readback).pixels().value();
    // (0.5, 0.25, 1) * (1, 0.5, 0.5, 1)
    CHECK(std::vector<uint8_t>(pixels.begin(), pixels.begin() + 4) == std::vector<uint8_t>{128, 32, 128, 255});
}

TEST_CASE_FIXTURE(GpuTest, "ZGAL materials check their bindings, formats and options") {
    auto& ctx = context();
    auto& heap = ctx.descriptors();
    auto image = Image::createEmptyRGBA(ctx, 4, 4).value();
    auto sampler = VulkanSampler::create(ctx, SamplerDesc{}).value();
    auto shaders = Textured::loadShaders(ctx).value();
    const Textured::Bindings bindings{.albedo = heap.sampledImage(image), .albedoSampler = heap.sampler(sampler)};

    // Every binding needs an entry
    CHECK_THROWS_WITH_AS(ctx.resources().create<Textured>(ctx, shaders, Textured::Bindings{.albedo = heap.sampledImage(image)},
                                                          rgba8(), GraphicsPipelineOptions{}),
                         doctest::Contains("Textured's binding 'albedoSampler' has no descriptor entry"), ContractViolation);
    // As many colour attachments as the fragment shader has outputs
    CHECK_THROWS_WITH_AS(ctx.resources().create<Textured>(ctx, shaders, bindings, AttachmentFormats{},
                                                          GraphicsPipelineOptions{}),
                         doctest::Contains("writes 1 colour attachments, but its target has 0"), ContractViolation);
    // The vertex input is the .zgal file's to declare
    CHECK_THROWS_WITH_AS(ctx.resources().create<Textured>(ctx, shaders, bindings, rgba8(),
                                                          GraphicsPipelineOptions{.vertexLayout = VertexLayout{}}),
                         doctest::Contains("leave GraphicsPipelineOptions::vertexLayout empty"), ContractViolation);

    auto material = ctx.resources().create<Textured>(ctx, shaders, bindings, rgba8(), GraphicsPipelineOptions{});
    CHECK(material->bindings()[0].access == Access::SampledRead);
    CHECK(material->bindings()[0].stages == VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    CHECK(material->pushConstants(0).size() == 2 * sizeof(uint32_t));
    CHECK(material->getPipeline() == VK_NULL_HANDLE);
    CHECK_OK(material->ensurePipeline());

    // An entry of the wrong kind would index the wrong descriptor array: binding the material is a
    // contract violation
    auto swapped = ctx.resources().create<Textured>(
        ctx, shaders, Textured::Bindings{.albedo = heap.sampler(sampler), .albedoSampler = heap.sampler(sampler)}, rgba8(),
        GraphicsPipelineOptions{});
    CHECK_THROWS_WITH_AS(swapped->pushConstants(0),
                         doctest::Contains("Textured's binding 'albedo' needs a sampled image entry, but was given a sampler entry"),
                         ContractViolation);
    ctx.waitIdleAll();
}

TEST_CASE_FIXTURE(GpuTest, "ZGAL graphs write uniforms once per frame and compute push constants each frame") {
    auto& ctx = context();
    RenderGraph graph(ctx);
    int tintCalls = 0;
    float brightness = 1.0f;
    const ProbeGraph probe = ProbeGraph::build(ctx, graph, ProbeGraph::Inputs{
        .scale = 0.5f,
        .withReadback = true,
        .tint = [&tintCalls](const zgal::FrameInfo&) {
            ++tintCalls;
            return TintData{.color = {1.0f, 0.5f, 0.25f, 1.0f}};
        },
        .brightness = [&brightness](const zgal::FrameInfo&) { return brightness; },
    }).value();
    REQUIRE(probe.nodes.readback);
    auto firstPixel = [&] {
        auto pixels = graph.nodeAs<ReadbackNode>(*probe.nodes.readback).pixels().value();
        return std::vector<uint8_t>(pixels.begin(), pixels.begin() + 4);
    };

    // (1, 0.5, 0.25) * 0.5 * brightness
    graph.execute().value();
    CHECK(firstPixel() == std::vector<uint8_t>{128, 64, 32, 255});
    brightness = 2.0f;
    graph.execute().value();
    CHECK(firstPixel() == std::vector<uint8_t>{255, 128, 64, 255});
    // Both materials use the uniform, but it's computed once a frame
    CHECK(tintCalls == 2);
    ctx.waitIdleAll();
}

TEST_CASE_FIXTURE(GpuTest, "ZGAL graphs leave out what their conditions exclude, and need every input") {
    auto& ctx = context();
    {
        RenderGraph graph(ctx);
        const ProbeGraph probe = ProbeGraph::build(ctx, graph, ProbeGraph::Inputs{
            .scale = 1.0f,
            .withReadback = false,
            .tint = [](const zgal::FrameInfo&) { return TintData{}; },
            .brightness = [](const zgal::FrameInfo&) { return 1.0f; },
        }).value();
        CHECK_FALSE(probe.nodes.readback);
        graph.execute().value();
        ctx.waitIdleAll();
    }
    {
        RenderGraph graph(ctx);
        CHECK_THROWS_WITH_AS((void)ProbeGraph::build(ctx, graph, ProbeGraph::Inputs{
                                 .tint = [](const zgal::FrameInfo&) { return TintData{}; },
                             }),
                             doctest::Contains("ProbeGraph's input brightness isn't given"), ContractViolation);
    }
}

TEST_CASE_FIXTURE(GpuTest, "ZGAL graphs compute dispatch sizes each frame") {
    auto& ctx = context();
    RenderGraph graph(ctx);
    uint32_t count = 3;
    const CountGraph counting = CountGraph::build(ctx, graph, CountGraph::Inputs{
        .count = [&count](const zgal::FrameInfo&) { return count; },
    }).value();
    // The red channel of each of the row's 8 pixels
    auto marked = [&] {
        auto pixels = graph.nodeAs<ReadbackNode>(counting.nodes.readback).pixels().value();
        std::vector<uint8_t> red;
        for (size_t i = 0; i < 8; ++i) {
            red.push_back(pixels[4 * i]);
        }
        return red;
    };

    graph.execute().value();
    CHECK(marked() == std::vector<uint8_t>{255, 255, 255, 0, 0, 0, 0, 0});
    count = 6;
    graph.execute().value();
    CHECK(marked() == std::vector<uint8_t>{255, 255, 255, 255, 255, 255, 0, 0});
    ctx.waitIdleAll();
}

TEST_CASE_FIXTURE(GpuTest, "ZGAL uniforms are computed once a frame and only written when they change") {
    auto& ctx = context();
    float value = 1.0f;
    int calls = 0;
    auto uniform = zgal::Uniform<float>::create(ctx, [&](const zgal::FrameInfo&) {
        ++calls;
        return value;
    }).value();
    const uint32_t frames = ctx.framesInFlight();
    uint64_t frameNumber = 0;
    auto runFrames = [&](uint32_t n) {
        for (uint32_t i = 0; i < n; ++i, ++frameNumber) {
            const zgal::FrameInfo frame{static_cast<uint32_t>(frameNumber % frames), frameNumber, {0, 0}};
            uniform->update(frame);
            // Again in the same frame, as a second material using it would
            uniform->update(frame);
        }
    };

    // Each frame's buffer is written once, then holds the value
    runFrames(2 * frames);
    CHECK(calls == static_cast<int>(2 * frames));
    CHECK(uniform->writes() == frames);
    // A new value is written to each buffer once more
    value = 2.0f;
    runFrames(2 * frames);
    CHECK(uniform->writes() == 2 * frames);
}

namespace {

class Evaluator : public Evaluate {
public:
    Evaluator(VulkanContext& context, const Shaders& shaders, Bindings bindings, uint32_t count)
    : Evaluate(context, shaders, std::move(bindings)), count_(count) {}

    void update(uint32_t, VkExtent2D) override {}

    glm::vec3 getDispatchDimensions() override {
        return workgroupsFor(count_);
    }

protected:
    void writePush(uint32_t, Push& push) override {
        push.count = count_;
    }

private:
    uint32_t count_;
};

} // namespace

TEST_CASE_FIXTURE(GpuTest, "ZGAL functions compute the same on the CPU as on the GPU") {
    auto& ctx = context();
    constexpr uint32_t kCount = 200;
    auto output = Buffer<glm::vec4>::create(ctx, kCount, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT).value();
    {
        RenderGraph graph(ctx);
        graph.importBuffer<glm::vec4>("output", output);
        auto shaders = Evaluate::loadShaders(ctx).value();
        // The binding's access (write) and stage were inferred from the body
        auto material = ctx.resources().create<Evaluator>(
            ctx, shaders, Evaluate::Bindings{.evaluated = ctx.descriptors().storageBuffer(output)}, kCount);
        CHECK(material->bindings()[0].access == Access::StorageBufferWrite);
        graph.add<DispatchNode>("evaluate", material);
        graph.execute().value();
        ctx.waitIdleAll();
    }
    output->mapAndExecute(0, kCount * sizeof(glm::vec4), [](void* data) {
        const auto* gpu = static_cast<const glm::vec4*>(data);
        for (uint32_t i = 0; i < kCount; ++i) {
            CAPTURE(i);
            // evaluate, compiled to C++ from the same definition
            const glm::vec4 cpu = evaluate(i);
            CHECK(gpu[i].w == cpu.w);   // Integer arithmetic: exact
            for (int c = 0; c < 3; ++c) {
                CHECK(gpu[i][c] == doctest::Approx(cpu[c]).epsilon(1e-4));
            }
        }
    }).value();
}

namespace {

// A context with the device features ZGAL materials ask for, and validation, recording errors
struct FeatureContext {
    explicit FeatureContext(const DeviceFeatures& features) {
        VulkanContextConfig config;
        config.applicationName = "ZGAL feature tests";
        config.shaderSearchPaths = {TEST_SHADER_DIR};
        config.enableValidation = true;
        config.enableSynchronizationValidation = true;
        config.features = features;
        config.onValidationMessage = [this](VkDebugUtilsMessageSeverityFlagBitsEXT severity, const char* message) {
            if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
                std::lock_guard<std::mutex> lock(mutex);
                errors.emplace_back(message);
            }
        };
        context = VulkanContext::create(std::move(config)).value();
    }

    std::mutex mutex;
    std::vector<std::string> errors;
    std::unique_ptr<VulkanContext> context;
};

// A mapped storage buffer
template<typename T>
Result<Handle<Buffer<T>>> hostBuffer(VulkanContext& ctx, size_t count) {
    return Buffer<T>::create(ctx, count, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

template<typename T>
std::vector<T> contents(const Handle<Buffer<T>>& buffer, size_t count) {
    std::vector<T> values(count);
    buffer->mapAndExecute(0, count * sizeof(T), [&values](void* data) { memcpy(values.data(), data, values.size() * sizeof(T)); })
        .value();
    return values;
}

class Reducer : public Reduce {
public:
    Reducer(VulkanContext& context, const Shaders& shaders, Bindings bindings, uint32_t count)
    : Reduce(context, shaders, std::move(bindings)), count_(count) {}

    void update(uint32_t, VkExtent2D) override {}

    glm::vec3 getDispatchDimensions() override {
        return workgroupsFor(count_);
    }

private:
    uint32_t count_;
};

} // namespace

TEST_CASE("ZGAL's 8- and 16-bit integers and shared memory") {
    const DeviceFeatures features = Reduce::requiredFeatures();
    CHECK(features.vulkan12.shaderInt8);
    CHECK(features.vulkan12.storageBuffer8BitAccess);
    CHECK(features.core.features.shaderInt16);
    CHECK(features.vulkan11.storageBuffer16BitAccess);
    FeatureContext feature(features);
    auto& ctx = *feature.context;

    constexpr uint32_t kCount = 256;
    constexpr uint32_t kGroups = kCount / 64;
    std::vector<Bytes> elements(kCount);
    for (uint32_t i = 0; i < kCount; ++i) {
        elements[i] = Bytes{.small = uint8_t(250 + i % 7),   // Up to 256: the repacked value wraps
                            .delta = int16_t(int(i % 200) - 100),
                            .many = {uint8_t(i), uint8_t(255 - i % 256), 7},
                            .tiny = int8_t(int(i % 100) - 50),
                            .wide = 100000 + i,
                            .level = uint16_t(65000 + i)};   // Wraps when repacked, from 65535
    }
    auto packed = hostBuffer<Bytes>(ctx, kCount).value();
    auto sums = hostBuffer<uint32_t>(ctx, kGroups).value();
    packed->mapAndExecute(0, kCount * sizeof(Bytes), [&](void* data) { memcpy(data, elements.data(), kCount * sizeof(Bytes)); })
        .value();
    {
        RenderGraph graph(ctx);
        graph.importBuffer<Bytes>("packed", packed);
        graph.importBuffer<uint32_t>("sums", sums);
        auto shaders = Reduce::loadShaders(ctx).value();
        auto& heap = ctx.descriptors();
        graph.add<DispatchNode>("reduce", ctx.resources().create<Reducer>(
            ctx, shaders, Reduce::Bindings{.packed = heap.storageBuffer(packed), .sums = heap.storageBuffer(sums)}, kCount));
        graph.execute().value();
        ctx.waitIdleAll();
    }

    const auto totals = contents(sums, kGroups);
    for (uint32_t g = 0; g < kGroups; ++g) {
        uint32_t expected = 0;
        for (uint32_t i = g * 64; i < (g + 1) * 64; ++i) {
            const Bytes& b = elements[i];
            expected += b.small + b.many[0] + b.many[1] + b.many[2] + b.wide + b.level + uint32_t(b.delta + b.tiny + 1000);
        }
        CAPTURE(g);
        CHECK(totals[g] == expected);
    }
    const auto repacked = contents(packed, kCount);
    for (uint32_t i = 0; i < kCount; ++i) {
        CAPTURE(i);
        CHECK(repacked[i].small == uint8_t(elements[i].small + 1));
        CHECK(repacked[i].delta == -elements[i].delta);
        CHECK(repacked[i].tiny == -elements[i].tiny);
        CHECK(repacked[i].level == uint16_t(elements[i].level + 1));
        CHECK(repacked[i].wide == elements[i].wide);
        CHECK(std::memcmp(repacked[i].many, elements[i].many, 3) == 0);
    }

    packed.reset();
    sums.reset();
    std::lock_guard<std::mutex> lock(feature.mutex);
    CHECK(feature.errors.empty());
    for (const auto& error : feature.errors) {
        MESSAGE(error);
    }
}

TEST_CASE_FIXTURE(GpuTest, "A ZGAL material needing device features the context lacks is a contract violation") {
    // The shared test context doesn't enable 8-bit integers
    auto& ctx = context();
    REQUIRE_FALSE(ctx.enabledFeatures().missing(Reduce::requiredFeatures()).empty());
    CHECK_THROWS_WITH_AS((void)Reduce::loadShaders(ctx), doctest::Contains("Reduce needs device features the context doesn't enable"),
                         ContractViolation);
}
