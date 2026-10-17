#pragma once

#include "core/Contract.h"
#include "core/Result.h"
#include "graph/RenderGraph.h"
#include "resources/Buffer.h"

#include <cstring>
#include <optional>
#include <vector>

/*
 Copies a buffer into host-visible memory every frame, e.g. to save results or check them in tests.
 contents<T>() returns the copy made by the most recent frame, waiting for it to finish. The buffer
 is copied whole, whatever its size that frame.
 */
class BufferReadbackNode : public TransferNode {
public:
    explicit BufferReadbackNode(BufferId buffer) : buffer_(buffer) {}

    void setup(NodeBuilder& builder) override {
        graph_ = &builder.graph();
        builder.read(buffer_, Access::TransferSrc);
        copies_ = PerFrame<Handle<Buffer<uint8_t>>>(graph_->context(), [](uint32_t) {
            return Handle<Buffer<uint8_t>>();
        });
    }

    // (Re)creates the frame's copy when the buffer's size changed
    Result<void> prepareFrame(FrameContext& context) override {
        const VkDeviceSize size = graph_->bufferSize(buffer_, context.frameIndex);
        auto& copy = copies_[context.frameIndex];
        if (!copy || copy->getSize() != size) {
            // The frame slot's previous copy has finished (the slot was waited for)
            TRY_ASSIGN(copy, Buffer<uint8_t>::create(context.context, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
        }
        return {};
    }

    void record(RecordContext& context) override {
        auto& copy = copies_[context.frameIndex];
        const VkBufferCopy region{0, 0, copy->getSize()};
        vkCmdCopyBuffer(context.commandBuffer, graph_->buffer(buffer_, context.frameIndex), copy->getBuffer(), 1, &region);
        lastFrame_ = context.frameIndex;
    }

    // The buffer's bytes copied by the most recent frame. Waits for the graph's submitted work.
    // Calling it before the node has run is a contract violation.
    Result<std::vector<uint8_t>> bytes() {
        return contents<uint8_t>();
    }

    // The buffer's elements of type T copied by the most recent frame (as many as fit)
    template<typename T>
    Result<std::vector<T>> contents() {
        if (!lastFrame_) {
            contractViolation("BufferReadbackNode hasn't run yet");
        }
        TRY(graph_->waitIdle());
        auto& copy = copies_[*lastFrame_];
        std::vector<T> elements(copy->getSize() / sizeof(T));
        TRY(copy->mapAndExecute(0, copy->getSize(), [&elements](void* data) {
            std::memcpy(elements.data(), data, elements.size() * sizeof(T));
        }));
        return elements;
    }

private:
    BufferId buffer_;
    RenderGraph* graph_ = nullptr;
    PerFrame<Handle<Buffer<uint8_t>>> copies_;
    std::optional<uint32_t> lastFrame_;
};
