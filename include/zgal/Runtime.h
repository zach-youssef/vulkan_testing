#pragma once

#include "core/Contract.h"
#include "core/FrameResources.h"
#include "core/Result.h"
#include "core/VulkanContext.h"
#include "descriptors/DescriptorHeap.h"
#include "graph/RenderGraph.h"
#include "graph/nodes/DispatchNode.h"
#include "graph/nodes/IndirectDrawNode.h"
#include "resources/Buffer.h"
#include "resources/Image.h"

#include <glm/glm.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
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

// The FrameInfo of a node's or graph's FrameContext
inline FrameInfo frameInfo(const FrameContext& context) {
    return {context.frameIndex, context.frameNumber, context.surfaceExtent};
}

// A value C++ computes for each frame. Called once per frame (for the graph's uniforms, by
// whichever material using them is updated first; for push constants, by each material using them)
template<typename T>
using FrameSource = std::function<T(const FrameInfo&)>;

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

/*
 Elements apps and C++ pass around: immutable and shared, so copying one (e.g. with an app's state)
 is cheap, and data that hasn't changed is the same data (sameAs), which is how graphs know not to
 upload it again. An app's (data T) values, its readbacks' results, and graphs' per-frame data
 inputs are Data. Convert from a std::vector to make one.
 */
template<typename T>
class Data {
public:
    Data() = default;
    Data(std::vector<T> elements) : elements_(std::make_shared<const std::vector<T>>(std::move(elements))) {}

    uint32_t size() const { return elements_ ? static_cast<uint32_t>(elements_->size()) : 0; }
    bool empty() const { return size() == 0; }

    const T& operator[](size_t i) const {
        if (i >= size()) {
            contractViolation("Data index " + std::to_string(i) + " is past its " + std::to_string(size()) + " elements");
        }
        return (*elements_)[i];
    }

    const std::vector<T>& elements() const {
        static const std::vector<T> none;
        return elements_ ? *elements_ : none;
    }

    auto begin() const { return elements().begin(); }
    auto end() const { return elements().end(); }

    // Whether both are the same elements (not merely equal ones)
    bool sameAs(const Data& other) const { return elements_ == other.elements_; }

private:
    std::shared_ptr<const std::vector<T>> elements_;
};

// A per-frame value computed at most once a frame, however many of a graph's functions use it
// (e.g. a per-frame data input's size and its upload)
template<typename T>
class FrameCache {
public:
    explicit FrameCache(FrameSource<T> source) : source_(std::move(source)) {}

    const T& get(const FrameInfo& frame) {
        if (!frameNumber_ || *frameNumber_ != frame.frameNumber) {
            value_ = source_(frame);
            frameNumber_ = frame.frameNumber;
        }
        return value_;
    }

private:
    FrameSource<T> source_;
    T value_{};
    std::optional<uint64_t> frameNumber_;
};

/*
 Uploads a per-frame data input into a graph buffer with a copy per frame in flight (sized from the
 data, see RenderGraph::createBuffer's size source): before a frame's work, when the frame's buffer
 doesn't hold the frame's data (it changed, or the buffer was recreated), it's copied through a
 staging buffer. Data that doesn't change isn't copied again, and frames never wait for each other,
 as each writes its own copy.
 */
template<typename T>
class DataUploadNode : public TransferNode {
public:
    DataUploadNode(BufferId buffer, std::shared_ptr<FrameCache<Data<T>>> data) : buffer_(buffer), data_(std::move(data)) {}

    void setup(NodeBuilder& builder) override {
        graph_ = &builder.graph();
        builder.write(buffer_, Access::TransferDst);
        slots_ = PerFrame<Slot>(graph_->context(), [](uint32_t) { return Slot{}; });
    }

