#include "Ubo.h"
#include "VkTestContext.h"
#include "TestShaders.h"
#include "ScreenshotApproval.h"
#include "OffscreenRenderTarget.h"
#include "BasicMaterial.h"
#include "RenderableNode.h"
#include "RenderablesNode.h"

#include <glm/gtc/matrix_transform.hpp>

namespace {

constexpr uint kFrames = 1;
constexpr uint32_t kWidth = 256;
constexpr uint32_t kHeight = 256;

// Same layout as the example's vertex, matching shader.vert's inputs
struct Vertex {
    glm::vec2 pos;
    glm::vec3 color;
    glm::vec2 texCoord0;

    static VkVertexInputBindingDescription getBindingDescription() {
        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;
        bindingDescription.stride = sizeof(Vertex);
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return bindingDescription;
    }

    static std::array<VkVertexInputAttributeDescription, 3> getAttributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 3> attributes{};
        attributes[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, pos)};
        attributes[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color)};
        attributes[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, texCoord0)};
        return attributes;
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
class FixedCameraMaterial : public BasicMaterial<kFrames, 3> {
public:
    FixedCameraMaterial(ResourceManager& resources,
                        VkDevice device,
                        VkPhysicalDevice physicalDevice,
                        VkExtent2D extent,
                        VkRenderPass renderPass,
                        std::vector<Handle<Descriptor>> descriptors,
                        Handle<Buffer<UniformBufferObject>> uniformBuffer)
    : BasicMaterial<kFrames, 3>(device, physicalDevice, descriptors, extent, renderPass,
                                loadTestShader("shader.vert"), loadTestShader("shader.frag"),
                                Vertex::getBindingDescription(), Vertex::getAttributeDescriptions()) {
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

// Everything needed to draw the example's textured quads into an offscreen target
class TexturedScene {
public:
    explicit TexturedScene(VkTestContext& ctx)
    : ctx_(ctx),
    target_(ctx.resources(), kWidth, kHeight, VK_FORMAT_R8G8B8A8_SRGB, ctx.device(), ctx.physicalDevice()) {
        auto& resources = ctx.resources();

        Handle<Image> texture;
        Image::createFromFile(texture, resources, std::string(TEST_TEXTURE_DIR) + "/texture.jpg",
                              ctx.queue(), ctx.commandPool(), ctx.device(), ctx.physicalDevice());
        Handle<VulkanSampler> sampler;
        VK_SUCCESS_OR_THROW(VulkanSampler::createWithAddressMode(sampler, resources, VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                                                 ctx.device(), ctx.physicalDevice()),
                            "Failed to create sampler");

        Handle<Buffer<UniformBufferObject>> uniformBuffer;
        VK_SUCCESS_OR_THROW(Buffer<UniformBufferObject>::create(uniformBuffer, ctx.vulkan(), 1,
                                                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
                            "Failed to create uniform buffer");

        std::vector<Handle<Descriptor>> descriptors{
            resources.create<UniformBufferDescriptor<UniformBufferObject, kFrames>>(
                std::array<Handle<Buffer<UniformBufferObject>>, kFrames>{uniformBuffer}, VK_SHADER_STAGE_VERTEX_BIT),
            resources.create<CombinedImageSamplerDescriptor<kFrames>>(
                VK_SHADER_STAGE_FRAGMENT_BIT, std::array<Handle<Image>, kFrames>{texture}, sampler),
        };

        material_ = resources.create<FixedCameraMaterial>(resources, ctx.device(), ctx.physicalDevice(),
                                                          target_.getExtent(), target_.getRenderPass(),
                                                          descriptors, uniformBuffer);

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = ctx.commandPool();
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = kFrames;
        VK_SUCCESS_OR_THROW(vkAllocateCommandBuffers(ctx.device(), &allocInfo, commandBuffers_.data()),
                            "Failed to allocate command buffers");
    }

    ~TexturedScene() {
        vkQueueWaitIdle(ctx_.queue());
        vkFreeCommandBuffers(ctx_.device(), ctx_.commandPool(), kFrames, commandBuffers_.data());
    }

    Handle<Renderable<kFrames>> createQuad(float scale, glm::vec2 offset) {
        return ctx_.resources().create<MeshRenderable<Vertex, kFrames>>(
            quad(scale, offset), kQuadIndices, material_, ctx_.vulkan());
    }

    // Submits a node's work for frame 0 into the offscreen target and reads back the result
    std::vector<uint8_t> render(RenderNode<kFrames>& node) {
        RenderEvalContext renderContext{
            0, target_.getExtent(),
            0, {target_.getFramebuffer()},
            VK_NULL_HANDLE,
            false,
        };
        node.submit(renderContext);
        vkQueueWaitIdle(ctx_.queue());
        return target_.readback(ctx_.queue(), ctx_.commandPool());
    }

    VkRenderPass renderPass() { return target_.getRenderPass(); }
    std::array<VkCommandBuffer, kFrames> commandBuffers() { return commandBuffers_; }

private:
    VkTestContext& ctx_;
    OffscreenRenderTarget target_;
    Handle<Material<kFrames>> material_;
    std::array<VkCommandBuffer, kFrames> commandBuffers_{};
};

} // namespace

TEST_CASE_FIXTURE(GpuTest, "Screenshot - RenderableNode draws a textured quad") {
    auto& ctx = context();
    std::vector<uint8_t> pixels;
    {
        TexturedScene scene(ctx);
        RenderableNode<kFrames> node(scene.createQuad(1.0f, {0.0f, 0.0f}),
                                     ctx.device(), ctx.queue(), scene.renderPass(), scene.commandBuffers());
        pixels = scene.render(node);
    }
    verifyScreenshot(pixels, kWidth, kHeight);
}

TEST_CASE_FIXTURE(GpuTest, "Screenshot - RenderablesNode draws two quads sharing a material") {
    auto& ctx = context();
    std::vector<uint8_t> pixels;
    {
        TexturedScene scene(ctx);
        std::vector<Handle<Renderable<kFrames>>> renderables{
            scene.createQuad(0.5f, {-0.3f, 0.0f}),
            scene.createQuad(0.5f, {0.3f, 0.0f}),
        };
        CHECK(renderables[0]->getMaterial() == renderables[1]->getMaterial());

        RenderablesNode<kFrames> node(std::move(renderables),
                                      ctx.device(), ctx.queue(), scene.renderPass(), scene.commandBuffers());
        pixels = scene.render(node);
    }
    verifyScreenshot(pixels, kWidth, kHeight);
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
