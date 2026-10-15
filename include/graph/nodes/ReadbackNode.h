#pragma once

#include "graph/RenderGraph.h"
#include "resources/Buffer.h"
#include "resources/Image.h"

/*
 Copies an image into host-visible memory every frame, e.g. for screenshots and tests.
 pixels() returns the copy made by the most recent frame, waiting for it to finish.

 The image must be in a format Image::bytesPerPixel supports.
 */
class ReadbackNode : public TransferNode {
public:
    explicit ReadbackNode(ImageId image) : image_(image) {}

    void setup(NodeBuilder& builder) override {
        graph_ = &builder.graph();
        builder.read(image_, Access::TransferSrc);
        buffers_ = PerFrame<Handle<Buffer<uint8_t>>>(graph_->context(), [](uint32_t) {
            return Handle<Buffer<uint8_t>>();
        });
    }

    void record(RecordContext& context) override {
        const VkExtent2D extent = graph_->imageExtent(image_, context.frameIndex);
        const VkFormat format = graph_->imageFormat(image_);
        const VkDeviceSize size = Image::bytesPerPixel(format) * extent.width * extent.height;

        auto& buffer = buffers_[context.frameIndex];
        if (!buffer || buffer->getSize() != size) {
            // The frame slot's previous copy has finished (the slot was waited for)
            VK_SUCCESS_OR_THROW(Buffer<uint8_t>::create(buffer, context.context, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
                                "Failed to create readback buffer");
        }

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(context.commandBuffer, graph_->image(image_, context.frameIndex)->getImage(),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer->getBuffer(), 1, &region);
        lastFrame_ = context.frameIndex;
        lastSize_ = size;
    }

    // The pixels copied by the most recent frame. Waits for the graph's submitted work.
    std::vector<uint8_t> pixels() {
        if (!lastFrame_) {
            throw std::logic_error("ReadbackNode hasn't run yet");
        }
        graph_->waitIdle();
        std::vector<uint8_t> pixels(lastSize_);
        buffers_[*lastFrame_]->mapAndExecute(0, lastSize_, [&pixels](void* data) {
            memcpy(pixels.data(), data, pixels.size());
        });
        return pixels;
    }

private:
    ImageId image_;
    RenderGraph* graph_ = nullptr;
    PerFrame<Handle<Buffer<uint8_t>>> buffers_;
    std::optional<uint32_t> lastFrame_;
    VkDeviceSize lastSize_ = 0;
};
