#include "VkTestContext.h"
#include "TestShaders.h"
#include "graph/RenderGraph.h"
#include "graph/nodes/BufferReadbackNode.h"
#include "graph/nodes/DispatchNode.h"
#include "graph/nodes/IndirectDrawNode.h"
#include "graph/nodes/ReadbackNode.h"
#include "materials/GraphicsMaterial.h"
#include "resources/Model.h"
#include "common/ResultChecks.h"

#include <array>

namespace {

// Writes words into a buffer with write_words.comp, as a shader writing indirect commands would
class WordsMaterial : public ComputeMaterial {
public:
    WordsMaterial(VulkanContext& ctx, FrameBinding<DescriptorEntry> target, std::vector<uint32_t> words)
    : ComputeMaterial(ctx, {MaterialBinding::storageBuffer(std::move(target), Access::StorageBufferWrite)},
                      testShader(ctx, "write_words.comp")),
      words_(std::move(words)) {
        if (words_.size() > 12) {
            contractViolation("write_words.comp writes at most 12 words");
        }
    }

    void update(uint32_t, VkExtent2D) override {}
    glm::vec3 getDispatchDimensions() override { return {1, 1, 1}; }

protected:
    void writePushConstants(uint32_t, std::vector<uint8_t>& data) override {
        std::array<uint32_t, 13> push{static_cast<uint32_t>(words_.size())};
        std::copy(words_.begin(), words_.end(), push.begin() + 1);
        const size_t start = data.size();
        data.resize(start + sizeof(push));
        memcpy(data.data() + start, push.data(), sizeof(push));
    }

private:
    std::vector<uint32_t> words_;
};

// Counts its workgroups with count_workgroups.comp; dispatched indirectly
class CountMaterial : public ComputeMaterial {
public:
    CountMaterial(VulkanContext& ctx, FrameBinding<DescriptorEntry> counts)
    : ComputeMaterial(ctx, {MaterialBinding::storageBuffer(std::move(counts), Access::StorageBufferReadWrite)},
                      testShader(ctx, "count_workgroups.comp")) {}

    void update(uint32_t, VkExtent2D) override {}
    glm::vec3 getDispatchDimensions() override { return {0, 0, 0}; }
};

// Draws red for instance 0 and green for the others: a mesh's vertices (position_instance.vert), or
// without one, a triangle covering the target (fullscreen_instance.vert)
Handle<GraphicsMaterial> drawMaterial(VulkanContext& ctx, AttachmentFormats formats, bool mesh) {
    GraphicsPipelineOptions options;
    options.cullMode = VK_CULL_MODE_NONE;
    if (mesh) {
        options.vertexLayout = VertexLayout::interleaved(sizeof(glm::vec2), {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}});
    }
    return ctx.resources().create<GraphicsMaterial>(
        ctx, std::vector<MaterialBinding>{}, std::move(formats),
        testShader(ctx, mesh ? "position_instance.vert" : "fullscreen_instance.vert"),
        testShader(ctx, "instance_color.frag"), options);
}

// A buffer the graph creates holding words, written by a shader first
BufferId commandBuffer(RenderGraph& graph, VulkanContext& ctx, const std::string& name, std::vector<uint32_t> words) {
    BufferId buffer = graph.createBuffer(name, FrameMode::PerFrame, VkDeviceSize(words.size() * sizeof(uint32_t)));
    graph.add<DispatchNode>("write " + name,
                            ctx.resources().create<WordsMaterial>(ctx, graph.storageBufferEntry(buffer), std::move(words)));
    return buffer;
}

using Pixel = std::vector<uint8_t>;
const Pixel kRed{255, 0, 0, 255};
const Pixel kGreen{0, 255, 0, 255};
const Pixel kBlue{0, 0, 255, 255};

// Draws into a 4x4 image cleared blue with an IndirectDrawNode drawing commands a shader wrote (and
// with a count, as many as a shader wrote), and returns its first pixel. mesh: draw a quad with
// 16-bit indices.
Pixel drawIndirect(VulkanContext& ctx, bool mesh, std::vector<uint32_t> commands, uint32_t maxDraws,
                   std::optional<uint32_t> count = std::nullopt) {
    RenderGraph graph(ctx);
    ImageId image = graph.createImage("color", FrameMode::Shared, VK_FORMAT_R8G8B8A8_UNORM, FixedExtent{{4, 4}});
    RenderTarget target;
    target.color.push_back({image, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {0, 0, 1, 1}});
    IndirectDrawNode::Desc desc;
    desc.material = drawMaterial(ctx, graph.formats(target), mesh);
    desc.target = target;
    desc.commands = {commandBuffer(graph, ctx, "commands", std::move(commands)), 0};
    desc.maxDraws = maxDraws;
    if (count) {
        desc.count = BufferOffset{commandBuffer(graph, ctx, "count", {*count}), 0};
    }
    if (mesh) {
        const std::vector<glm::vec2> corners{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        const std::vector<uint16_t> indices{0, 1, 2, 2, 3, 0};
        desc.mesh = Mesh::create(ctx, corners, indices).value();
    }
    graph.add<IndirectDrawNode>("draw", std::move(desc));
    NodeId readback = graph.add<ReadbackNode>("readback", image);
    graph.execute().value();
    auto pixels = graph.nodeAs<ReadbackNode>(readback).pixels().value();
    return Pixel(pixels.begin(), pixels.begin() + 4);
}

} // namespace