    Result<void> prepareFrame(FrameContext& context) override {
        const Data<T>& data = data_->get(frameInfo(context));
        Slot& slot = slots_[context.frameIndex];
        const VkBuffer target = graph_->buffer(buffer_, context.frameIndex);
        slot.copy = !(slot.target == target && slot.uploaded.sameAs(data)) && !data.empty();
        if (slot.copy) {
            // The slot's previous staging buffer has finished (the slot was waited for)
            TRY_ASSIGN(slot.staging, Buffer<T>::create(context.context, data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
            TRY(slot.staging->mapAndExecute(0, sizeof(T) * data.size(), [&data](void* mapped) {
                std::memcpy(mapped, data.elements().data(), sizeof(T) * data.size());
            }));
        }
        slot.target = target;
        slot.uploaded = data;
        return {};
    }

    void record(RecordContext& context) override {
        Slot& slot = slots_[context.frameIndex];
        if (!slot.copy) {
            return;
        }
        const VkBufferCopy region{0, 0, slot.staging->getSize()};
        vkCmdCopyBuffer(context.commandBuffer, slot.staging->getBuffer(), slot.target, 1, &region);
        ++copies_;
    }

    // How many copies it has recorded: for tests
    uint64_t copies() const { return copies_; }

private:
    // A frame slot's buffer, the data it holds, and whether this frame copies
    struct Slot {
        VkBuffer target = VK_NULL_HANDLE;
        Data<T> uploaded;
        bool copy = false;
        Handle<Buffer<T>> staging;
    };

    BufferId buffer_;
    std::shared_ptr<FrameCache<Data<T>>> data_;
    RenderGraph* graph_ = nullptr;
    PerFrame<Slot> slots_;
    uint64_t copies_ = 0;
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

    // The push constants computed for the frame (by update)
    const Push& pushValues() const {
        return values_;
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

// Pushes a node's own push constants, after binding its material: the material's for the frame,
// with those the node gives replaced by `write(frame, push)`
template<typename Material, typename Write>
void pushOverride(const RecordContext& record, PushInstance<Material>& material, Write&& write) {
    typename Material::Push push = material.pushValues();
    write(frameInfo(record), push);
    vkCmdPushConstants(record.commandBuffer, material.getPipelineLayout(), VK_SHADER_STAGE_ALL, Material::kPushOffset,
                       sizeof(push), &push);
}

// A DispatchSequenceNode step's workgroups, from a function of the frame
inline std::function<glm::vec3(const RecordContext&)> recordedWorkgroups(DispatchSource workgroups) {
    return [workgroups = std::move(workgroups)](const RecordContext& record) { return workgroups(frameInfo(record)); };
}

/*
 A dispatch with its own push constants or workgroups, replacing its material's (e.g. one per
 iteration of a loop, sharing a material, and so a pipeline). push, if given, pushes them after the
 material is bound (pushOverride); workgroups, if given, are the dispatch's for the frame, unless
 it dispatches indirectly, as many workgroups as the command at `command` says.
 */
class OverridingDispatchNode : public ComputeNode {
public:
    using PushFn = std::function<void(const RecordContext&)>;

    OverridingDispatchNode(Handle<ComputeMaterial> material, PushFn push, DispatchSource workgroups,
                           std::optional<BufferOffset> command)
    : material_(std::move(material)), push_(std::move(push)), workgroups_(std::move(workgroups)), command_(command) {
        if (workgroups_ && command_) {
            contractViolation("OverridingDispatchNode dispatches indirectly, so it takes no workgroups");
        }
    }

    void setup(NodeBuilder& builder) override {
        builder.uses(material_);
        if (command_) {
            builder.read(command_->buffer, Access::IndirectRead);
        }
    }

    void record(RecordContext& context) override {
        material_->bind(context.commandBuffer, context.frameIndex);
        if (push_) {
            push_(context);
        }
        if (command_) {
            vkCmdDispatchIndirect(context.commandBuffer, context.graph.buffer(command_->buffer, context.frameIndex),
                                  command_->offset);
            return;
        }
        const glm::vec3 groups = workgroups_ ? workgroups_(frameInfo(context)) : material_->getDispatchDimensions();
        vkCmdDispatch(context.commandBuffer, static_cast<uint32_t>(groups.x), static_cast<uint32_t>(groups.y),
                      static_cast<uint32_t>(groups.z));
    }

private:
    Handle<ComputeMaterial> material_;
    PushFn push_;
    DispatchSource workgroups_;
    std::optional<BufferOffset> command_;
};

// Element `index` of a graph's data input, checked to exist
template<typename T>
const T& elementOf(const std::vector<T>& elements, int64_t index, const char* graph, const char* input) {
    if (index < 0 || static_cast<uint64_t>(index) >= elements.size()) {
        contractViolation(std::string("Graph ") + graph + " reads element " + std::to_string(index) + " of " + input +
                          ", which has " + std::to_string(elements.size()));
    }
    return elements[static_cast<size_t>(index)];
}

// The id of iteration `index` of a node in a loop (of those its #:when condition kept)
inline NodeId nodeAt(const std::vector<NodeId>& nodes, size_t index, const char* graph, const char* node) {
    if (index >= nodes.size()) {
        contractViolation(std::string("Graph ") + graph + " has " + std::to_string(nodes.size()) + " nodes " + node +
                          ", so none at " + std::to_string(index));
    }
    return nodes[index];
}

// A graph's declaration that exists only under a condition, used where the condition was false
template<typename T>
const T& require(const std::optional<T>& value, const char* graph, const char* name) {
    if (!value) {
        contractViolation(std::string("Graph ") + graph + " uses " + name + ", whose #:when condition was false");
    }
    return *value;
}

// An extern input: adds nodes to the graph (with the ids of the graph's resources, and of the nodes
// before it), returning their ids
template<typename Graph>
using ExternNodes = std::function<std::vector<NodeId>(RenderGraph& graph, const Graph& ids)>;

// Records a copy node's copy of count elements of elementSize bytes, from sourceOffset elements into
// source to destinationOffset elements into destination. Without a count, it copies the rest of the
// source. Both buffers must hold the elements (whatever size the application's buffers have).
inline void copyBuffer(RecordContext& record, BufferId source, BufferId destination, VkDeviceSize elementSize,
                       VkDeviceSize sourceOffset, VkDeviceSize destinationOffset, std::optional<VkDeviceSize> count,
                       const char* node) {
    const VkDeviceSize sourceElements = record.graph.bufferSize(source, record.frameIndex) / elementSize;
    const VkDeviceSize destinationElements = record.graph.bufferSize(destination, record.frameIndex) / elementSize;
    const VkDeviceSize elements = count ? *count : (sourceOffset < sourceElements ? sourceElements - sourceOffset : 0);
    if (sourceOffset + elements > sourceElements || destinationOffset + elements > destinationElements) {
        contractViolation(std::string("Copy ") + node + " copies " + std::to_string(elements) + " elements from element " +
                          std::to_string(sourceOffset) + " of a buffer of " + std::to_string(sourceElements) +
                          " to element " + std::to_string(destinationOffset) + " of a buffer of " +
                          std::to_string(destinationElements));
    }
    if (elements == 0) {
        return;
    }
    const VkBufferCopy region{sourceOffset * elementSize, destinationOffset * elementSize, elements * elementSize};
    vkCmdCopyBuffer(record.commandBuffer, record.graph.buffer(source, record.frameIndex),
                    record.graph.buffer(destination, record.frameIndex), 1, &region);
}

// Records a copy node's copy of a buffer into a region of an image: the texels from sourceOffset
// bytes into the buffer, tightly packed in the image's format, into the region at offset of the
// given extent (by default, the rest of the image). A region outside the image, a buffer too small
// for it, or an offset Vulkan can't copy from (not a multiple of 4 and of the texel size) is a
// contract violation.
inline void copyBufferToImage(RecordContext& record, BufferId source, ImageId destination, VkDeviceSize sourceOffset,
                              VkOffset2D offset, std::optional<VkExtent2D> extent, const char* node) {
    const VkExtent2D size = record.graph.imageExtent(destination, record.frameIndex);
    const VkDeviceSize texelSize = Image::bytesPerPixel(record.graph.imageFormat(destination));
    const auto fail = [node](const std::string& problem) {
        contractViolation(std::string("Copy ") + node + " " + problem);
    };
    if (uint32_t(offset.x) > size.width || uint32_t(offset.y) > size.height) {
        fail("starts at texel (" + std::to_string(offset.x) + ", " + std::to_string(offset.y) + "), outside the image");
    }
    const VkExtent2D region = extent.value_or(VkExtent2D{size.width - uint32_t(offset.x), size.height - uint32_t(offset.y)});
    if (uint32_t(offset.x) + region.width > size.width || uint32_t(offset.y) + region.height > size.height) {
        fail("copies a region of " + std::to_string(region.width) + "x" + std::to_string(region.height) + " at (" +
             std::to_string(offset.x) + ", " + std::to_string(offset.y) + "), outside an image of " +
             std::to_string(size.width) + "x" + std::to_string(size.height));
    }
    if (sourceOffset % 4 != 0 || sourceOffset % texelSize != 0) {
        fail("starts " + std::to_string(sourceOffset) + " bytes into its buffer, which isn't a multiple of 4 and of the texel size");
    }
    const VkDeviceSize needed = texelSize * region.width * region.height;
    const VkDeviceSize available = record.graph.bufferSize(source, record.frameIndex);
    if (sourceOffset + needed > available) {
        fail("copies " + std::to_string(needed) + " bytes from byte " + std::to_string(sourceOffset) + " of a buffer of " +
             std::to_string(available));
    }
    if (region.width == 0 || region.height == 0) {
        return;
    }
    VkBufferImageCopy copy{};
    copy.bufferOffset = sourceOffset;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageOffset = {offset.x, offset.y, 0};
    copy.imageExtent = {region.width, region.height, 1};
    vkCmdCopyBufferToImage(record.commandBuffer, record.graph.buffer(source, record.frameIndex),
                           record.graph.image(destination, record.frameIndex)->getImage(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

// A device-local buffer holding data, for a graph's upload (which the graph imports): shaders and
// copies read it, and indirect dispatches and draws can read commands from it. what: the upload,
// for the contract violation an empty one is (a buffer can't be empty)
template<typename T>
Result<Handle<Buffer<T>>> upload(VulkanContext& context, const std::vector<T>& data, const char* what) {
    if (data.empty()) {
        contractViolation(std::string(what) + " is given no data");
    }
    return Buffer<T>::createAndInitialize(context, data,
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                              | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
}

// upload, of elements laid out as T's (e.g. Vulkan's indirect commands as ZGAL's structs)
template<typename T, typename U>
Result<Handle<Buffer<T>>> uploadAs(VulkanContext& context, const std::vector<U>& data, const char* what) {
    static_assert(sizeof(T) == sizeof(U) && std::is_trivially_copyable_v<U>, "uploadAs reinterprets elements of the same size");
    std::vector<T> converted(data.size());
    if (!data.empty()) {
        std::memcpy(converted.data(), data.data(), data.size() * sizeof(U));
    }
    return upload(context, converted, what);
}

// A graph's image-file: the image at path (found with VulkanContext::findAsset), loaded for sampling.
// what names it in errors.
inline Result<Handle<Image>> loadImage(VulkanContext& context, const std::filesystem::path& path, ColorSpace colorSpace,
                                       const char* what) {
    TRY_ASSIGN(auto found, context.findAsset(path).withContext(std::string("loading ") + what));
    return Image::createFromFile(context, found, colorSpace).withContext(std::string("loading ") + what);
}

// A graph's buffer-file: the files at paths (found with VulkanContext::findAsset), each a whole
// number of Ts as they're laid out in memory (e.g. a memory dump), in a device-local buffer in
// order. what names it in errors.
template<typename T>
Result<Handle<Buffer<T>>> loadBufferFile(VulkanContext& context, const std::vector<std::filesystem::path>& paths, const char* what) {
    static_assert(std::is_trivially_copyable_v<T>, "a buffer file holds Ts' bytes");
    const std::string loading = std::string("loading ") + what;
    std::vector<T> elements;
    for (const auto& path : paths) {
        TRY_ASSIGN(auto found, context.findAsset(path).withContext(loading));
        std::ifstream file(found, std::ios::binary);
        if (!file) {
            return Error(ErrorCode::NotFound, "Can't open '" + found.string() + "'").withContext(loading);
        }
        const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
        if (bytes.empty() || bytes.size() % sizeof(T) != 0) {
            return Error(ErrorCode::InvalidData, "'" + found.string() + "' holds " + std::to_string(bytes.size())
                                                     + " bytes, not a whole number of " + std::to_string(sizeof(T)) + "-byte elements")
                .withContext(loading);
        }
        const size_t first = elements.size();
        elements.resize(first + bytes.size() / sizeof(T));
        std::memcpy(elements.data() + first, bytes.data(), bytes.size());
    }
    return upload(context, elements, what).withContext(loading);
}

// The extent of an imported image (the current frame's, if it has one per frame)
inline VkExtent2D extentOf(VulkanContext& context, const FrameBinding<Image>& image, const char* name) {
    auto handle = image.get(context.currentFrame());
    if (!handle) {
        contractViolation(std::string("Imported image ") + name + " has no image");
    }
    return {handle->getWidth(), handle->getHeight()};
}


// Adds an indirect draw of as many of desc's commands as the uint32_t at `count` says (at most
// desc.maxDraws): natively where the device has drawIndirectCount, else by dispatching `limit` (the
// library's ZgalLimitDraws), which copies the commands into `limited` with those past the count
// given no instances, and drawing all of those. The limiting node is enabled with the draw
// (RenderGraph::enableWith), so the draw's id is all callers need. limitPush and limitWorkgroups,
// if given, are the limiting dispatch's own (as an OverridingDispatchNode's), e.g. in a loop.
inline NodeId addCountedDraw(RenderGraph& graph, const std::string& name, IndirectDrawNode::Desc desc,
                             BufferOffset count, Handle<ComputeMaterial> limit, BufferId limited,
                             OverridingDispatchNode::PushFn limitPush = nullptr, DispatchSource limitWorkgroups = nullptr) {
    if (graph.context().enabledFeatures().vulkan12.drawIndirectCount) {
        desc.count = count;
        return graph.add<IndirectDrawNode>(name, std::move(desc));
    }
    const NodeId limitNode = limitPush || limitWorkgroups
        ? graph.add<OverridingDispatchNode>(name + " (limit)", std::move(limit), std::move(limitPush), std::move(limitWorkgroups), std::nullopt)
        : graph.add<DispatchNode>(name + " (limit)", std::move(limit));
    desc.commands = BufferOffset{limited, 0};
    const NodeId draw = graph.add<IndirectDrawNode>(name, std::move(desc));
    graph.enableWith(limitNode, draw);
    return draw;
}

} // namespace zgal
