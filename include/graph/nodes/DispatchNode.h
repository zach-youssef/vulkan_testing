#pragma once

#include "graph/RenderGraph.h"
#include "materials/Renderable.h"

/*
 Dispatches a compute material. The graph updates the material once per frame.

 Resources the material's descriptors reference must be declared separately (graph.node(id).read(...)
 and .write(...)).
 */
class DispatchNode : public ComputeNode {
public:
    explicit DispatchNode(Handle<ComputeMaterial> material) : material_(std::move(material)) {}

    void setup(NodeBuilder& builder) override {
        builder.uses(material_);
    }

    void record(RecordContext& context) override {
        material_->bind(context.commandBuffer, context.frameIndex);
        auto groups = material_->getDispatchDimensions();
        vkCmdDispatch(context.commandBuffer,
                      static_cast<uint32_t>(groups.x),
                      static_cast<uint32_t>(groups.y),
                      static_cast<uint32_t>(groups.z));
    }

private:
    Handle<ComputeMaterial> material_;
};
