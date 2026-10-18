#pragma once

#include "core/Contract.h"
#include "graph/RenderGraph.h"
#include "materials/Renderable.h"

#include <functional>
#include <optional>

/*
 Draws with a material from commands in a graph buffer, which shaders write (e.g. a culling pass),
 so the GPU decides what's drawn without the CPU reading anything back:

    - without a mesh, VkDrawIndirectCommands, for materials without vertex input (vertices pulled
      from buffers or computed from gl_VertexIndex)
    - with a mesh, VkDrawIndexedIndirectCommands drawing ranges of its vertex and index buffers

 It draws maxDraws commands (more than one needs multiDrawIndirect), or with a count, as many as the
 uint32_t at `count` says, up to maxDraws. Counts need drawIndirectCount, which the context enables
 when the device has it (check ctx.enabledFeatures().vulkan12.drawIndirectCount); without it, write
 instanceCount 0 into the commands not to draw instead. A command's firstInstance (gl_InstanceIndex's
 base) other than 0 needs drawIndirectFirstInstance.
 */
class IndirectDrawNode : public GraphicsNode {
public:
    struct Desc {
        Handle<Material> material;
        RenderTarget target;
        BufferOffset commands;               // The first command
        uint32_t maxDraws = 1;               // Commands, tightly packed
        std::optional<BufferOffset> count;   // A uint32_t: how many commands to draw
        Handle<Mesh> mesh;                   // Indexed draws from its buffers, or null
        // Called after the material is bound, before drawing, if given: e.g. to push the node's own
        // push constants over the material's
        std::function<void(const RecordContext&)> afterBind;
    };

    explicit IndirectDrawNode(Desc desc) : desc_(std::move(desc)) {
        if (!desc_.material) {
            contractViolation("IndirectDrawNode was given no material");
        }
    }

    void setup(NodeBuilder& builder) override {
        const DeviceFeatures& features = builder.graph().context().enabledFeatures();
        if (desc_.count && !features.vulkan12.drawIndirectCount) {
            contractViolation("IndirectDrawNode with a count needs drawIndirectCount, which this device lacks "
                              "(check ctx.enabledFeatures().vulkan12.drawIndirectCount)");
        }
        if (desc_.maxDraws > 1 && !features.core.features.multiDrawIndirect) {
            contractViolation("IndirectDrawNode drawing more than one command needs multiDrawIndirect "
                              "(request it in VulkanContextConfig::features)");
        }
        builder.renderTo(desc_.target).uses(desc_.material).read(desc_.commands.buffer, Access::IndirectRead);
        if (desc_.count) {
            builder.read(desc_.count->buffer, Access::IndirectRead);
        }
    }

    void record(RecordContext& context) override {
        VkCommandBuffer cmd = context.commandBuffer;
        desc_.material->bind(cmd, context.frameIndex);
        if (desc_.afterBind) {
            desc_.afterBind(context);
        }
        const VkBuffer commands = context.graph.buffer(desc_.commands.buffer, context.frameIndex);
        const VkBuffer count = desc_.count ? context.graph.buffer(desc_.count->buffer, context.frameIndex) : VK_NULL_HANDLE;
        if (desc_.mesh) {
            VkDeviceSize offsets[] {0};
            VkBuffer vertexBuffers[] {desc_.mesh->vertexBuffer()};
            vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
            vkCmdBindIndexBuffer(cmd, desc_.mesh->indexBuffer(), 0, desc_.mesh->indexType());
            constexpr uint32_t stride = sizeof(VkDrawIndexedIndirectCommand);
            if (desc_.count) {
                vkCmdDrawIndexedIndirectCount(cmd, commands, desc_.commands.offset, count, desc_.count->offset,
                                              desc_.maxDraws, stride);
            } else {
                vkCmdDrawIndexedIndirect(cmd, commands, desc_.commands.offset, desc_.maxDraws, stride);
            }
            return;
        }
        constexpr uint32_t stride = sizeof(VkDrawIndirectCommand);
        if (desc_.count) {
            vkCmdDrawIndirectCount(cmd, commands, desc_.commands.offset, count, desc_.count->offset, desc_.maxDraws, stride);
        } else {
            vkCmdDrawIndirect(cmd, commands, desc_.commands.offset, desc_.maxDraws, stride);
        }
    }

private:
    Desc desc_;
};
