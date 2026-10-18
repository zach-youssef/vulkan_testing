#pragma once

#include "graph/RenderGraph.h"
#include "materials/Renderable.h"

#include <functional>

/*
 Draws renderables into a render target. Each renderable's material is updated once per frame by
 the graph, however many renderables (or nodes) share it, and the resources of its bindings are
 declared automatically. afterBind, if given, is called after each renderable's material is bound,
 before it's drawn: e.g. to push the node's own push constants over the material's.
 */
class RenderablesNode : public GraphicsNode {
public:
    using AfterBind = std::function<void(const RecordContext&, Material&)>;

    RenderablesNode(std::vector<Handle<Renderable>> renderables, RenderTarget target, AfterBind afterBind = nullptr)
    : renderables_(std::move(renderables)), target_(std::move(target)), afterBind_(std::move(afterBind)) {}

    void setup(NodeBuilder& builder) override {
        builder.renderTo(target_);
        for (auto& renderable : renderables_) {
            builder.uses(renderable->getMaterialHandle());
        }
    }

    void record(RecordContext& context) override {
        for (auto& renderable : renderables_) {
            renderable->getMaterial()->bind(context.commandBuffer, context.frameIndex);
            if (afterBind_) {
                afterBind_(context, *renderable->getMaterial());
            }
            VkDeviceSize offsets[] {0};
            VkBuffer vertexBuffers[] {renderable->getVertexBuffer()};
            vkCmdBindVertexBuffers(context.commandBuffer, 0, 1, vertexBuffers, offsets);
            vkCmdBindIndexBuffer(context.commandBuffer, renderable->getIndexBuffer(), 0, renderable->getIndexType());
            vkCmdDrawIndexed(context.commandBuffer, renderable->getIndexCount(), 1, renderable->getFirstIndex(),
                             renderable->getVertexOffset(), renderable->getFirstInstance());
        }
    }

private:
    std::vector<Handle<Renderable>> renderables_;
    RenderTarget target_;
    AfterBind afterBind_;
};
