#pragma once

#include "graph/RenderGraph.h"
#include "materials/Material.h"

#include <optional>

/*
 Dispatches a compute material: as many workgroups as its getDispatchDimensions(), or, indirectly,
 as many as a VkDispatchIndirectCommand in a graph buffer says (e.g. one a previous node's shader
 wrote), so the GPU decides without the CPU reading anything back.
 */
class DispatchNode : public ComputeNode {
public:
    explicit DispatchNode(Handle<ComputeMaterial> material) : material_(std::move(material)) {}

    // Dispatches the workgroups of the command at `command`
    DispatchNode(Handle<ComputeMaterial> material, BufferOffset command)
    : material_(std::move(material)), command_(command) {}

    void setup(NodeBuilder& builder) override {
        builder.uses(material_);
        if (command_) {
            builder.read(command_->buffer, Access::IndirectRead);
        }
    }

    void record(RecordContext& context) override {
        material_->bind(context.commandBuffer, context.frameIndex);
        if (command_) {
            vkCmdDispatchIndirect(context.commandBuffer, context.graph.buffer(command_->buffer, context.frameIndex),
                                  command_->offset);
            return;
        }
        auto groups = material_->getDispatchDimensions();
        vkCmdDispatch(context.commandBuffer,
                      static_cast<uint32_t>(groups.x),
                      static_cast<uint32_t>(groups.y),
                      static_cast<uint32_t>(groups.z));
    }

private:
    Handle<ComputeMaterial> material_;
    std::optional<BufferOffset> command_;
};
