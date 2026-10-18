#pragma once

#include "core/Contract.h"
#include "core/FrameResources.h"
#include "core/Result.h"
#include "core/VulkanContext.h"
#include "descriptors/DescriptorHeap.h"
#include "resources/Buffer.h"

#include <algorithm>
#include <cstring>
#include <memory>
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
 indices into the frame's buffer, only when they changed. setElements() replaces the elements, any
 number of them: each frame's buffer grows when that frame next updates it (its previous work has
 finished), and render graphs using the array declare the new elements' uses before their next
 frame (version() tells them it changed).

 Create it with create(); it's managed (a Handle), and the buffers follow the number of frames in
 flight.
 */
class DescriptorArray {
public:
    using Mapping = Buffer<uint32_t>::Mapping;

    DescriptorArray(VulkanContext& context, std::vector<FrameBinding<DescriptorEntry>> elements,
                    std::shared_ptr<size_t> capacity, Handle<PerFrameHandles<Buffer<uint32_t>>> buffers,
                    Handle<PerFrameHandles<DescriptorEntry>> entries, PerFrame<Mapping> mappings)
    : context_(&context), elements_(std::move(elements)), capacity_(std::move(capacity)), buffers_(std::move(buffers)),
      entries_(std::move(entries)), mappings_(std::move(mappings)) {}

    static Result<Handle<DescriptorArray>> create(VulkanContext& context, std::vector<FrameBinding<DescriptorEntry>> elements) {
        // The words each buffer holds: frames added later (more frames in flight) get as many
        auto capacity = std::make_shared<size_t>(1 + elements.size());
        // Written by the CPU when the indices change, read by shaders
        const VkMemoryPropertyFlags memory = mappedMemoryFor(context);
        // Created in this order, so resized in this order: buffers, then their entries and mappings
        TRY_ASSIGN(auto buffers, PerFrameHandles<Buffer<uint32_t>>::createManaged(context, [&context, capacity, memory](uint32_t) {
            return Buffer<uint32_t>::create(context, *capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory);
        }));
        auto entries = context.resources().create<PerFrameHandles<DescriptorEntry>>(
            context, [ctx = &context, buffers](uint32_t frame) { return ctx->descriptors().storageBuffer((*buffers)[frame]); });
        TRY_ASSIGN(auto mappings, PerFrame<Mapping>::create(context, [buffers](uint32_t frame) {
            return (*buffers)[frame]->getPersistentMapping(0, (*buffers)[frame]->getSize());
        }));
        return context.resources().create<DescriptorArray>(context, std::move(elements), std::move(capacity), std::move(buffers),
                                                           std::move(entries), std::move(mappings));
    }

    // Replaces the elements (any number of them), from the next frame each binds the array
    void setElements(std::vector<FrameBinding<DescriptorEntry>> elements) {
        elements_ = std::move(elements);
        *capacity_ = std::max(*capacity_, 1 + elements_.size());
        ++version_;
    }

    // Makes the frame's buffer hold the elements' indices, replacing it by a larger one (and its
    // entry) if it can't. Call it when the frame slot's previous work has finished (render graphs
    // do, before submitting a frame); update() does otherwise, as a contract violation on failure.
    Result<void> reserve(uint32_t frameIndex) {
        if (holdsElements(frameIndex)) {
            return {};
        }
        TRY_ASSIGN(auto buffer, Buffer<uint32_t>::create(*context_, *capacity_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                         mappedMemoryFor(*context_)));
        TRY_ASSIGN(auto mapping, buffer->getPersistentMapping(0, buffer->getSize()));
        // The frame's previous buffer is released once nothing holds it
        mappings_[frameIndex] = std::move(mapping);
        (*entries_)[frameIndex] = context_->descriptors().storageBuffer(buffer);
        (*buffers_)[frameIndex] = std::move(buffer);
        return {};
    }

    // Changes with every setElements, e.g. for a render graph to know its uses changed
    uint64_t version() const {
        return version_;
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
    // element must have an entry for the frame. Called when a material binding the array binds,
    // when the frame slot's previous work has finished, so its buffer can be replaced by a larger
    // one (and its entry with it) when there are more elements than it holds. Fails (a contract
    // violation, as binding can't fail) only when the device can't create that buffer.
    void update(uint32_t frameIndex) {
        if (!holdsElements(frameIndex)) {
            if (auto grown = reserve(frameIndex); !grown) {
                contractViolation("Descriptor array couldn't grow its buffer of indices: " + grown.error().describe());
            }
        }
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
    bool holdsElements(uint32_t frameIndex) {
        return (*buffers_)[frameIndex]->getSize() >= (1 + elements_.size()) * sizeof(uint32_t);
    }

    // What a frame's buffer holds
    struct Written {
        const Buffer<uint32_t>* buffer = nullptr;
        std::vector<uint32_t> words;
    };

    VulkanContext* context_;
    std::vector<FrameBinding<DescriptorEntry>> elements_;
    std::shared_ptr<size_t> capacity_;
    uint64_t version_ = 0;
    Handle<PerFrameHandles<Buffer<uint32_t>>> buffers_;
    Handle<PerFrameHandles<DescriptorEntry>> entries_;
    PerFrame<Mapping> mappings_;
    std::vector<Written> written_;
    uint64_t writes_ = 0;
};
