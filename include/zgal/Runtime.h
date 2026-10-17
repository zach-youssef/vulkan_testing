#pragma once

#include "core/Contract.h"
#include "core/FrameResources.h"
#include "core/Result.h"
#include "core/VulkanContext.h"
#include "descriptors/DescriptorHeap.h"
#include "resources/Buffer.h"
#include "resources/Image.h"

#include <glm/glm.hpp>

#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

/*
 What the C++ that ZGAL generates for graphs (dsl/, `make docs`) uses at runtime. Applications use
 FrameInfo and FrameSource to supply a graph's per-frame inputs; the rest is for generated code.
 */
namespace zgal {

// The frame a graph's per-frame inputs are asked for
struct FrameInfo {
    uint32_t frameIndex;        // context.currentFrame(): indexes PerFrame collections
    uint64_t frameNumber;       // context.frameNumber(): one more each frame
    VkExtent2D surfaceExtent;   // The graph's surfaceExtent(): {0, 0} without a surface or one set headless
};

// A value C++ computes for each frame. Called once per frame (for the graph's uniforms, by
// whichever material using them is updated first; for push constants, by each material using them)
template<typename T>
using FrameSource = std::function<T(const FrameInfo&)>;

// Memory the CPU writes and shaders read: device local where the device can also map it (all
// memory on unified memory architectures such as Apple's, and resizable BAR on discrete GPUs), so
// shaders read video memory rather than across the bus; otherwise host memory
inline VkMemoryPropertyFlags mappedMemoryFor(VulkanContext& context) {
    constexpr VkMemoryPropertyFlags mapped = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(context.physicalDevice(), &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & (mapped | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) == (mapped | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            return mapped | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        }
    }
    return mapped;
}

/*
 A uniform: a mapped storage buffer per frame in flight holding a T, written from its source once
 per frame, just before the first material using it records. A buffer per frame means the CPU never
 writes one the GPU is reading. The collections follow the number of frames in flight.

 To spare memory traffic, the buffers are device local where they can be (mappedMemoryFor), and a
 frame's buffer is only written when the value differs from what it already holds (e.g. a camera
 that isn't moving).

 Shared by the materials using it, and destroyed with the last (so before the context).
 */
template<typename T>
class Uniform {
public:
    static_assert(std::is_trivially_copyable_v<T>, "uniforms are copied into buffers byte for byte");
    using Mapping = typename Buffer<T>::Mapping;

    static Result<std::shared_ptr<Uniform>> create(VulkanContext& context, FrameSource<T> source) {
        const VkMemoryPropertyFlags memory = mappedMemoryFor(context);
        // Created in this order, so resized in this order: buffers, then their entries and mappings
        TRY_ASSIGN(auto buffers, PerFrameHandles<Buffer<T>>::createManaged(context, [&context, memory](uint32_t) {
            return Buffer<T>::create(context, 1, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory);
        }));
        auto entries = context.resources().create<PerFrameHandles<DescriptorEntry>>(
            context, [ctx = &context, buffers](uint32_t frame) {
                return ctx->descriptors().storageBuffer((*buffers)[frame]);
            });
        TRY_ASSIGN(auto mappings, PerFrame<Mapping>::create(context, [buffers](uint32_t frame) {
            return (*buffers)[frame]->getPersistentMapping(0, sizeof(T));
        }));
        return std::shared_ptr<Uniform>(new Uniform(std::move(source), std::move(buffers), std::move(entries),
                                                    std::move(mappings)));
    }

    // The buffers' descriptor entries, one per frame
    FrameBinding<DescriptorEntry> entries() const {
        return entries_;
    }

    // Computes the frame's value, unless it has been this frame, and writes it to the frame's
    // buffer unless the buffer already holds it
    void update(const FrameInfo& frame) {
        if (updated_ && lastFrame_ == frame.frameNumber) {
            return;
        }
        updated_ = true;
        lastFrame_ = frame.frameNumber;
        const T value = source_(frame);

        if (contents_.size() <= frame.frameIndex) {
            contents_.resize(frame.frameIndex + 1);
        }
        // A buffer recreated when the number of frames in flight changed holds nothing yet
        Contents& contents = contents_[frame.frameIndex];
        const Buffer<T>* buffer = (*buffers_)[frame.frameIndex].get();
        if (contents.buffer == buffer && std::memcmp(contents.bytes, &value, sizeof(T)) == 0) {
            return;
        }
        std::memcpy(mappings_[frame.frameIndex].get(), &value, sizeof(T));
        contents.buffer = buffer;
        std::memcpy(contents.bytes, &value, sizeof(T));
        ++writes_;
    }

