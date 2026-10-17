#pragma once

#include "graph/RenderGraph.h"
#include "materials/Renderable.h"

/*
 Draws renderables into a render target. Each renderable's material is updated once per frame by
 the graph, however many renderables (or nodes) share it, and the resources of its bindings are
 declared automatically.
 */
class RenderablesNode : public GraphicsNode {
public:
    RenderablesNode(std::vector<Handle<Renderable>> renderables, RenderTarget target)
    : renderables_(std::move(renderables)), target_(std::move(target)) {}

    void setup(NodeBuilder& builder) override {
        builder.renderTo(target_);
        for (auto& renderable : renderables_) {
            builder.uses(renderable->getMaterialHandle());
        }
    }

    void record(RecordContext& context) override {
        for (auto& renderable : renderables_) {
            renderable->getMaterial()->bind(context.commandBuffer, context.frameIndex);
            VkDeviceSize offsets[] {0};
            VkBuffer vertexBuffers[] {renderable->getVertexBuffer()};
            vkCmdBindVertexBuffers(context.commandBuffer, 0, 1, vertexBuffers, offsets);
            vkCmdBindIndexBuffer(context.commandBuffer, renderable->getIndexBuffer(), 0, renderable->getIndexType());
            vkCmdDrawIndexed(context.commandBuffer, renderable->getIndexCount(), 1, 0, 0, 0);
        }
    }

private:
    std::vector<Handle<Renderable>> renderables_;
    RenderTarget target_;
};
