// The C++ ZGAL generates (from zgal/interfaces.zgal), checked against what its shaders do on the
// device
#include "VkTestContext.h"
#include "graph/RenderGraph.h"
#include "graph/nodes/DispatchNode.h"
#include "graph/nodes/ReadbackNode.h"
#include "graph/nodes/RenderablesNode.h"
#include "materials/Renderable.h"

#include "interfaces.zgal.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <numeric>

// The generated code is in the namespace of its file, interfaces.zgal
using namespace interfaces;

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

TEST_CASE_FIXTURE(GpuTest, "ZGAL vertex attributes can be stored in fewer bits than shaders read") {
    auto& ctx = context();
    // Normalized, half and small integer attributes, converted as the generated struct's comments say
    static_assert(sizeof(PackedVertex) % 4 == 0, "packed attributes start at 4-byte boundaries");
    const auto vertex = [](glm::vec2 position) {
        return PackedVertex{.position = glm::packSnorm<int16_t>(position),
                            .color = glm::packUnorm<uint8_t>(glm::vec4(1.0f, 0.5f, 0.25f, 1.0f)),
                            .id = 7,
                            .shade = glm::packHalf1x16(0.5f)};
    };
    const std::vector<PackedVertex> vertices{vertex({-1.0f, -1.0f}), vertex({1.0f, -1.0f}), vertex({1.0f, 1.0f}),
                                             vertex({-1.0f, 1.0f})};
    const std::vector<uint16_t> indices{0, 1, 2, 2, 3, 0};
    auto mesh = Mesh::create(ctx, vertices, indices).value();

    RenderGraph graph(ctx);
    ImageId image = graph.createImage("color", FrameMode::Shared, VK_FORMAT_R8G8B8A8_UNORM, FixedExtent{{4, 4}});
    RenderTarget target;
    target.color.push_back({image, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {0.0f, 0.0f, 0.0f, 1.0f}});
    auto material = ctx.resources().create<Packed>(ctx, Packed::loadShaders(ctx).value(), graph.formats(target),
                                                   GraphicsPipelineOptions{.cullMode = VK_CULL_MODE_NONE});
    graph.add<RenderablesNode>("draw", std::vector<Handle<Renderable>>{ctx.resources().create<MeshRenderable>(mesh, material)},
                               target);
    NodeId readback = graph.add<ReadbackNode>("readback", image);
    graph.execute().value();
    auto pixels = graph.nodeAs<ReadbackNode>(readback).pixels().value();
    // (1, 0.5, 0.25, 1) * 0.5, every pixel covered
    for (size_t i = 0; i < pixels.size(); i += 4) {
        CHECK(std::vector<uint8_t>(pixels.begin() + i, pixels.begin() + i + 4) == std::vector<uint8_t>{128, 64, 32, 128});
    }
}

