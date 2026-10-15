#include "resources/Ubo.h"
#include "VkTestContext.h"
#include "TestShaders.h"
#include "ScreenshotApproval.h"
#include "materials/GraphicsMaterial.h"
#include "graph/nodes/ReadbackNode.h"
#include "graph/nodes/RenderablesNode.h"

#include <glm/gtc/matrix_transform.hpp>

namespace {

constexpr uint32_t kWidth = 256;
constexpr uint32_t kHeight = 256;

// Same layout as the example's vertex, matching shader.vert's inputs
struct Vertex {
    glm::vec2 pos;
    glm::vec3 color;
    glm::vec2 texCoord0;

    static VertexLayout layout() {
        return VertexLayout::interleaved(sizeof(Vertex), {
            {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, pos)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, texCoord0)},
        });
    }
};

std::vector<Vertex> quad(float scale, glm::vec2 offset) {
    std::vector<Vertex> vertices = {
        {{-0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
        {{0.5f, -0.5f},  {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
        {{0.5f, 0.5f},   {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
        {{-0.5f, 0.5f},  {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f}},
    };
    for (auto& vertex : vertices) {
        vertex.pos = vertex.pos * scale + offset;
    }
    return vertices;
}

const std::vector<uint16_t> kQuadIndices = {0, 1, 2, 2, 3, 0};

// The example's textured material (shader.vert / shader.frag) with a fixed camera, so
// renders are deterministic: the example's view at time = 0
class FixedCameraMaterial : public GraphicsMaterial {
public:
    FixedCameraMaterial(VulkanContext& context,
                        VkExtent2D extent,
                        AttachmentFormats formats,
                        std::vector<MaterialBinding> bindings,
                        Handle<Buffer<UniformBufferObject>> uniformBuffer)
    : GraphicsMaterial(context, std::move(bindings), std::move(formats),
                       testShader(context, "shader.vert"), testShader(context, "shader.frag"),
                       GraphicsPipelineOptions{.vertexLayout = Vertex::layout()}) {
        auto view = glm::lookAt(glm::vec3(2.0f, 2.0f, 2.0f), glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        auto projection = glm::perspective(glm::radians(45.0f), extent.width / (float) extent.height, 0.1f, 10.0f);
        projection[1][1] *= -1; // GLM's clip space Y is inverted relative to Vulkan's
        auto ubo = UniformBufferObject::fromModelViewProjection(glm::mat4(1.0f), view, projection);

        uniformBuffer->mapAndExecute(0, sizeof(ubo), [&ubo](void* data) {
            memcpy(data, &ubo, sizeof(ubo));
        });
    }

    void update(uint32_t, VkExtent2D) override {}
};

// Everything needed to draw the example's textured quads into a graph-created image and read it back
class TexturedScene {
public:
    explicit TexturedScene(VulkanContext& ctx) : ctx_(ctx), graph_(ctx) {
        auto& resources = ctx.resources();

        Image::createFromFile(texture_, ctx, std::string(TEST_TEXTURE_DIR) + "/texture.jpg");
        Handle<VulkanSampler> sampler;
        VK_SUCCESS_OR_THROW(VulkanSampler::createWithAddressMode(sampler, ctx, VK_SAMPLER_ADDRESS_MODE_REPEAT),
                            "Failed to create sampler");

        Handle<Buffer<UniformBufferObject>> uniformBuffer;
        VK_SUCCESS_OR_THROW(Buffer<UniformBufferObject>::create(uniformBuffer, ctx, 1,
                                                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
                            "Failed to create uniform buffer");

        color_ = graph_.createImage("color", FrameMode::Shared, VK_FORMAT_R8G8B8A8_SRGB, FixedExtent{{kWidth, kHeight}});
        ImageId depth = graph_.createImage("depth", FrameMode::Shared, VK_FORMAT_D32_SFLOAT, FixedExtent{{kWidth, kHeight}});
        target_.color.push_back({color_, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {0.0f, 0.0f, 0.0f, 0.0f}});
        target_.depth = DepthAttachment{depth};
        // Bindings in the order shader.vert and shader.frag expect: uniforms, texture, sampler
        auto& heap = ctx.descriptors();
        std::vector<MaterialBinding> bindings{
            MaterialBinding::storageBuffer(heap.storageBuffer(uniformBuffer), Access::StorageBufferRead,
                                           VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT),
            MaterialBinding::sampled(heap.sampledImage(texture_), VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT),
            MaterialBinding::sampler(heap.sampler(sampler)),
        };
        material_ = resources.create<FixedCameraMaterial>(ctx, VkExtent2D{kWidth, kHeight}, graph_.formats(target_),
                                                          std::move(bindings), uniformBuffer);
    }

    Handle<Renderable> createQuad(float scale, glm::vec2 offset) {
        auto mesh = ctx_.resources().create<Mesh>(ctx_, quad(scale, offset), kQuadIndices);
        return ctx_.resources().create<MeshRenderable>(std::move(mesh), material_);
    }

    // Draws renderables in a frame and reads back the result
    std::vector<uint8_t> render(std::vector<Handle<Renderable>> renderables) {
        // The material's bindings declare its uniform buffer and texture uses
        graph_.add<RenderablesNode>("draw", std::move(renderables), target_);
        NodeId readback = graph_.add<ReadbackNode>("readback", color_);
        graph_.execute();
        return graph_.nodeAs<ReadbackNode>(readback).pixels();
    }

    RenderGraph& graph() { return graph_; }

private:
    VulkanContext& ctx_;
    RenderGraph graph_;
    Handle<Image> texture_;
    ImageId color_;
    RenderTarget target_;
    Handle<Material> material_;
};

} // namespace

TEST_CASE_FIXTURE(GpuTest, "Screenshot - RenderablesNode draws a textured quad") {
    auto& ctx = context();
    std::vector<uint8_t> pixels;
    {
        TexturedScene scene(ctx);
        pixels = scene.render({scene.createQuad(1.0f, {0.0f, 0.0f})});
    }
    verifyScreenshot(pixels, kWidth, kHeight);
}

TEST_CASE_FIXTURE(GpuTest, "Screenshot - RenderablesNode draws two quads sharing a material") {
    auto& ctx = context();
    std::vector<uint8_t> pixels;
    {
        TexturedScene scene(ctx);
        std::vector<Handle<Renderable>> renderables{
            scene.createQuad(0.5f, {-0.3f, 0.0f}),
            scene.createQuad(0.5f, {0.3f, 0.0f}),
        };
        CHECK(renderables[0]->getMaterial() == renderables[1]->getMaterial());
        pixels = scene.render(std::move(renderables));
    }
    verifyScreenshot(pixels, kWidth, kHeight);
}

TEST_CASE_FIXTURE(GpuTest, "Frames added at runtime render the same as existing frames") {
    auto& ctx = context();
    const uint32_t originalFrames = ctx.framesInFlight();

    TexturedScene scene(ctx);
    const auto expected = scene.render({scene.createQuad(1.0f, {0.0f, 0.0f})});

    // The graph's command buffers and the material's descriptor sets grow with the frames in flight
    ctx.setFramesInFlight(originalFrames + 1);
    for (uint32_t frame = 0; frame <= originalFrames + 1; ++frame) {
        scene.graph().execute();
    }
    auto& readback = scene.graph().nodeAs<ReadbackNode>(NodeId{1});
    CHECK(readback.pixels() == expected);

    ctx.setFramesInFlight(originalFrames);
    scene.graph().execute();
    scene.graph().execute();
    CHECK(readback.pixels() == expected);
}

TEST_CASE("compareWithFlip passes identical images and flags visibly different ones") {
    constexpr uint32_t size = 32;
    std::vector<uint8_t> gradient(size * size * 4);
    for (uint32_t i = 0; i < size * size; ++i) {
        gradient[i * 4 + 0] = static_cast<uint8_t>(i % size * 8);
        gradient[i * 4 + 1] = static_cast<uint8_t>(i / size * 8);
        gradient[i * 4 + 2] = 128;
        gradient[i * 4 + 3] = 255;
    }

    const ScreenshotTolerance tolerance;

    auto identical = compareWithFlip(gradient, gradient, size, size, tolerance.highErrorThreshold);
    CHECK(identical.meanError == doctest::Approx(0.0f));
    CHECK(identical.highErrorFraction == 0.0f);

    // A one-level change everywhere is imperceptible
    auto nudged = gradient;
    for (size_t i = 0; i < nudged.size(); i += 4) {
        nudged[i] = static_cast<uint8_t>(std::min(255, nudged[i] + 1));
    }
    auto subtle = compareWithFlip(gradient, nudged, size, size, tolerance.highErrorThreshold);
    CHECK(subtle.meanError <= tolerance.maxMeanError);
    CHECK(subtle.highErrorFraction <= tolerance.maxHighErrorFraction);

    // A small white square is a localized but obvious defect
    auto defect = gradient;
    for (uint32_t y = 12; y < 20; ++y) {
        for (uint32_t x = 12; x < 20; ++x) {
            for (int channel = 0; channel < 3; ++channel) {
                defect[(y * size + x) * 4 + channel] = 255;
            }
        }
    }
    auto obvious = compareWithFlip(gradient, defect, size, size, tolerance.highErrorThreshold);
    CHECK(obvious.highErrorFraction > tolerance.maxHighErrorFraction);
}
