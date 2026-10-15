#include "Ubo.h"
#include "VkTestContext.h"
#include "TestShaders.h"
#include "OffscreenRenderTarget.h"
#include "BasicMaterial.h"

namespace {

constexpr uint kFrames = 2;

struct Vertex {
    glm::vec2 pos;
    glm::vec3 color;
    glm::vec2 texCoord0;
};

std::array<VkVertexInputAttributeDescription, 3> attributes() {
    return {{
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, pos)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, texCoord0)},
    }};
}

class TestMaterial : public BasicMaterial<kFrames, 3> {
public:
    TestMaterial(VkTestContext& ctx, VkExtent2D extent, VkRenderPass renderPass,
                 std::vector<Handle<Descriptor>> descriptors)
    : BasicMaterial<kFrames, 3>(ctx.device(), ctx.physicalDevice(), descriptors, extent, renderPass,
                                loadTestShader("shader.vert"), loadTestShader("shader.frag"),
                                {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX}, attributes()) {}

    void update(uint32_t, VkExtent2D) override {}

    // Exposed so tests can re-populate after binding
    using Material<kFrames>::populateDescriptorSet;
};

} // namespace

TEST_CASE_FIXTURE(GpuTest, "Material tolerates descriptors whose buffers are bound after construction") {
    auto& ctx = context();
    auto& resources = ctx.resources();

    OffscreenRenderTarget target(ctx.vulkan(), 16, 16, VK_FORMAT_R8G8B8A8_UNORM);

    Handle<Image> texture;
    Image::createEmptyRGBA(texture, ctx.vulkan(), 4, 4);
    Handle<VulkanSampler> sampler;
    REQUIRE(VulkanSampler::createWithAddressMode(sampler, resources, VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                                 ctx.device(), ctx.physicalDevice()) == VK_SUCCESS);

    // The uniform buffers are not known yet when the material is created
    using UboDescriptor = UniformBufferDescriptor<UniformBufferObject, kFrames>;
    auto uboDescriptor = resources.create<UboDescriptor>(std::array<Handle<Buffer<UniformBufferObject>>, kFrames>{},
                                                         VK_SHADER_STAGE_VERTEX_BIT);
    std::vector<Handle<Descriptor>> descriptors{
        uboDescriptor,
        resources.create<CombinedImageSamplerDescriptor<kFrames>>(
            VK_SHADER_STAGE_FRAGMENT_BIT, std::array<Handle<Image>, kFrames>{texture, texture}, sampler),
    };

    // Creating the material must not write the unbound (VK_NULL_HANDLE) buffers
    auto material = resources.create<TestMaterial>(ctx, target.getExtent(), target.getRenderPass(), descriptors);
    CHECK(material->getPipeline() != VK_NULL_HANDLE);

    // Binding the buffers later and re-populating writes them
    for (uint frame = 0; frame < kFrames; ++frame) {
        Handle<Buffer<UniformBufferObject>> buffer;
        REQUIRE(Buffer<UniformBufferObject>::create(buffer, ctx.vulkan(), 1, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == VK_SUCCESS);
        uboDescriptor->bindBuffer(frame, buffer);
        material->populateDescriptorSet(frame);
    }
}
