#include "resources/Ubo.h"
#include "VkTestContext.h"
#include "TestShaders.h"
#include "materials/BasicMaterial.h"

namespace {

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

class TestMaterial : public BasicMaterial<3> {
public:
    TestMaterial(VulkanContext& ctx, std::vector<Handle<Descriptor>> descriptors)
    : BasicMaterial<3>(ctx, descriptors, AttachmentFormats{{VK_FORMAT_R8G8B8A8_UNORM}, VK_FORMAT_D32_SFLOAT},
                                loadTestShader("shader.vert"), loadTestShader("shader.frag"),
                                {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX}, attributes()) {}

    void update(uint32_t, VkExtent2D) override {}

    // Exposed so tests can re-populate after binding
    using Material::populateDescriptorSet;
};

} // namespace

TEST_CASE_FIXTURE(GpuTest, "Material tolerates descriptors whose buffers are bound after construction") {
    auto& ctx = context();
    auto& resources = ctx.resources();


    Handle<Image> texture;
    Image::createEmptyRGBA(texture, ctx, 4, 4);
    Handle<VulkanSampler> sampler;
    REQUIRE(VulkanSampler::createWithAddressMode(sampler, ctx, VK_SAMPLER_ADDRESS_MODE_REPEAT) == VK_SUCCESS);

    // The uniform buffers are not known yet when the material is created
    using UboDescriptor = UniformBufferDescriptor<UniformBufferObject>;
    auto uboDescriptor = resources.create<UboDescriptor>(nullptr, VK_SHADER_STAGE_VERTEX_BIT);
    std::vector<Handle<Descriptor>> descriptors{
        uboDescriptor,
        resources.create<CombinedImageSamplerDescriptor>(VK_SHADER_STAGE_FRAGMENT_BIT, texture, sampler),
    };

    // Creating the material, and using its sets, must not write the unbound (VK_NULL_HANDLE) buffers
    auto material = resources.create<TestMaterial>(ctx, descriptors);
    CHECK(material->getPipeline() != VK_NULL_HANDLE);
    CHECK(*material->getDescriptorSet(0) != VK_NULL_HANDLE);

    // Binding the buffers later and re-populating writes them
    auto buffers = resources.create<PerFrameHandles<Buffer<UniformBufferObject>>>(ctx, [&ctx](uint32_t) {
        Handle<Buffer<UniformBufferObject>> buffer;
        VK_SUCCESS_OR_THROW(Buffer<UniformBufferObject>::create(buffer, ctx, 1, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
                            "Failed to create uniform buffer");
        return buffer;
    });
    uboDescriptor->bind(buffers);
    for (uint32_t frame = 0; frame < ctx.framesInFlight(); ++frame) {
        material->populateDescriptorSet(frame);
    }
}

TEST_CASE_FIXTURE(GpuTest, "Material creates and writes descriptor sets for frames added at runtime") {
    auto& ctx = context();
    auto& resources = ctx.resources();
    const uint32_t originalFrames = ctx.framesInFlight();

    Handle<Image> texture;
    Image::createEmptyRGBA(texture, ctx, 4, 4);
    Handle<VulkanSampler> sampler;
    REQUIRE(VulkanSampler::createWithAddressMode(sampler, ctx, VK_SAMPLER_ADDRESS_MODE_REPEAT) == VK_SUCCESS);

    auto buffers = resources.create<PerFrameHandles<Buffer<UniformBufferObject>>>(ctx, [&ctx](uint32_t) {
        Handle<Buffer<UniformBufferObject>> buffer;
        VK_SUCCESS_OR_THROW(Buffer<UniformBufferObject>::create(buffer, ctx, 1, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
                            "Failed to create uniform buffer");
        return buffer;
    });
    // A per-frame uniform buffer and a texture shared by every frame
    std::vector<Handle<Descriptor>> descriptors{
        resources.create<UniformBufferDescriptor<UniformBufferObject>>(buffers, VK_SHADER_STAGE_VERTEX_BIT),
        resources.create<CombinedImageSamplerDescriptor>(VK_SHADER_STAGE_FRAGMENT_BIT, texture, sampler),
    };
    auto material = resources.create<TestMaterial>(ctx, descriptors);
    buffers.reset();

    const VkDescriptorSet frame0 = *material->getDescriptorSet(0);
    CHECK_THROWS(material->getDescriptorSet(originalFrames));

    ctx.setFramesInFlight(originalFrames + 2);
    ctx.advanceFrame();
    // Existing frames keep their sets; new frames get sets of their own
    CHECK(*material->getDescriptorSet(0) == frame0);
    const VkDescriptorSet added = *material->getDescriptorSet(originalFrames + 1);
    CHECK(added != VK_NULL_HANDLE);
    CHECK(added != frame0);

    ctx.setFramesInFlight(originalFrames);
    ctx.advanceFrame();
    CHECK_THROWS(material->getDescriptorSet(originalFrames));
}
