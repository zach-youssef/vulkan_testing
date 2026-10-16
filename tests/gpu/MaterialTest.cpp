#include "resources/Ubo.h"
#include "VkTestContext.h"
#include "TestShaders.h"
#include "materials/GraphicsMaterial.h"

namespace {

struct Vertex {
    glm::vec2 pos;
    glm::vec3 color;
    glm::vec2 texCoord0;
};

VertexLayout vertexLayout() {
    return VertexLayout::interleaved(sizeof(Vertex), {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, pos)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, texCoord0)},
    });
}

// The example's material (shader.vert / shader.frag), with an extra push constant
class TestMaterial : public GraphicsMaterial {
public:
    TestMaterial(VulkanContext& ctx, std::vector<MaterialBinding> bindings)
    : GraphicsMaterial(ctx, std::move(bindings), AttachmentFormats{{VK_FORMAT_R8G8B8A8_UNORM}, VK_FORMAT_D32_SFLOAT},
                       testShader(ctx, "shader.vert"), testShader(ctx, "shader.frag"),
                       GraphicsPipelineOptions{.vertexLayout = vertexLayout()}) {}

    void update(uint32_t, VkExtent2D) override {}

    uint32_t extra = 0;

protected:
    void writePushConstants(uint32_t, std::vector<uint8_t>& data) override {
        if (extra) {
            data.resize(data.size() + sizeof(extra));
            memcpy(data.data() + data.size() - sizeof(extra), &extra, sizeof(extra));
        }
    }
};

Handle<Buffer<UniformBufferObject>> uniformBuffer(VulkanContext& ctx) {
    return Buffer<UniformBufferObject>::create(ctx, 1, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT).value();
}

std::vector<uint32_t> asIndices(const std::vector<uint8_t>& data) {
    std::vector<uint32_t> indices(data.size() / sizeof(uint32_t));
    memcpy(indices.data(), data.data(), indices.size() * sizeof(uint32_t));
    return indices;
}

} // namespace

TEST_CASE_FIXTURE(GpuTest, "Materials push their bindings' descriptor indices in order") {
    auto& ctx = context();
    auto& heap = ctx.descriptors();

    auto texture = Image::createEmptyRGBA(ctx, 4, 4).value();
    auto sampler = VulkanSampler::createWithAddressMode(ctx, VK_SAMPLER_ADDRESS_MODE_REPEAT).value();

    auto ubo = heap.storageBuffer(uniformBuffer(ctx));
    auto textureEntry = heap.sampledImage(texture);
    auto samplerEntry = heap.sampler(sampler);
    auto material = ctx.resources().create<TestMaterial>(ctx, std::vector<MaterialBinding>{
        MaterialBinding::storageBuffer(ubo),
        MaterialBinding::sampled(textureEntry),
        MaterialBinding::sampler(samplerEntry),
    });
    // The pipeline is built on demand (render graphs build it when compiling)
    CHECK(material->getPipeline() == VK_NULL_HANDLE);
    CHECK_OK(material->ensurePipeline());
    CHECK(material->getPipeline() != VK_NULL_HANDLE);
    CHECK(material->pipelineVersion() == 1);
    const VkPipeline pipeline = material->getPipeline();
    CHECK_OK(material->ensurePipeline());
    CHECK(material->getPipeline() == pipeline);
    CHECK(material->getPipelineLayout() == heap.pipelineLayout());

    const std::vector<uint32_t> expected{ubo->index(), textureEntry->index(), samplerEntry->index()};
    CHECK(asIndices(material->pushConstants(0)) == expected);
    CHECK(asIndices(material->pushConstants(1)) == expected);

    // Extra push constants follow the indices
    material->extra = 1234;
    auto withExtra = asIndices(material->pushConstants(0));
    REQUIRE(withExtra.size() == 4);
    CHECK(withExtra[3] == 1234);
}

TEST_CASE_FIXTURE(GpuTest, "Material bindings may have an entry per frame that follows frames in flight") {
    auto& ctx = context();
    auto& heap = ctx.descriptors();
    const uint32_t originalFrames = ctx.framesInFlight();

    auto texture = Image::createEmptyRGBA(ctx, 4, 4).value();
    auto sampler = VulkanSampler::createWithAddressMode(ctx, VK_SAMPLER_ADDRESS_MODE_REPEAT).value();

    // A uniform buffer (and its entry) per frame; the texture and sampler are shared by every frame
    auto entries = ctx.resources().create<PerFrameHandles<DescriptorEntry>>(ctx, [&ctx](uint32_t) {
        return ctx.descriptors().storageBuffer(uniformBuffer(ctx));
    });
    auto material = ctx.resources().create<TestMaterial>(ctx, std::vector<MaterialBinding>{
        MaterialBinding::storageBuffer(entries),
        MaterialBinding::sampled(heap.sampledImage(texture)),
        MaterialBinding::sampler(heap.sampler(sampler)),
    });
    CHECK(asIndices(material->pushConstants(0))[0] == (*entries)[0]->index());
    CHECK(asIndices(material->pushConstants(1))[0] == (*entries)[1]->index());
    CHECK(asIndices(material->pushConstants(0))[1] == asIndices(material->pushConstants(1))[1]);

    ctx.setFramesInFlight(originalFrames + 1);
    ctx.advanceFrame().value();
    CHECK(asIndices(material->pushConstants(originalFrames))[0] == (*entries)[originalFrames]->index());
    ctx.setFramesInFlight(originalFrames);
    ctx.advanceFrame().value();
    entries.reset();
}

TEST_CASE_FIXTURE(GpuTest, "Binding a material without an entry for the frame is a contract violation") {
    auto& ctx = context();
    auto material = ctx.resources().create<TestMaterial>(ctx, std::vector<MaterialBinding>{
        MaterialBinding::storageBuffer(nullptr),
    });
    CHECK_THROWS_WITH_AS(material->pushConstants(0), doctest::Contains("has no descriptor entry"), ContractViolation);
}

TEST_CASE_FIXTURE(GpuTest, "Binding a material before its pipeline is built is a contract violation") {
    auto& ctx = context();
    auto material = ctx.resources().create<TestMaterial>(ctx, std::vector<MaterialBinding>{});
    // Checked before any command is recorded
    CHECK_THROWS_WITH_AS(material->bind(VK_NULL_HANDLE, 0), doctest::Contains("before its pipeline was built"),
                         ContractViolation);
}
