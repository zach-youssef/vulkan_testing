#include "VkTestContext.h"
#include "TestShaders.h"
#include "graph/RenderGraph.h"
#include "graph/nodes/ReadbackNode.h"
#include "materials/GraphicsMaterial.h"

namespace {

// Draws fullscreen.vert's triangle (no vertex input) in a pushed colour
class ColorMaterial : public GraphicsMaterial {
public:
    ColorMaterial(VulkanContext& ctx, AttachmentFormats formats, GraphicsPipelineOptions options, glm::vec4 color)
    : GraphicsMaterial(ctx, {}, std::move(formats), testShader(ctx, "fullscreen.vert"),
                       testShader(ctx, "constant_color.frag"), std::move(options)), color_(color) {}

protected:
    void writePushConstants(uint32_t, std::vector<uint8_t>& data) override {
        data.resize(data.size() + sizeof(color_));
        memcpy(data.data() + data.size() - sizeof(color_), &color_, sizeof(color_));
    }

private:
    glm::vec4 color_;
};

// Clears a 4x4 RGBA8 image to `background` (and with clearDepth, a depth image), draws the triangle
// (at depth 0) with each colour in turn using the same options, and returns the first pixel
std::vector<uint8_t> draw(VulkanContext& ctx, GraphicsPipelineOptions options, std::array<float, 4> background,
                          std::vector<glm::vec4> colors, std::optional<float> clearDepth = std::nullopt) {
    RenderGraph graph(ctx);
    ImageId image = graph.createImage("color", FrameMode::Shared, VK_FORMAT_R8G8B8A8_UNORM, FixedExtent{{4, 4}});
    RenderTarget target;
    target.color.push_back({image, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, background});
    if (clearDepth) {
        ImageId depth = graph.createImage("depth", FrameMode::Shared, VK_FORMAT_D32_SFLOAT, FixedExtent{{4, 4}});
        target.depth = DepthAttachment{depth, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, *clearDepth};
    }
    std::vector<Handle<Material>> materials;
    for (const auto& color : colors) {
        materials.push_back(ctx.resources().create<ColorMaterial>(ctx, graph.formats(target), options, color));
    }
    graph.addGraphics("draw", [&](NodeBuilder& b) {
        b.renderTo(target);
        for (auto& material : materials) b.uses(material);
    }, [materials](RecordContext& context) {
        for (auto& material : materials) {
            material->bind(context.commandBuffer, context.frameIndex);
            vkCmdDraw(context.commandBuffer, 3, 1, 0, 0);
        }
    });
    NodeId readback = graph.add<ReadbackNode>("readback", image);
    graph.execute();
    auto pixels = graph.nodeAs<ReadbackNode>(readback).pixels();
    return std::vector<uint8_t>(pixels.begin(), pixels.begin() + 4);
}

} // namespace

TEST_CASE_FIXTURE(GpuTest, "Graphics materials can draw without vertex input and blend as configured") {
    auto& ctx = context();
    const std::array<float, 4> grey{0.2f, 0.2f, 0.2f, 1.0f};
    const glm::vec4 color{0.4f, 0.2f, 0.0f, 0.5f};
    GraphicsPipelineOptions options;
    options.cullMode = VK_CULL_MODE_NONE;

    options.blend = {BlendMode::Opaque};
    CHECK(draw(ctx, options, grey, {color}) == std::vector<uint8_t>{102, 51, 0, 128});
    // Each draw adds to the last
    options.blend = {BlendMode::Additive};
    CHECK(draw(ctx, options, grey, {color, color}) == std::vector<uint8_t>{255, 153, 51, 255});
    // 0.4 * 0.5 + 0.2 * 0.5 = 0.3; alpha 0.5 + 1 * 0.5 = 1
    options.blend = {BlendMode::Alpha};
    CHECK(draw(ctx, options, grey, {color}) == std::vector<uint8_t>{77, 51, 26, 255});
    // 0.4 + 0.2 * 0.5 = 0.5
    options.blend = {BlendMode::PremultipliedAlpha};
    CHECK(draw(ctx, options, grey, {color}) == std::vector<uint8_t>{128, 77, 26, 255});
}

TEST_CASE_FIXTURE(GpuTest, "Graphics materials cull the faces their options say") {
    auto& ctx = context();
    const std::array<float, 4> black{0.0f, 0.0f, 0.0f, 1.0f};
    const glm::vec4 white{1.0f};
    const std::vector<uint8_t> drawn{255, 255, 255, 255};
    GraphicsPipelineOptions options;

    options.cullMode = VK_CULL_MODE_NONE;
    CHECK(draw(ctx, options, black, {white}) == drawn);
    options.cullMode = VK_CULL_MODE_FRONT_AND_BACK;
    CHECK(draw(ctx, options, black, {white}) != drawn);
    // The triangle is one or the other, whichever its winding makes it
    options.cullMode = VK_CULL_MODE_BACK_BIT;
    const bool front = draw(ctx, options, black, {white}) == drawn;
    options.cullMode = VK_CULL_MODE_FRONT_BIT;
    CHECK((draw(ctx, options, black, {white}) == drawn) != front);
    // Flipping which winding is the front flips the result
    options.frontFace = VK_FRONT_FACE_CLOCKWISE;
    CHECK((draw(ctx, options, black, {white}) == drawn) == front);
}

TEST_CASE_FIXTURE(GpuTest, "Graphics materials depth test as configured") {
    auto& ctx = context();
    const std::array<float, 4> black{0.0f, 0.0f, 0.0f, 1.0f};
    const glm::vec4 white{1.0f};
    const std::vector<uint8_t> drawn{255, 255, 255, 255};
    GraphicsPipelineOptions options;
    options.cullMode = VK_CULL_MODE_NONE;

    // The triangle (depth 0) isn't less than a cleared depth of 0
    CHECK(draw(ctx, options, black, {white}, 0.0f) != drawn);
    CHECK(draw(ctx, options, black, {white}, 1.0f) == drawn);
    options.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
    CHECK(draw(ctx, options, black, {white}, 0.0f) == drawn);
    options.depthCompare = VK_COMPARE_OP_LESS;
    options.depthTest = false;
    CHECK(draw(ctx, options, black, {white}, 0.0f) == drawn);
    // Bias is in units of the depth format's resolution at the triangle's depth, which for a float
    // format at depth 0 is too small to observe; check a biased pipeline builds and draws
    options.depthTest = true;
    options.depthBias = DepthBias{.constantFactor = 4.0f, .slopeFactor = 1.0f};
    CHECK(draw(ctx, options, black, {white}, 1.0f) == drawn);
}

TEST_CASE_FIXTURE(GpuTest, "Graphics materials need one blend mode, or one per colour attachment") {
    auto& ctx = context();
    GraphicsPipelineOptions options;
    options.blend = {BlendMode::Opaque, BlendMode::Additive};
    CHECK_THROWS_AS(ColorMaterial(ctx, AttachmentFormats{{VK_FORMAT_R8G8B8A8_UNORM}}, options, glm::vec4(1.0f)),
                    std::invalid_argument);
}
