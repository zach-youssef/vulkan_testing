#pragma once

#include "core/Contract.h"
#include "core/Result.h"
#include "graph/RenderGraph.h"
#include "graph/nodes/FinishedCopies.h"
#include "resources/Buffer.h"
#include "resources/Image.h"

#include <cstring>
#include <optional>
#include <string>
#include <vector>

/*
 Copies an image into host-visible memory every frame, e.g. for screenshots and tests.
 pixels() returns the copy made by the most recent frame, waiting for it to finish.

 takeFinished<T>() doesn't wait: it returns the newest copy that has finished since the last one it
 returned, if any, as texels of type T (one per texel, holding its bits, e.g. uint32_t for RGBA8),
 with its extent, for code polling between frames.

 The image must be in a format Image::bytesPerPixel supports.
 */
class ReadbackNode : public TransferNode {
public:
    explicit ReadbackNode(ImageId image) : image_(image) {}

    // A finished copy's texels and the image's extent then
    template<typename T>
    struct Texels {
        std::vector<T> texels;
        VkExtent2D extent;
    };

    void setup(NodeBuilder& builder) override {
        graph_ = &builder.graph();
        builder.read(image_, Access::TransferSrc);
        copies_ = PerFrame<Copy>(graph_->context(), [](uint32_t) { return Copy{}; });
    }

    // (Re)creates the frame's buffer when the image's size changed
    Result<void> prepareFrame(FrameContext& context) override {
        const VkDeviceSize size = frameSize(context.frameIndex);
        auto& buffer = copies_[context.frameIndex].buffer;
        if (!buffer || buffer->getSize() != size) {
            // The frame slot's previous copy has finished (the slot was waited for)
            TRY_ASSIGN(buffer, Buffer<uint8_t>::create(context.context, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
        }
        return {};
    }

    void record(RecordContext& context) override {
        const VkExtent2D extent = graph_->imageExtent(image_, context.frameIndex);
        auto& copy = copies_[context.frameIndex];
        auto& buffer = copy.buffer;

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(context.commandBuffer, graph_->image(image_, context.frameIndex)->getImage(),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer->getBuffer(), 1, &region);
        lastFrame_ = context.frameIndex;
        lastSize_ = frameSize(context.frameIndex);
        lastExtent_ = extent;
        copy.size = lastSize_;
        copy.extent = extent;
        finished_.recorded(context.frameIndex);
    }

    // The pixels copied by the most recent frame. Waits for the graph's submitted work. Calling it
    // before the node has run is a contract violation.
    Result<std::vector<uint8_t>> pixels() {
        if (!lastFrame_) {
            contractViolation("ReadbackNode hasn't run yet");
        }
        TRY(graph_->waitIdle());
        std::vector<uint8_t> pixels(lastSize_);
        TRY(copies_[*lastFrame_].buffer->mapAndExecute(0, lastSize_, [&pixels](void* data) {
            memcpy(pixels.data(), data, pixels.size());
        }));
        return pixels;
    }

    // The size of the most recent frame's copy (which pixels() returns), and the image's format,
    // e.g. to encode the pixels. Calling extent() before the node has run is a contract violation.
    VkExtent2D extent() const {
        if (!lastFrame_) {
            contractViolation("ReadbackNode hasn't run yet");
        }
        return lastExtent_;
    }
    VkFormat format() const { return graph_->imageFormat(image_); }

    // The newest copy that has finished and that this hasn't returned, without waiting; empty if
    // none has. Call it between frames. T holds a texel: sizeof(T) is the format's bytes per pixel.
    template<typename T>
    Result<std::optional<Texels<T>>> takeFinished() {
        if (sizeof(T) != Image::bytesPerPixel(format())) {
            contractViolation("ReadbackNode::takeFinished's texel type has " + std::to_string(sizeof(T)) + " bytes, but the image's "
                              "format has " + std::to_string(Image::bytesPerPixel(format())) + " per pixel");
        }
        const std::optional<uint32_t> newest = finished_.takeNewest(graph_->context(), copies_.size());
        if (!newest) {
            return std::optional<Texels<T>>();
        }
        const Copy& copy = copies_[*newest];
        std::vector<T> texels(copy.size / sizeof(T));
        if (!texels.empty()) {
            TRY(copy.buffer->mapAndExecute(0, copy.size, [&texels](void* data) {
                std::memcpy(texels.data(), data, texels.size() * sizeof(T));
            }));
        }
        return std::optional<Texels<T>>(Texels<T>{std::move(texels), copy.extent});
    }

private:
    // A frame slot's copy: host memory, and what the slot's last copy holds
    struct Copy {
        Handle<Buffer<uint8_t>> buffer;
        VkDeviceSize size = 0;
        VkExtent2D extent{};
    };

    VkDeviceSize frameSize(uint32_t frameIndex) {
        const VkExtent2D extent = graph_->imageExtent(image_, frameIndex);
        return Image::bytesPerPixel(graph_->imageFormat(image_)) * extent.width * extent.height;
    }

    ImageId image_;
    RenderGraph* graph_ = nullptr;
    PerFrame<Copy> copies_;
    FinishedCopies finished_;
    std::optional<uint32_t> lastFrame_;
    VkDeviceSize lastSize_ = 0;
    VkExtent2D lastExtent_{};
};