TEST_CASE_FIXTURE(GpuTest, "ZGAL image files load from the asset search paths in their colour space") {
    auto& ctx = context();
    RenderGraph graph(ctx);
    auto ids = ImageFiles::build(ctx, graph, ImageFiles::Inputs{.chosen = "quadrants.png"}).value();
    graph.execute().value();
    const auto srgb = ids.srgbReadContents(graph).value();
    const auto linear = ids.linearReadContents(graph).value();
    REQUIRE(srgb.size() == 4);
    REQUIRE(linear.size() == 4);
    CHECK(srgb[0] == glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
    CHECK(linear[2] == glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
    // Grey 128: decoded from sRGB, or as stored
    CHECK(srgb[3].r == doctest::Approx(0.2158f).epsilon(0.01));
    CHECK(linear[3].r == doctest::Approx(128.0f / 255.0f));

    RenderGraph missing(ctx);
    CHECK_ERROR(ImageFiles::build(ctx, missing, ImageFiles::Inputs{.chosen = "missing.png"}), ErrorCode::NotFound,
                "Asset 'missing.png' not found");
}

TEST_CASE_FIXTURE(GpuTest, "ZGAL graphs draw models with their materials' base colours") {
    auto& ctx = context();
    RenderGraph graph(ctx);
    auto ids = Models::build(ctx, graph, Models::Inputs{.file = "halves.gltf"}).value();
    // Two materials and the default one
    CHECK(ids.images.halfColors.size() == 3);
    graph.execute().value();
    const auto pixels = ids.canvasReadPixels(graph).value();
    const auto pixel = [&](size_t x, size_t y) {
        return std::vector<uint8_t>(pixels.begin() + (y * 4 + x) * 4, pixels.begin() + (y * 4 + x) * 4 + 4);
    };
    // The left half samples quadrants.png (sRGB, decoded), the right half is the green factor
    CHECK(pixel(0, 0) == std::vector<uint8_t>{255, 0, 0, 255});
    CHECK(pixel(1, 1) == std::vector<uint8_t>{0, 255, 0, 255});
    CHECK(pixel(0, 3) == std::vector<uint8_t>{0, 0, 255, 255});
    CHECK(pixel(1, 2) == std::vector<uint8_t>{55, 55, 55, 255});
    for (size_t y = 0; y < 4; ++y) {
        CHECK(pixel(2, y) == std::vector<uint8_t>{0, 128, 0, 255});
        CHECK(pixel(3, y) == std::vector<uint8_t>{0, 128, 0, 255});
    }

    // OBJ files too: the quad's material, then the default one for the triangle without any
    RenderGraph obj(ctx);
    auto objIds = Models::build(ctx, obj, Models::Inputs{.file = "quad.obj"}).value();
    CHECK(objIds.images.halfColors.size() == 2);
    obj.execute().value();

    // Attributes without defaults must be in the file; ones with defaults take them
    ModelPrimitive bare;
    bare.positions = {glm::vec3(0.0f)};
    bare.uvs = {glm::vec2(0.5f)};
    CHECK_ERROR(ModelVertex::fromModel(bare), ErrorCode::InvalidData,
                "the model has no normals, which ModelVertex's normal reads (or give it a #:default)");
    bare.normals = {glm::vec3(0.0f, 0.0f, 1.0f)};
    const auto vertices = ModelVertex::fromModel(bare).value();
    CHECK(vertices[0].color == glm::u8vec4(255));
    CHECK(vertices[0].uv == glm::packHalf(glm::vec2(0.5f)));

    // A model that can't be found
    RenderGraph missing(ctx);
    CHECK_ERROR(Models::build(ctx, missing, Models::Inputs{.file = "missing.gltf"}), ErrorCode::NotFound,
                "Asset 'missing.gltf' not found");
}

TEST_CASE_FIXTURE(GpuTest, "ZGAL graphs draw the meshes of models a shader chose") {
    auto& ctx = context();
    REQUIRE(ctx.enabledFeatures().missing(CulledModels::requiredFeatures()).empty());
    const std::vector<uint8_t> black{0, 0, 0, 255};
    for (const float side : {1.0f, -1.0f}) {
        CAPTURE(side);
        RenderGraph graph(ctx);
        auto ids = CulledModels::build(ctx, graph, CulledModels::Inputs{.file = "halves.gltf", .side = side}).value();
        for (int frame = 0; frame < 3; ++frame) {
            graph.execute().value();
        }
        const auto pixels = ids.canvasReadPixels(graph).value();
        const auto pixel = [&](size_t x, size_t y) {
            return std::vector<uint8_t>(pixels.begin() + (y * 4 + x) * 4, pixels.begin() + (y * 4 + x) * 4 + 4);
        };
        // Only the half on the chosen side: the right half's green factor, or the left's texture
        CHECK(pixel(3, 1) == (side > 0 ? std::vector<uint8_t>{0, 128, 0, 255} : black));
        CHECK(pixel(0, 0) == (side > 0 ? black : std::vector<uint8_t>{255, 0, 0, 255}));
        ctx.waitIdleAll();
    }
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

// A mapped buffer that copies can read and write
template<typename T>
Result<Handle<Buffer<T>>> hostBuffer(VulkanContext& ctx, size_t count) {
    return Buffer<T>::create(ctx, count, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

template<typename T>
std::vector<T> contents(const Handle<Buffer<T>>& buffer, size_t count) {
    std::vector<T> values(count);
    buffer->mapAndExecute(0, count * sizeof(T), [&values](void* data) { memcpy(values.data(), data, values.size() * sizeof(T)); })
        .value();
    return values;
}

} // namespace

TEST_CASE("ZGAL's 8- and 16-bit integers, shared memory, imported buffers, copies, cpu and extern nodes") {
    // The materials' features, which the graph collects
    const DeviceFeatures features = ReduceGraph::requiredFeatures();
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
    // One per frame, so the CPU never writes one a frame in flight is still copying from
    auto staging = PerFrameHandles<Buffer<Bytes>>::createManaged(ctx, [&ctx](uint32_t) { return hostBuffer<Bytes>(ctx, kCount); })
                       .value();
    auto results = hostBuffer<uint32_t>(ctx, kGroups + 1).value();
    auto repacked = hostBuffer<Bytes>(ctx, kCount).value();
    // A placeholder before the copied sums (copied with a destination offset)
    results->mapAndExecute(0, sizeof(uint32_t), [](void* data) { *static_cast<uint32_t*>(data) = 0xFEEDu; }).value();

    int filled = 0;
    {
        RenderGraph graph(ctx);
        const ReduceGraph reduce = ReduceGraph::build(ctx, graph, ReduceGraph::Inputs{
            .count = kCount,
            // The cpu node writes the staging buffer, before the copy reads it
            .upload = [&](FrameContext& frame) {
                ++filled;
                (*staging)[frame.frameIndex]->mapAndExecute(0, kCount * sizeof(Bytes), [&](void* data) {
                    memcpy(data, elements.data(), kCount * sizeof(Bytes));
                }).value();
            },
            // C++'s own node, using the graph's buffers by their ids
            .copyBack = [](RenderGraph& g, const ReduceGraph& ids) {
                const BufferId from = ids.buffers.packedBuffer;
                const BufferId to = ids.buffers.repacked;
                return std::vector<NodeId>{g.addTransfer(
                    "copy back", [from, to](NodeBuilder& builder) { builder.read(from, Access::TransferSrc).write(to, Access::TransferDst); },
                    [from, to](RecordContext& record) {
                        const VkBufferCopy region{0, 0, kCount * sizeof(Bytes)};
                        vkCmdCopyBuffer(record.commandBuffer, record.graph.buffer(from, record.frameIndex),
                                        record.graph.buffer(to, record.frameIndex), 1, &region);
                    })};
            },
            .staging = staging,
            .results = results,
            .repacked = repacked,
        }).value();
        CHECK(reduce.nodes.repack.size() == 1);
        // Frames in flight overlap, each with its own staging buffer
        for (int frame = 0; frame < 3; ++frame) {
            graph.execute().value();
        }
        ctx.waitIdleAll();
        // Copies check their ranges against the buffers' sizes, imported or not
        CHECK(graph.bufferSize(reduce.buffers.results, 0) == (kGroups + 1) * sizeof(uint32_t));
        CHECK(graph.bufferSize(reduce.buffers.packedBuffer, 0) == kCount * sizeof(Bytes));
    }
    CHECK(filled == 3);

    const auto sums = contents(results, kGroups + 1);
    CHECK(sums[0] == 0xFEEDu);
    for (uint32_t g = 0; g < kGroups; ++g) {
        uint32_t expected = 0;
        for (uint32_t i = g * 64; i < (g + 1) * 64; ++i) {
            const Bytes& b = elements[i];
            expected += b.small + b.many[0] + b.many[1] + b.many[2] + b.wide + b.level + uint32_t(b.delta + b.tiny + 1000);
        }
        CAPTURE(g);
        CHECK(sums[g + 1] == expected);
    }
    const auto packed = contents(repacked, kCount);
    for (uint32_t i = 0; i < kCount; ++i) {
        CAPTURE(i);
        CHECK(packed[i].small == uint8_t(elements[i].small + 1));
        CHECK(packed[i].delta == -elements[i].delta);
        CHECK(packed[i].tiny == -elements[i].tiny);
        CHECK(packed[i].level == uint16_t(elements[i].level + 1));
        CHECK(packed[i].wide == elements[i].wide);
        CHECK(std::memcmp(packed[i].many, elements[i].many, 3) == 0);
    }

    staging.reset();
    results.reset();
    repacked.reset();
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

TEST_CASE_FIXTURE(GpuTest, "ZGAL apps compute their state from events and frames, and their graph's inputs from it") {
    auto& ctx = context();
    RenderGraph graph(ctx);
    auto app = Counting::create(ctx, graph, Counting::Inputs{.start = 2, .timestep = zgal::Timestep::fixed(0.25f)}).value();
    CHECK(app->state().count == 2);
    auto marked = [&] {
        app->beginFrame().value();
        graph.execute().value();
        auto pixels = graph.nodeAs<ReadbackNode>(app->ids().nodes.readback).pixels().value();
        int count = 0;
        for (size_t i = 0; i < 8; ++i) {
            count += pixels[4 * i] == 255 ? 1 : 0;
        }
        return count;
    };

    CHECK(marked() == 2);
    app->handle(zgal::Event{zgal::KeyPressed{zgal::Key::Up}});
    app->handle(zgal::Event{zgal::KeyPressed{zgal::Key::Up}});
    CHECK(marked() == 4);
    // Events without a handler change nothing
    app->handle(zgal::Event{zgal::KeyReleased{zgal::Key::Up}});
    app->handle(zgal::Event{zgal::Scrolled{{0.0f, 1.0f}}});
    CHECK(marked() == 4);
    app->handle(zgal::Event{zgal::MousePressed{zgal::MouseButton::Left, {0.5f, 0.0f}}});
    CHECK(app->state().count == 6);
    CHECK(marked() == 6);
    // The marking pass is enabled from the state each frame
    app->handle(zgal::Event{zgal::KeyPressed{zgal::Key::M}});
    CHECK(marked() == 0);
    CHECK_FALSE(graph.isEnabled(app->ids().nodes.markRow));
    app->handle(zgal::Event{zgal::KeyPressed{zgal::Key::M}});
    CHECK(marked() == 6);

    // The frame handler ran each frame, on the fixed clock
    CHECK(app->state().frames == 6);
    CHECK(app->state().elapsed == doctest::Approx(6 * 0.25f));
    // C++ can set the state, e.g. to drive a test
    Counting::State state = app->state();
    state.count = 1;
    app->setState(state);
    CHECK(marked() == 1);
    CHECK_FALSE(app->quitRequested());
    ctx.waitIdleAll();

    // The clock is a required choice
    RenderGraph other(ctx);
    CHECK_THROWS_WITH_AS((void)Counting::create(ctx, other, Counting::Inputs{.start = 1}), doctest::Contains("timestep isn't given"),
                         ContractViolation);
}

TEST_CASE_FIXTURE(GpuTest, "ZGAL graphs upload data, clear buffers, copy buffers into images and read buffers back") {
    auto& ctx = context();
    const std::vector<uint32_t> values{1, 2, 3, 100, 12345};
    const std::vector<uint32_t> texels{0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0x80402010u};
    {
        RenderGraph graph(ctx);
        const DataGraph data = DataGraph::build(ctx, graph, DataGraph::Inputs{.values = values, .texels = texels}).value();
        graph.execute().value();

        std::vector<uint32_t> doubled;
        for (uint32_t v : values) {
            doubled.push_back(2 * v);
        }
        CHECK(data.readDoubledContents(graph).value() == doubled);
        CHECK(data.readFilledContents(graph).value() == std::vector<uint32_t>(4, 7u));
        const auto pixels = data.readPicturePixels(graph).value();
        REQUIRE(pixels.size() == texels.size() * 4);
        // The whole image, then texel (1, 1) patched with the first texel
        const std::vector<uint32_t> patched{texels[0], texels[1], texels[2], texels[0]};
        CHECK(std::memcmp(pixels.data(), patched.data(), pixels.size()) == 0);
        ctx.waitIdleAll();
    }
    {
        // An upload needs data
        RenderGraph graph(ctx);
        CHECK_THROWS_WITH_AS((void)DataGraph::build(ctx, graph, DataGraph::Inputs{.values = values}),
                             doctest::Contains("DataGraph's upload texelBuffer is given no data"), ContractViolation);
    }
}

TEST_CASE_FIXTURE(GpuTest, "ZGAL apps call the extern functions C++ gives") {
    auto& ctx = context();
    RenderGraph graph(ctx);
    std::vector<float> recorded;
    auto echo = Echo::create(ctx, graph, Echo::Inputs{
        .scale = [](float x) { return x * 2.0f; },
        .record = [&recorded](float x) { recorded.push_back(x); },
    }).value();
    echo->handle(zgal::Event{zgal::KeyPressed{zgal::Key::Space}});
    echo->handle(zgal::Event{zgal::KeyPressed{zgal::Key::Space}});
    CHECK(recorded == std::vector<float>{1.0f, 2.0f});
    CHECK(echo->state().value == 4.0f);
    CHECK_THROWS_WITH_AS((void)Echo::create(ctx, graph, Echo::Inputs{.scale = [](float x) { return x; }}),
                         doctest::Contains("Echo's extern function record isn't given"), ContractViolation);
}

namespace {

class Tallier : public Tally {
public:
    Tallier(VulkanContext& context, const Shaders& shaders, Bindings bindings, uint32_t count)
    : Tally(context, shaders, std::move(bindings)), count_(count) {}

    void update(uint32_t, VkExtent2D) override {}

    // Fewer workgroups than samples: the grid-stride loop covers the rest
    glm::vec3 getDispatchDimensions() override { return workgroupsFor(count_ / 2); }

protected:
    void writePush(uint32_t, Push& push) override { push.count = count_; }

private:
    uint32_t count_;
};

uint32_t collatzSteps(uint32_t n) {
    uint32_t taken = 0;
    while (n > 1 && taken < 1000) {
        n = n % 2 == 0 ? n / 2 : 3 * n + 1;
        ++taken;
    }
    return taken;
}

} // namespace

TEST_CASE("ZGAL's atomics, while loops, subgroup operations, strided ranges and early returns") {
    // Float atomics need a device feature, which the material reports
    const DeviceFeatures features = Tally::requiredFeatures();
    CHECK(features.atomicFloat.shaderBufferFloat32AtomicAdd);
    FeatureContext feature(features);
    auto& ctx = *feature.context;
    REQUIRE((ctx.subgroupProperties().supportedOperations & Tally::kSubgroupOperations) == Tally::kSubgroupOperations);

    constexpr uint32_t kCount = 256;
    std::vector<uint32_t> samples(kCount);
    for (uint32_t i = 0; i < kCount; ++i) {
        samples[i] = (i * 37 + 11) % 200 + 1;
    }
    auto sampleBuffer = Buffer<uint32_t>::createAndInitialize(ctx, samples, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT).value();
    auto make = [&](size_t count) {
        return Buffer<uint32_t>::createAndInitialize(ctx, std::vector<uint32_t>(count, 0u), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
            .value();
    };
    auto counts = make(16);
    auto steps = make(kCount);
    auto subgroupSums = make(kCount);
    auto total = Buffer<float>::createAndInitialize(ctx, std::vector<float>{0.0f}, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                     .value();
    {
        RenderGraph graph(ctx);
        for (auto& [name, buffer] : std::vector<std::pair<const char*, Handle<Buffer<uint32_t>>>>{
                 {"samples", sampleBuffer}, {"counts", counts}, {"steps", steps}, {"subgroupSums", subgroupSums}}) {
            graph.importBuffer<uint32_t>(name, buffer);
        }
        graph.importBuffer<float>("total", total);
        auto& heap = ctx.descriptors();
        auto shaders = Tally::loadShaders(ctx).value();
        auto material = ctx.resources().create<Tallier>(
            ctx, shaders,
            Tally::Bindings{.samples = heap.storageBuffer(sampleBuffer), .counts = heap.storageBuffer(counts),
                            .total = heap.storageBuffer(total), .steps = heap.storageBuffer(steps),
                            .subgroupSums = heap.storageBuffer(subgroupSums)},
            kCount);
        // Atomics read and write their places
        CHECK(material->bindings()[1].access == Access::StorageBufferReadWrite);
        graph.add<DispatchNode>("tally", material);
        graph.execute().value();
        ctx.waitIdleAll();
    }

    // The histogram's grid-stride loop covers every sample; the rest is per invocation, of which
    // there are half as many
    constexpr uint32_t kInvocations = kCount / 2;
    std::vector<uint32_t> expectedCounts(16, 0);
    float expectedTotal = 0.0f;
    for (uint32_t i = 0; i < kCount; ++i) {
        ++expectedCounts[samples[i] % 16];
        if (i < kInvocations) {
            expectedTotal += float(samples[i]);
        }
    }
    CHECK(contents(counts, 16) == expectedCounts);
    CHECK(contents(total, 1)[0] == expectedTotal);
    const auto taken = contents(steps, kCount);
    const auto sums = contents(subgroupSums, kCount);
    const uint32_t subgroupSize = ctx.subgroupProperties().subgroupSize;
    for (uint32_t i = 0; i < kInvocations; ++i) {
        CAPTURE(i);
        CHECK(taken[i] == collatzSteps(samples[i]));
        // Every invocation is below the count, so every subgroup is full
        CHECK(sums[i] == subgroupSize);
    }

    sampleBuffer.reset();
    counts.reset();
    steps.reset();
    subgroupSums.reset();
    total.reset();
    std::lock_guard<std::mutex> lock(feature.mutex);
    CHECK(feature.errors.empty());
    for (const auto& error : feature.errors) {
        MESSAGE(error);
    }
}

TEST_CASE_FIXTURE(GpuTest, "ZGAL fragment shaders discard fragments and take derivatives") {
    auto& ctx = context();
    RenderGraph graph(ctx);
    const CutGraph cut = CutGraph::build(ctx, graph, CutGraph::Inputs{}).value();
    graph.execute().value();
    const auto pixels = cut.readCanvasPixels(graph).value();
    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
            CAPTURE(x);
            CAPTURE(y);
            const uint8_t* p = &pixels[(y * 4 + x) * 4];
            if (x < 2) {
                // Discarded: the clear colour
                CHECK((p[0] == 0 && p[1] == 0));
            } else {
                // fwidth(fragCoord.x) is a pixel
                CHECK((p[0] == 255 && p[1] == 255));
            }
        }
    }
    ctx.waitIdleAll();
}

TEST_CASE_FIXTURE(GpuTest, "ZGAL buffers sized from per-frame inputs follow them") {
    auto& ctx = context();
    {
        RenderGraph graph(ctx);
        auto app = Growing::create(ctx, graph, Growing::Inputs{}).value();
        auto squares = [&] {
            app->beginFrame().value();
            graph.execute().value();
            return app->ids().readSquaresContents(graph).value();
        };
        auto expected = [](uint32_t n) {
            std::vector<uint32_t> values;
            for (uint32_t i = 0; i < n; ++i) {
                values.push_back(i * i);
            }
            return values;
        };
        CHECK(squares() == expected(3));
        app->handle(zgal::Event{zgal::KeyPressed{zgal::Key::Up}});
        CHECK(squares() == expected(6));
        app->handle(zgal::Event{zgal::KeyPressed{zgal::Key::Up}});
        CHECK(squares() == expected(12));
        // Every frame's copy follows
        for (uint32_t frame = 0; frame < ctx.framesInFlight(); ++frame) {
            CHECK(squares() == expected(12));
        }
        app->handle(zgal::Event{zgal::KeyPressed{zgal::Key::Digit0}});
        CHECK(squares().empty());
        ctx.waitIdleAll();
    }
    ctx.collectRetired();
}

TEST_CASE_FIXTURE(GpuTest, "ZGAL array bindings choose textures and buffers per invocation") {
    auto& ctx = context();
    {
        std::vector<FrameBinding<Image>> layers;
        for (uint32_t width : {2u, 4u, 6u}) {
            layers.emplace_back(Image::createEmptyRGBA(ctx, width, 1).value());
        }
        RenderGraph graph(ctx);
        const GatherGraph gather = GatherGraph::build(ctx, graph, GatherGraph::Inputs{.layers = layers}).value();
        CHECK(gather.images.layers.size() == 3);
        graph.execute().value();
        CHECK(gather.readGatheredContents(graph).value() == std::vector<uint32_t>{5002, 9004, 5006});
        CHECK(gather.readEvensContents(graph).value() == std::vector<uint32_t>{5002, 5006});
        CHECK(gather.readOddsContents(graph).value()[0] == 9004);
        ctx.waitIdleAll();
    }
    {
        // Every image must be given
        RenderGraph graph(ctx);
        CHECK_THROWS_WITH_AS((void)GatherGraph::build(ctx, graph, GatherGraph::Inputs{.layers = {FrameBinding<Image>()}}),
                             doctest::Contains("GatherGraph's imported image layers 0 isn't given"), ContractViolation);
    }
}

TEST_CASE_FIXTURE(GpuTest, "ZGAL apps run phases: graphs chosen by state, once nodes, readback events and data uploads") {
    auto& ctx = context();
    {
        RenderGraph seedGraph(ctx);
        RenderGraph useGraph(ctx);
        auto app = Phases::create(ctx, Phases::Graphs{.seedGraph = &seedGraph, .useGraph = &useGraph}, Phases::Inputs{
            .extend = [](const zgal::Data<uint32_t>& seeds) {
                std::vector<uint32_t> grown = seeds.elements();
                grown.push_back(100);
                return zgal::Data<uint32_t>(std::move(grown));
            },
        }).value();
        auto frame = [&] {
            app->beginFrame().value();
            app->renderGraph().execute().value();
        };

        // The seed graph computes and reads back its seeds once
        frame();
        CHECK(&app->renderGraph() == &seedGraph);
        CHECK_FALSE(seedGraph.isEnabled(app->seedGraphIds().nodes.seed));
        CHECK_FALSE(app->state().ready);
        ctx.waitIdleAll();

        // Its copy arrives as an event, which switches to the graph using the seeds
        frame();
        CHECK(app->state().ready);
        CHECK(app->state().seeds.elements() == std::vector<uint32_t>{1, 11, 21, 31});
        CHECK(&app->renderGraph() == &useGraph);
        CHECK(app->useGraphIds().readDoubledContents(useGraph).value() == std::vector<uint32_t>{2, 22, 42, 62});
        frame();
        CHECK(app->state().reads >= 1);
        CHECK(app->state().lastTotal == 128);

        // Changed seeds are uploaded again, into a buffer of their new size
        app->handle(zgal::Event{zgal::KeyPressed{zgal::Key::Space}});
        frame();
        CHECK(app->useGraphIds().readDoubledContents(useGraph).value() == std::vector<uint32_t>{2, 22, 42, 62, 200});
        frame();
        CHECK(app->state().lastTotal == 328);
        ctx.waitIdleAll();
    }
    ctx.collectRetired();
}

TEST_CASE_FIXTURE(GpuTest, "ZGAL's compute library scans and reduces, counted by the CPU or the GPU") {
    auto& ctx = context();
    // Small, one tile, and several tiles per workgroup (with every workgroup of the grid)
    for (const uint32_t size : {5u, 256u, 70001u}) {
        CAPTURE(size);
        std::vector<uint32_t> values(size);
        std::vector<float> floats(size);
        for (uint32_t i = 0; i < size; ++i) {
            values[i] = (i * 7919u) % 100u + 3u;
            floats[i] = static_cast<float>((i * 31u) % 1000u) / 8.0f - 20.0f;
        }
        const uint32_t gpuCount = size - size / 3;
        RenderGraph graph(ctx);
        const LibraryGraph library = LibraryGraph::build(ctx, graph, LibraryGraph::Inputs{
            .gpuCount = gpuCount, .values = values, .floats = floats}).value();
        for (int frame = 0; frame < 2; ++frame) {
            graph.execute().value();
        }

        std::vector<uint32_t> sums(size);
        std::inclusive_scan(values.begin(), values.end(), sums.begin());
        CHECK(library.readSumsContents(graph).value() == sums);
        CHECK(library.readInPlaceContents(graph).value() == sums);
        // Exclusive, over the first gpuCount elements (the rest are left as they were: zero)
        std::vector<uint32_t> before(size, 0);
        std::exclusive_scan(values.begin(), values.begin() + gpuCount, before.begin(), 0u);
        CHECK(library.readBeforeContents(graph).value() == before);
        // The maximum's identity is the smallest int, not 0
        CHECK(library.readMaximaContents(graph).value() == std::vector<int32_t>(size, -1));

        const auto results = library.readResultsContents(graph).value();
        CHECK(results[0] == sums.back());
        CHECK(results[1] == *std::min_element(values.begin(), values.begin() + gpuCount));
        const auto floatResults = library.readFloatResultsContents(graph).value();
        CHECK(floatResults[0] == *std::max_element(floats.begin(), floats.end()));
        CHECK(floatResults[1] == doctest::Approx(std::accumulate(floats.begin(), floats.end(), 0.0)).epsilon(1e-4));
        ctx.waitIdleAll();
    }
}

TEST_CASE_FIXTURE(GpuTest, "ZGAL graphs dispatch and draw what shaders wrote, as many draws as a buffer says") {
    auto& ctx = context();
    // The second draw's first instance (1) draws green; none draws nothing, leaving the clear blue
    const std::vector<std::pair<uint32_t, std::vector<uint8_t>>> cases{
        {1, {255, 0, 0, 255}}, {2, {0, 255, 0, 255}}, {0, {0, 0, 255, 255}}};
    for (const auto& [drawn, color] : cases) {
        CAPTURE(drawn);
        RenderGraph graph(ctx);
        const IndirectGraph indirect = IndirectGraph::build(ctx, graph, IndirectGraph::Inputs{.groups = 5, .drawn = drawn}).value();
        for (int frame = 0; frame < 3; ++frame) {
            graph.execute().value();
        }
        // Workgroups 0 to 4
        CHECK(indirect.readTallyContents(graph).value() == std::vector<uint32_t>{10});
        const auto pixels = indirect.readPicturePixels(graph).value();
        CHECK(std::vector<uint8_t>(pixels.begin(), pixels.begin() + 4) == color);
        ctx.waitIdleAll();
    }
    CHECK(IndirectGraph::requiredFeatures().core.features.multiDrawIndirect == VK_TRUE);
}
