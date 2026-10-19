#pragma once

#include "core/Contract.h"
#include "core/Result.h"
#include "graph/RenderGraph.h"
#include "graph/nodes/FinishedCopies.h"
#include "resources/Buffer.h"

#include <cstring>
#include <optional>
#include <vector>

/*
 Copies a buffer into host-visible memory every frame, e.g. to save results or check them in tests.
 contents<T>() returns the copy made by the most recent frame, waiting for it to finish. The buffer
 is copied whole, whatever its size that frame (which may be 0, giving no elements).

 takeFinished<T>() doesn't wait: it returns the newest copy that has finished since the last one it
 returned, if any, for code polling between frames (e.g. an app reacting to results as they arrive
 while frames keep going).
 */
class BufferReadbackNode : public TransferNode {
public:
    explicit BufferReadbackNode(BufferId buffer) : buffer_(buffer) {}

    void setup(NodeBuilder& builder) override {
        graph_ = &builder.graph();
        builder.read(buffer_, Access::TransferSrc);
        copies_ = PerFrame<Copy>(graph_->context(), [](uint32_t) { return Copy{}; });
    }

    // (Re)creates the frame's copy when the buffer's size changed
    Result<void> prepareFrame(FrameContext& context) override {
        const VkDeviceSize size = graph_->bufferSize(buffer_, context.frameIndex);
        auto& copy = copies_[context.frameIndex];
        copy.size = size;
        if (size != 0 && (!copy.buffer || copy.buffer->getSize() != size)) {
            // The frame slot's previous copy has finished (the slot was waited for)
            TRY_ASSIGN(copy.buffer, Buffer<uint8_t>::create(context.context, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
        }
        return {};
    }

    void record(RecordContext& context) override {
        auto& copy = copies_[context.frameIndex];
        lastFrame_ = context.frameIndex;
        // This frame slot's previous copy is overwritten
        finished_.recorded(context.frameIndex);
        if (copy.size == 0) {
            return;
        }
        const VkBufferCopy region{0, 0, copy.size};
        vkCmdCopyBuffer(context.commandBuffer, graph_->buffer(buffer_, context.frameIndex), copy.buffer->getBuffer(), 1, &region);
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
        return read<T>(copies_[*lastFrame_]);
    }

    // The newest copy that has finished and that this hasn't returned, without waiting; empty if
    // none has. Call it between frames.
    template<typename T>
    Result<std::optional<std::vector<T>>> takeFinished() {
        const std::optional<uint32_t> newest = finished_.takeNewest(graph_->context(), copies_.size());
        if (!newest) {
            return std::optional<std::vector<T>>();
        }
        TRY_ASSIGN(auto elements, read<T>(copies_[*newest]));
        return std::optional<std::vector<T>>(std::move(elements));
    }

private:
    // A frame's copy: the buffer's size that frame, and host memory holding it (none for size 0)
    struct Copy {
        VkDeviceSize size = 0;
        Handle<Buffer<uint8_t>> buffer;
    };

    template<typename T>
    static Result<std::vector<T>> read(Copy& copy) {
        std::vector<T> elements(copy.size / sizeof(T));
        if (elements.empty()) {
            return elements;
        }
        TRY(copy.buffer->mapAndExecute(0, copy.size, [&elements](void* data) {
            std::memcpy(elements.data(), data, elements.size() * sizeof(T));
        }));
        return elements;
    }

    BufferId buffer_;
    RenderGraph* graph_ = nullptr;
    PerFrame<Copy> copies_;
    FinishedCopies finished_;
    std::optional<uint32_t> lastFrame_;
};