    // How many times a buffer has been written: for tests
    uint64_t writes() const {
        return writes_;
    }

private:
    Uniform(FrameSource<T> source, Handle<PerFrameHandles<Buffer<T>>> buffers,
            Handle<PerFrameHandles<DescriptorEntry>> entries, PerFrame<Mapping> mappings)
    : source_(std::move(source)), buffers_(std::move(buffers)), entries_(std::move(entries)),
      mappings_(std::move(mappings)) {}

    // What a frame's buffer holds
    struct Contents {
        const Buffer<T>* buffer = nullptr;
        unsigned char bytes[sizeof(T)];
    };

    FrameSource<T> source_;
    Handle<PerFrameHandles<Buffer<T>>> buffers_;
    Handle<PerFrameHandles<DescriptorEntry>> entries_;
    PerFrame<Mapping> mappings_;
    std::vector<Contents> contents_;
    bool updated_ = false;
    uint64_t lastFrame_ = 0;
    uint64_t writes_ = 0;
};

// What a graph's material does each frame before recording: e.g. writing its uniforms
using Updates = std::vector<std::function<void(const FrameInfo&)>>;

// Fills a material's push constants for a frame
template<typename Push>
using PushSource = std::function<void(const FrameInfo&, Push&)>;

// A compute material's workgroups for a frame (empty for graphics materials)
using DispatchSource = std::function<glm::vec3(const FrameInfo&)>;

/*
 A material generated from ZGAL, as a graph uses it: Material is the generated class (e.g. Lit),
 whose constructor arguments follow these. Each frame, update() runs the updates and computes the
 frame's workgroups (for compute materials) and push constants (PushInstance), which every bind
 that frame then uses.

 getDispatchDimensions and update are declared without `override` so these serve graphics and
 compute materials alike: for compute materials they override ComputeMaterial's, and for graphics
 materials getDispatchDimensions is an unused function. (Marking only some of them `override`
 would be inconsistent, which compilers warn about.)
 */
template<typename Material>
class Instance final : public Material {
public:
    template<typename... Args>
    Instance(Updates updates, DispatchSource dispatch, Args&&... args)
    : Material(std::forward<Args>(args)...), updates_(std::move(updates)), dispatchSource_(std::move(dispatch)) {}

    void update(uint32_t frameIndex, VkExtent2D surfaceExtent) {
        const FrameInfo frame{frameIndex, this->context_->frameNumber(), surfaceExtent};
        for (auto& update : updates_) {
            update(frame);
        }
        if (dispatchSource_) {
            dispatch_ = dispatchSource_(frame);
        }
    }

    glm::vec3 getDispatchDimensions() {
        return dispatch_;
    }

private:
    Updates updates_;
    DispatchSource dispatchSource_;
    glm::vec3 dispatch_{0.0f};
};

template<typename Material>
class PushInstance final : public Material {
public:
    using Push = typename Material::Push;

    template<typename... Args>
    PushInstance(Updates updates, PushSource<Push> push, DispatchSource dispatch, Args&&... args)
    : Material(std::forward<Args>(args)...), updates_(std::move(updates)), push_(std::move(push)),
      dispatchSource_(std::move(dispatch)) {}

    void update(uint32_t frameIndex, VkExtent2D surfaceExtent) {
        const FrameInfo frame{frameIndex, this->context_->frameNumber(), surfaceExtent};
        for (auto& update : updates_) {
            update(frame);
        }
        if (dispatchSource_) {
            dispatch_ = dispatchSource_(frame);
        }
        values_ = Push{};
        push_(frame, values_);
    }

    glm::vec3 getDispatchDimensions() {
        return dispatch_;
    }

protected:
    void writePush(uint32_t, Push& push) {
        push = values_;
    }

private:
    Updates updates_;
    PushSource<Push> push_;
    DispatchSource dispatchSource_;
    glm::vec3 dispatch_{0.0f};
    Push values_{};
};

// A graph's declaration that exists only under a condition, used where the condition was false
template<typename T>
const T& require(const std::optional<T>& value, const char* graph, const char* name) {
    if (!value) {
        contractViolation(std::string("Graph ") + graph + " uses " + name + ", whose #:when condition was false");
    }
    return *value;
}

// The extent of an imported image (the current frame's, if it has one per frame)
inline VkExtent2D extentOf(VulkanContext& context, const FrameBinding<Image>& image, const char* name) {
    auto handle = image.get(context.currentFrame());
    if (!handle) {
        contractViolation(std::string("Imported image ") + name + " has no image");
    }
    return {handle->getWidth(), handle->getHeight()};
}

} // namespace zgal