TEST_CASE_FIXTURE(GpuTest, "DispatchNode dispatches the workgroups a command a shader wrote says") {
    auto& ctx = context();
    RenderGraph graph(ctx);
    BufferId command = commandBuffer(graph, ctx, "command", {3, 1, 1});
    BufferId counts = graph.createBuffer("counts", FrameMode::PerFrame, 2 * sizeof(uint32_t));
    graph.addTransfer("clear", [counts](NodeBuilder& b) { b.write(counts, Access::TransferDst); },
                      [counts](RecordContext& record) {
                          vkCmdFillBuffer(record.commandBuffer, record.graph.buffer(counts, record.frameIndex), 0, VK_WHOLE_SIZE, 0);
                      });
    graph.add<DispatchNode>("count", ctx.resources().create<CountMaterial>(ctx, graph.storageBufferEntry(counts)),
                            BufferOffset{command, 0});
    NodeId readback = graph.add<BufferReadbackNode>("readback", counts);
    for (int frame = 0; frame < 3; ++frame) {
        graph.execute().value();
        // Workgroups 0, 1 and 2
        CHECK(graph.nodeAs<BufferReadbackNode>(readback).contents<uint32_t>().value() == std::vector<uint32_t>{3, 3});
    }
}

TEST_CASE_FIXTURE(GpuTest, "IndirectDrawNode draws the commands shaders wrote") {
    auto& ctx = context();
    // Without vertex input: a triangle covering the target, or nothing for no instances
    CHECK(drawIndirect(ctx, false, {3, 1, 0, 0}, 1) == kRed);
    CHECK(drawIndirect(ctx, false, {3, 0, 0, 0}, 1) == kBlue);
    // Several commands, the second's firstInstance (1) choosing green
    CHECK(drawIndirect(ctx, false, {3, 1, 0, 0, 3, 1, 0, 1}, 2) == kGreen);
    CHECK(drawIndirect(ctx, false, {3, 1, 0, 0, 3, 1, 0, 1}, 1) == kRed);
    // Indexed, from a mesh: index count, instance count, first index, vertex offset, first instance
    CHECK(drawIndirect(ctx, true, {6, 1, 0, 0, 0}, 1) == kRed);
    CHECK(drawIndirect(ctx, true, {6, 1, 0, 0, 0, 6, 1, 0, 0, 1}, 2) == kGreen);
}

TEST_CASE_FIXTURE(GpuTest, "IndirectDrawNode draws as many commands as a count says, where the device can") {
    auto& ctx = context();
    if (!ctx.enabledFeatures().vulkan12.drawIndirectCount) {
        // Code drawing with counts checks first (as ZGAL's generated code does)
        RenderGraph graph(ctx);
        BufferId buffer = graph.createBuffer("commands", FrameMode::Shared, 64);
        IndirectDrawNode::Desc desc;
        desc.material = drawMaterial(ctx, AttachmentFormats{{VK_FORMAT_R8G8B8A8_UNORM}}, false);
        desc.commands = {buffer, 0};
        desc.count = BufferOffset{buffer, 32};
        CHECK_THROWS_AS(graph.add<IndirectDrawNode>("draw", std::move(desc)), ContractViolation);
        MESSAGE("This device lacks drawIndirectCount");
        return;
    }
    CHECK(drawIndirect(ctx, false, {3, 1, 0, 0, 3, 1, 0, 1}, 2, 1) == kRed);
    CHECK(drawIndirect(ctx, false, {3, 1, 0, 0, 3, 1, 0, 1}, 2, 2) == kGreen);
    CHECK(drawIndirect(ctx, true, {6, 1, 0, 0, 0}, 1, 0) == kBlue);
}

TEST_CASE_FIXTURE(GpuTest, "Uploaded models keep their primitives in one mesh, with a draw and bounds each") {
    auto& ctx = context();
    const auto data = ModelData::load(ctx.findAsset("halves.gltf").value()).value();
    REQUIRE(data.primitives.size() == 2);
    auto model = uploadModel<glm::vec3>(ctx, data, [](const ModelPrimitive& p) -> Result<std::vector<glm::vec3>> {
        return p.positions;
    }).value();
    REQUIRE(model.draws.size() == 2);
    // Each draw is its primitive's range of the mesh, its first instance its material
    CHECK(model.mesh->indexType() == VK_INDEX_TYPE_UINT16);
    CHECK(model.mesh->indexCount() == data.primitives[0].indices.size() + data.primitives[1].indices.size());
    CHECK(model.draws[1].firstIndex == data.primitives[0].indices.size());
    CHECK(model.draws[1].vertexOffset == static_cast<int32_t>(data.primitives[0].positions.size()));
    for (size_t i = 0; i < 2; ++i) {
        CHECK(model.draws[i].instanceCount == 1);
        CHECK(model.draws[i].firstInstance == data.primitives[i].material);
    }
    // The quads on the left and right halves of clip space
    CHECK(model.bounds[0].x < 0.0f);
    CHECK(model.bounds[1].x > 0.0f);
    CHECK(model.renderables(ctx, drawMaterial(ctx, AttachmentFormats{{VK_FORMAT_R8G8B8A8_UNORM}}, true)).size() == 2);

    CHECK_ERROR(uploadModel<glm::vec3>(ctx, ModelData{}, nullptr), ErrorCode::InvalidData, "without triangles");
}
