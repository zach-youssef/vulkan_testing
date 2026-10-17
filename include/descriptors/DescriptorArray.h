#pragma once

#include "core/Contract.h"
#include "core/FrameResources.h"
#include "core/Result.h"
#include "core/VulkanContext.h"
#include "descriptors/DescriptorHeap.h"
#include "resources/Buffer.h"

#include <cstring>
#include <vector>

/*
 An array of descriptor entries a material binds as one binding (MaterialBinding::array), e.g. a
 set of textures shaders choose between. Push constants can't hold many indices, so the array keeps
 them in a small storage buffer per frame in flight, holding the element count and then each
 element's index:

     struct { uint count; uint indices[]; }

 The binding pushes that buffer's index, and shaders read the count and indices from it, indexing
 the universal arrays with nonuniformEXT (different invocations may choose different elements).

 Elements are FrameBindings, so they can be per frame (e.g. a render graph's entries, which follow
 recreated resources): before a material binds the array for a frame, update() writes the frame's
 indices into the frame's buffer, only when they changed. The number of elements is fixed.

 Create it with create(); it's managed (a Handle), and the buffers follow the number of frames in
 flight.
 */
class DescriptorArray {
public:
    using Mapping = Buffer<uint32_t>::Mapping;

    DescriptorArray(std::vector<FrameBinding<DescriptorEntry>> elements, Handle<PerFrameHandles<Buffer<uint32_t>>> buffers,
                    Handle<PerFrameHandles<DescriptorEntry>> entries, PerFrame<Mapping> mappings)
    : elements_(std::move(elements)), buffers_(std::move(buffers)), entries_(std::move(entries)),
      mappings_(std::move(mappings)) {}

    static Result<Handle<DescriptorArray>> create(VulkanContext& context, std::vector<FrameBinding<DescriptorEntry>> elements) {
        const size_t words = 1 + elements.size();
        // Written by the CPU when the indices change, read by shaders
        const VkMemoryPropertyFlags memory = mappedMemoryFor(context);
        // Created in this order, so resized in this order: buffers, then their entries and mappings
        TRY_ASSIGN(auto buffers, PerFrameHandles<Buffer<uint32_t>>::createManaged(context, [&context, words, memory](uint32_t) {
            return Buffer<uint32_t>::create(context, words, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory);
        }));
        auto entries = context.resources().create<PerFrameHandles<DescriptorEntry>>(
            context, [ctx = &context, buffers](uint32_t frame) { return ctx->descriptors().storageBuffer((*buffers)[frame]); });
        TRY_ASSIGN(auto mappings, PerFrame<Mapping>::create(context, [buffers, words](uint32_t frame) {
            return (*buffers)[frame]->getPersistentMapping(0, words * sizeof(uint32_t));
        }));
        return context.resources().create<DescriptorArray>(std::move(elements), std::move(buffers), std::move(entries),
                                                           std::move(mappings));
    }

    // The entries of the buffers holding the indices, one per frame: what the binding pushes
    FrameBinding<DescriptorEntry> entries() const {
        return entries_;
    }

    const std::vector<FrameBinding<DescriptorEntry>>& elements() const {
        return elements_;
    }

    size_t size() const {
        return elements_.size();
    }

    // Writes the frame's element indices into the frame's buffer, unless it holds them. Every
    // element must have an entry for the frame. Called when a material binding the array binds.
    void update(uint32_t frameIndex) {
        std::vector<uint32_t> words(1 + elements_.size());
        words[0] = static_cast<uint32_t>(elements_.size());
        for (size_t i = 0; i < elements_.size(); ++i) {
            auto entry = elements_[i].get(frameIndex);
            if (!entry) {
                contractViolation("Descriptor array element " + std::to_string(i) + " has no entry for frame "
                                  + std::to_string(frameIndex));
            }
            words[1 + i] = entry->index();
        }
        if (written_.size() <= frameIndex) {
            written_.resize(frameIndex + 1);
        }
        // A buffer recreated when the number of frames in flight changed holds nothing yet
        Written& written = written_[frameIndex];
        const Buffer<uint32_t>* buffer = (*buffers_)[frameIndex].get();
        if (written.buffer == buffer && written.words == words) {
            return;
        }
        std::memcpy(mappings_[frameIndex].get(), words.data(), words.size() * sizeof(uint32_t));
        written.buffer = buffer;
        written.words = std::move(words);
        ++writes_;
    }

    // How many times a buffer has been written: for tests
    uint64_t writes() const {
        return writes_;
    }

private:
    // What a frame's buffer holds
    struct Written {
        const Buffer<uint32_t>* buffer = nullptr;
        std::vector<uint32_t> words;
    };

    std::vector<FrameBinding<DescriptorEntry>> elements_;
    Handle<PerFrameHandles<Buffer<uint32_t>>> buffers_;
    Handle<PerFrameHandles<DescriptorEntry>> entries_;
    PerFrame<Mapping> mappings_;
    std::vector<Written> written_;
    uint64_t writes_ = 0;
};
