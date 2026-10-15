#pragma once

#include "core/VkTypes.h"
#include "core/VkUtil.h"
#include "core/VulkanContext.h"
#include "core/FrameResources.h"
#include "graph/GraphTypes.h"
#include "graph/GraphCompiler.h"
#include "graph/GraphSync.h"
#include "graph/Node.h"
#include "descriptors/DescriptorHeap.h"
#include "materials/Material.h"
#include "resources/Buffer.h"
#include "resources/Image.h"
#include "targets/RenderTarget.h"
#include "targets/Swapchain.h"

#include <any>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <variant>

/*
 A render graph: nodes that declare which resources they use and how, from which the graph derives
 execution order, queue assignment, barriers and semaphores. See docs in Node.h, GraphCompiler.h
 and GraphSync.h.

    RenderGraph graph(context, &swapchain);      // or RenderGraph(context) without a surface
    ImageId texture = graph.importImage("texture", textureHandle, ImageState::shaderReadOnly());
    ImageId output = graph.createImage("output", FrameMode::PerFrame, VK_FORMAT_R16G16B16A16_SFLOAT,
                                       FixedExtent{{512, 512}});
    NodeId copy = graph.add<DispatchNode>("copy", material);
    graph.node(copy).read(texture, Access::SampledRead).write(output, Access::StorageWrite);
    graph.execute();                              // one frame

 Each execute() begins and ends a frame on the context (VulkanContext::beginFrame/advanceFrame),
 waiting only for the frame slot it reuses.

 Resources:
    - Imported images rest in a declared state between frames: the graph expects them in it at the
      start of every frame and returns them to it at the end. Imported resources may be shared by
      every frame or have one per frame, as their FrameBinding says.
    - Created resources are owned by the graph, with usage derived from their declared uses. Whether
      they are per frame or shared is a required choice. Surface-relative images are recreated when
      the surface is resized.

 Plans (order, queues, submissions) are cached per set of enabled nodes, so enabling and disabling
 nodes only compiles combinations not seen before. Adding nodes, resources or dependencies clears
 the cache. Barriers and semaphore waits are derived while recording, from each resource's tracked
 state, so they stay correct whatever ran before.

 Not thread safe: use a graph from one thread at a time.
 */

struct FixedExtent {
    VkExtent2D extent;
};

// A multiple of the surface's extent; requires a graph with a surface
struct SurfaceRelative {
    float scale = 1.0f;
};

using ImageExtent = std::variant<FixedExtent, SurfaceRelative>;

struct SurfaceInfo {
    ImageId image;
    VkFormat format;
    VkExtent2D extent;
};

struct FrameResult {
    // Nothing was submitted, because no surface image could be acquired
    bool skipped = false;
    // The swapchain no longer matches the surface and should be recreated
    bool surfaceOutOfDate = false;
};

class RenderGraph {
public:
    using SetupFn = LambdaNode::SetupFn;
    using RecordFn = LambdaNode::RecordFn;
    using RunFn = LambdaNode::RunFn;

    // A graph without a surface, or one presenting to swapchain (which must outlive it)
    explicit RenderGraph(VulkanContext& context, Swapchain* swapchain = nullptr)
    : context_(&context), swapchain_(swapchain) {
        submittedPoints_.assign(context.queueCount(), 0);
        slots_ = PerFrame<FrameSlot>(context, [this](uint32_t) {
            FrameSlot slot;
            slot.queues.resize(context_->queueCount());
            return slot;
        });
        if (swapchain_) {
            ImageRecord surface;
            surface.name = "surface";
            surface.kind = ImageKind::Surface;
            images_.push_back(std::make_unique<ImageRecord>(std::move(surface)));
            surfaceImage_ = ImageId{0};
        }
    }

    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;

    ~RenderGraph() {
        // The graph's resources and command buffers may still be in use
        waitIdle();
    }

    VulkanContext& context() { return *context_; }

public: // Nodes
    template<typename NodeT, typename... Args>
    NodeId add(std::string name, Args&&... args) {
        return addNode(std::move(name), std::make_unique<NodeT>(std::forward<Args>(args)...));
    }

    NodeId addNode(std::string name, std::unique_ptr<Node> node) {
        NodeRecord record;
        record.name = std::move(name);
        record.node = std::move(node);
        nodes_.push_back(std::move(record));
        structureChanged();
        const uint32_t index = static_cast<uint32_t>(nodes_.size() - 1);
        NodeBuilder builder(*this, index);
        nodes_[index].node->setup(builder);
        return {index};
    }

    NodeId addGraphics(std::string name, SetupFn setup, RecordFn record) {
        return addNode(std::move(name), std::make_unique<LambdaNode>(NodeKind::Graphics, std::move(setup), std::move(record), nullptr));
    }

    NodeId addCompute(std::string name, SetupFn setup, RecordFn record) {
        return addNode(std::move(name), std::make_unique<LambdaNode>(NodeKind::Compute, std::move(setup), std::move(record), nullptr));
    }

    NodeId addTransfer(std::string name, SetupFn setup, RecordFn record) {
        return addNode(std::move(name), std::make_unique<LambdaNode>(NodeKind::Transfer, std::move(setup), std::move(record), nullptr));
    }

    NodeId addCpu(std::string name, SetupFn setup, RunFn run) {
        return addNode(std::move(name), std::make_unique<LambdaNode>(NodeKind::Cpu, std::move(setup), nullptr, std::move(run)));
    }

    // Declares more about a node after adding it (e.g. resources its materials use)
    NodeBuilder node(NodeId id) {
        nodeRecord(id);
        return NodeBuilder(*this, id.index);
    }

    template<typename NodeT>
    NodeT& nodeAs(NodeId id) {
        auto* node = dynamic_cast<NodeT*>(nodeRecord(id).node.get());
        if (!node) {
            throw std::invalid_argument("Node '" + nodeRecord(id).name + "' is not of the requested type");
        }
        return *node;
    }

    void addDependency(NodeId before, NodeId after) {
        nodeRecord(before);
        nodeRecord(after);
        dependencies_.push_back({before.index, after.index});
        structureChanged();
    }

    // Disabled nodes drop out of the plan. Plans are cached per set of enabled nodes.
    void setEnabled(NodeId id, bool enabled) {
        nodeRecord(id).enabled = enabled;
    }

    bool isEnabled(NodeId id) {
        return nodeRecord(id).enabled;
    }

public: // Resources
    // An image the graph doesn't own. It rests in `resting` between frames: the graph expects it in
    // that state at the start of every frame and returns it there at the end.
    ImageId importImage(std::string name, FrameBinding<Image> image, ImageState resting) {
        ImageRecord record;
        record.name = std::move(name);
        record.kind = ImageKind::Imported;
        record.perFrame = image.isPerFrame();
        record.resolveImported = [image = std::move(image)](uint32_t frame) { return image.get(frame); };
        record.resting = resting;
        return addImage(std::move(record));
    }

    // A buffer the graph doesn't own
    template<typename T>
    BufferId importBuffer(std::string name, FrameBinding<Buffer<T>> buffer) {
        BufferRecord record;
        record.name = std::move(name);
        record.kind = BufferKind::Imported;
        record.perFrame = buffer.isPerFrame();
        record.resolveImported = [buffer](uint32_t frame) -> VkBuffer {
            auto handle = buffer.get(frame);
            if (!handle) {
                throw GraphError("Imported buffer is unbound");
            }
            return handle->getBuffer();
        };
        record.makeEntry = [buffer](uint32_t frame, DescriptorHeap& heap) {
            return heap.storageBuffer(buffer.get(frame));
        };
        return addBuffer(std::move(record));
    }

    // An image owned by the graph. Its usage is derived from the declared accesses (plus
    // extraUsage). Whether it has a copy per frame in flight is a required choice.
    ImageId createImage(std::string name, FrameMode mode, VkFormat format, ImageExtent extent,
                        VkImageUsageFlags extraUsage = 0) {
        if (std::holds_alternative<SurfaceRelative>(extent) && !swapchain_) {
            throw GraphError("Image '" + name + "' is surface relative, but the graph has no surface");
        }
        ImageRecord record;
        record.name = std::move(name);
        record.kind = ImageKind::Created;
        record.perFrame = mode == FrameMode::PerFrame;
        record.format = format;
        record.extent = extent;
        record.extraUsage = extraUsage;
        return addImage(std::move(record));
    }

    // A buffer owned by the graph, device local unless memory says otherwise
    BufferId createBuffer(std::string name, FrameMode mode, VkDeviceSize size, VkBufferUsageFlags extraUsage = 0,
                          VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
        BufferRecord record;
        record.name = std::move(name);
        record.kind = BufferKind::Created;
        record.perFrame = mode == FrameMode::PerFrame;
        record.size = size;
        record.extraUsage = extraUsage;
        record.memory = memory;
        return addBuffer(std::move(record));
    }

    // The image used in a frame. Created images exist once the graph has compiled (compile(), or
    // the first execute()). Not available for the surface; see surfaceImageView().
    Handle<Image> image(ImageId id, uint32_t frameIndex) {
        auto& record = imageRecord(id);
        switch (record.kind) {
            case ImageKind::Imported: {
                auto image = record.resolveImported(frameIndex);
                if (!image) {
                    throw GraphError("Imported image '" + record.name + "' is unbound");
                }
                return image;
            }
            case ImageKind::Created:
                if (record.perFrame ? !record.perFrameImages : !record.shared) {
                    throw GraphError("Image '" + record.name + "' is created when the graph compiles");
                }
                return record.perFrame ? (*record.perFrameImages)[frameIndex] : record.shared;
            case ImageKind::Surface:
                break;
        }
        throw GraphError("The surface image is only available while recording");
    }

    // A graph-created buffer used in a frame (exists once the graph has compiled)
    Handle<Buffer<uint8_t>> createdBuffer(BufferId id, uint32_t frameIndex) {
        auto& record = bufferRecord(id);
        if (record.kind != BufferKind::Created) {
            throw GraphError("Buffer '" + record.name + "' was imported");
        }
        if (record.perFrame ? !record.perFrameBuffers : !record.shared) {
            throw GraphError("Buffer '" + record.name + "' is created when the graph compiles");
        }
        return record.perFrame ? (*record.perFrameBuffers)[frameIndex] : record.shared;
    }

    VkBuffer buffer(BufferId id, uint32_t frameIndex) {
        auto& record = bufferRecord(id);
        if (record.kind == BufferKind::Imported) {
            return record.resolveImported(frameIndex);
        }
        return createdBuffer(id, frameIndex)->getBuffer();
    }

    VkFormat imageFormat(ImageId id) {
        auto& record = imageRecord(id);
        switch (record.kind) {
            case ImageKind::Surface: return swapchain_->getFormat();
            case ImageKind::Created: return record.format;
            case ImageKind::Imported: return image(id, context_->currentFrame())->getFormat();
        }
        return VK_FORMAT_UNDEFINED;
    }

    VkExtent2D imageExtent(ImageId id, uint32_t frameIndex) {
        auto& record = imageRecord(id);
        switch (record.kind) {
            case ImageKind::Surface: return swapchain_->getExtent();
            case ImageKind::Created: return resolveExtent(record.extent);
            case ImageKind::Imported: {
                auto handle = image(id, frameIndex);
                return {handle->getWidth(), handle->getHeight()};
            }
        }
        return {0, 0};
    }

    // The formats a pipeline must be built against to render to target
    AttachmentFormats formats(const RenderTarget& target) {
        AttachmentFormats formats;
        for (const auto& attachment : target.color) {
            formats.color.push_back(imageFormat(attachment.image));
        }
        if (target.depth) {
            formats.depth = imageFormat(target.depth->image);
        }
        return formats;
    }

public: // Descriptor entries for graph resources
    // Entries of the universal descriptor set for a resource, one per frame, kept up to date as the
    // graph creates and recreates its images and buffers. Give them to materials (MaterialBinding):
    // a node using the material then declares the matching use automatically.
    FrameBinding<DescriptorEntry> sampledEntry(ImageId image) {
        imageRecord(image);
        return graphEntry(true, image.index, DescriptorType::SampledImage);
    }

    FrameBinding<DescriptorEntry> storageEntry(ImageId image) {
        imageRecord(image);
        return graphEntry(true, image.index, DescriptorType::StorageImage);
    }

    FrameBinding<DescriptorEntry> storageBufferEntry(BufferId buffer) {
        bufferRecord(buffer);
        return graphEntry(false, buffer.index, DescriptorType::StorageBuffer);
    }

public: // Surface
    bool hasSurface() { return swapchain_ != nullptr; }

    SurfaceInfo surface() {
        requireSurface();
        return {surfaceImage_, swapchain_->getFormat(), swapchain_->getExtent()};
    }

    // A render target that clears the surface, with a surface-sized depth image created by the
    // graph (none if depthFormat is empty). The depth image is shared between frames: its
    // contents are discarded every frame.
    RenderTarget surfaceTarget(std::array<float, 4> clear = {0.0f, 0.0f, 0.0f, 1.0f},
                               std::optional<VkFormat> depthFormat = VK_FORMAT_D32_SFLOAT) {
        requireSurface();
        RenderTarget target;
        target.color.push_back({surfaceImage_, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, clear});
        if (depthFormat) {
            auto it = surfaceDepth_.find(*depthFormat);
            if (it == surfaceDepth_.end()) {
                it = surfaceDepth_.emplace(*depthFormat, createImage("surface depth", FrameMode::Shared, *depthFormat,
                                                                     SurfaceRelative{1.0f})).first;
            }
            target.depth = DepthAttachment{it->second};
        }
        return target;
    }

public: // Compiling and running
    // Compiles the plan for the enabled nodes (or reuses a cached one) and creates the graph's
    // resources. execute() does this when needed; calling it explicitly reports errors early.
    void compile() {
        prepare();
        currentPlan();
    }

    // Runs one frame: waits for the frame slot's previous use, acquires the surface image if the
    // plan uses it, runs CPU nodes and records and submits GPU nodes in order (updating materials
    // before the first submission using them), presents, and advances the context's frame.
    FrameResult execute() {
        context_->beginFrame();
        const uint32_t frame = context_->currentFrame();
        FrameResult result;

        prepare();
        const CompiledGraph& plan = currentPlan();
        refreshEntries(frame);

        // A frame executed by something else (another graph) may still be using imported resources
        const bool continuous = lastFrameNumber_ && *lastFrameNumber_ + 1 == context_->frameNumber();
        std::optional<VulkanContext::TimelinePoints> foreignWork;
        if (!continuous) {
            foreignWork = context_->submittedPoints();
            markImportedUnknown();
        }

        Swapchain::Acquired acquired{};
        if (plan.usesSurface) {
            acquired = swapchain_->acquire(frame);
            if (acquired.result == VK_ERROR_OUT_OF_DATE_KHR) {
                result.skipped = true;
                result.surfaceOutOfDate = true;
                context_->advanceFrame();
                return result;
            }
            result.surfaceOutOfDate = acquired.result == VK_SUBOPTIMAL_KHR;
            surfaceState_ = ResourceSyncState::forImage();
            surfaceState_.externalSemaphore = true;
            acquiredImage_ = acquired.imageIndex;
        }

        auto& slot = slots_[frame];
        for (auto& queue : slot.queues) {
            if (queue.pool) {
                vkResetCommandPool(context_->device(), queue.pool->get(), 0);
                queue.used = 0;
            }
        }

        FrameContext frameContext{*context_, *this, frame, context_->frameNumber(),
                                  swapchain_ ? swapchain_->getExtent() : VkExtent2D{0, 0}};

        // Each distinct material is updated once, just before the first submission recording it,
        // so CPU nodes ordered before its nodes have run
        std::set<Material*> updated;

        std::vector<uint64_t> submissionValues(plan.submissions.size(), 0);
        for (const auto& step : plan.steps) {
            if (step.cpu) {
                // Host work after the GPU work it depends on
                for (uint32_t predecessor : plan.predecessors[step.index]) {
                    int32_t submission = plan.nodeSubmission[predecessor];
                    if (submission >= 0) {
                        context_->wait(plan.submissions[submission].queue, submissionValues[submission]);
                    }
                }
                nodes_[step.index].node->run(frameContext);
                continue;
            }
            for (uint32_t node : plan.submissions[step.index].nodes) {
                for (auto& material : nodes_[node].materials) {
                    if (updated.insert(material.get()).second) {
                        material->update(frame, frameContext.surfaceExtent);
                    }
                }
            }
            submissionValues[step.index] = recordAndSubmit(plan, step.index, slot, frameContext, submissionValues,
                                                           foreignWork, acquired);
        }

        if (plan.usesSurface) {
            VkSemaphore renderFinished = swapchain_->getRenderFinishedSemaphore(acquired.imageIndex);
            VkSwapchainKHR swapchain = swapchain_->get();
            VkPresentInfoKHR presentInfo{};
            presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            presentInfo.waitSemaphoreCount = 1;
            presentInfo.pWaitSemaphores = &renderFinished;
            presentInfo.swapchainCount = 1;
            presentInfo.pSwapchains = &swapchain;
            presentInfo.pImageIndices = &acquired.imageIndex;
            VkResult presented = context_->present(presentInfo);
            if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
                result.surfaceOutOfDate = true;
            } else {
                VK_SUCCESS_OR_THROW(presented, "Failed to present swapchain image");
            }
        }

        lastFrameNumber_ = context_->frameNumber();
        context_->advanceFrame();
        return result;
    }

    // Blocks until everything this graph submitted has completed (not the whole device)
    void waitIdle() {
        context_->wait(submittedPoints_);
    }

    // Called with compiler warnings (by default printed to std::cerr)
    void setWarningHandler(std::function<void(const std::string&)> handler) {
        warningHandler_ = std::move(handler);
    }

    // The current plan, for debugging
    std::string dumpPlan() {
        prepare();
        return currentPlan().describe(lastDesc_);
    }

    // The queue the current plan runs a node on; -1 for CPU and disabled nodes
    int32_t queueOf(NodeId id) {
        nodeRecord(id);
        prepare();
        return currentPlan().nodeQueue[id.index];
    }

    // Number of plans compiled so far, and cached now (for tests)
    uint64_t compileCount() const { return compileCount_; }
    size_t cachedPlanCount() const { return plans_.size(); }

private:
    friend class NodeBuilder;

    enum class ImageKind { Imported, Created, Surface };
    enum class BufferKind { Imported, Created };

    struct NodeUse {
        bool image;
        uint32_t index;
        Access access;
        VkPipelineStageFlags2 shaderStages;
        bool discard;
    };

    struct NodeRecord {
        std::string name;
        std::unique_ptr<Node> node;
        bool enabled = true;
        bool pinToPrimaryQueue = false;
        std::vector<NodeUse> uses;
        std::optional<RenderTarget> target;
        std::vector<Handle<Material>> materials;
    };

    struct ImageRecord {
        std::string name;
        ImageKind kind = ImageKind::Created;
        bool perFrame = false;
        // Imported
        std::function<Handle<Image>(uint32_t)> resolveImported;
        ImageState resting;
        // Created
        VkFormat format = VK_FORMAT_UNDEFINED;
        ImageExtent extent = FixedExtent{{0, 0}};
        VkImageUsageFlags extraUsage = 0;
        VkImageUsageFlags usage = 0;
        VkExtent2D realizedExtent{0, 0};
        Handle<Image> shared;
        std::unique_ptr<PerFrame<Handle<Image>>> perFrameImages;
        // Tracked state, of the one copy or each frame's copy
        ResourceSyncState sharedState = ResourceSyncState::forImage();
        std::unique_ptr<PerFrame<ResourceSyncState>> frameStates;
    };

    struct BufferRecord {
        std::string name;
        BufferKind kind = BufferKind::Created;
        bool perFrame = false;
        std::function<VkBuffer(uint32_t)> resolveImported;
        // Creates a storage buffer entry for a frame (imported buffers of a known type)
        std::function<Handle<DescriptorEntry>(uint32_t, DescriptorHeap&)> makeEntry;
        VkDeviceSize size = 0;
        VkBufferUsageFlags extraUsage = 0;
        VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        VkBufferUsageFlags usage = 0;
        Handle<Buffer<uint8_t>> shared;
        std::unique_ptr<PerFrame<Handle<Buffer<uint8_t>>>> perFrameBuffers;
        ResourceSyncState sharedState = ResourceSyncState::forBuffer();
        std::unique_ptr<PerFrame<ResourceSyncState>> frameStates;
    };

    // Descriptor entries the graph keeps up to date for one of its resources
    struct GraphEntries {
        bool image;
        uint32_t index;
        DescriptorType type;
        Handle<PerFrameHandles<DescriptorEntry>> collection;
    };

    // Command buffers of a frame slot, per queue
    struct FrameSlot {
        struct Queue {
            std::unique_ptr<CommandPool> pool;
            std::vector<VkCommandBuffer> buffers;
            uint32_t used = 0;
        };
        std::vector<Queue> queues;
    };

    NodeRecord& nodeRecord(NodeId id) {
        if (id.index >= nodes_.size()) {
            throw std::out_of_range("Unknown node");
        }
        return nodes_[id.index];
    }

    ImageRecord& imageRecord(ImageId id) {
        if (id.index >= images_.size()) {
            throw std::out_of_range("Unknown image");
        }
        return *images_[id.index];
    }

    BufferRecord& bufferRecord(BufferId id) {
        if (id.index >= buffers_.size()) {
            throw std::out_of_range("Unknown buffer");
        }
        return *buffers_[id.index];
    }

    void requireSurface() {
        if (!swapchain_) {
            throw std::logic_error("The graph has no surface");
        }
    }

    void structureChanged() {
        structureDirty_ = true;
    }

    ImageId addImage(ImageRecord record) {
        images_.push_back(std::make_unique<ImageRecord>(std::move(record)));
        auto* added = images_.back().get();
        if (added->perFrame) {
            const ResourceSyncState initial = initialImageState(*added);
            added->frameStates = std::make_unique<PerFrame<ResourceSyncState>>(*context_, [initial](uint32_t) {
                return initial;
            });
        } else {
            added->sharedState = initialImageState(*added);
        }
        structureChanged();
        return {static_cast<uint32_t>(images_.size() - 1)};
    }

    BufferId addBuffer(BufferRecord record) {
        buffers_.push_back(std::make_unique<BufferRecord>(std::move(record)));
        auto* added = buffers_.back().get();
        if (added->perFrame) {
            added->frameStates = std::make_unique<PerFrame<ResourceSyncState>>(*context_, [](uint32_t) {
                return ResourceSyncState::forBuffer();
            });
        }
        structureChanged();
        return {static_cast<uint32_t>(buffers_.size() - 1)};
    }

    static ResourceSyncState initialImageState(const ImageRecord& record) {
        if (record.kind == ImageKind::Imported) {
            // Whatever happened to it before the graph is unknown, but it rests in this layout
            auto state = ResourceSyncState::forImage(record.resting.layout);
            state.unknown = true;
            return state;
        }
        return ResourceSyncState::forImage();
    }

    void markImportedUnknown() {
        for (auto& record : images_) {
            if (record->kind != ImageKind::Imported) {
                continue;
            }
            auto mark = [&](ResourceSyncState& state) {
                state.unknown = true;
                state.layout = record->resting.layout;
            };
            if (record->frameStates) {
                for (auto& state : *record->frameStates) mark(state);
            } else {
                mark(record->sharedState);
            }
        }
        for (auto& record : buffers_) {
            if (record->kind != BufferKind::Imported) {
                continue;
            }
            if (record->frameStates) {
                for (auto& state : *record->frameStates) state.unknown = true;
            } else {
                record->sharedState.unknown = true;
            }
        }
    }

    ResourceSyncState& imageState(uint32_t index, uint32_t frame) {
        auto& record = *images_[index];
        if (record.kind == ImageKind::Surface) {
            return surfaceState_;
        }
        return record.frameStates ? (*record.frameStates)[frame] : record.sharedState;
    }

    ResourceSyncState& bufferState(uint32_t index, uint32_t frame) {
        auto& record = *buffers_[index];
        return record.frameStates ? (*record.frameStates)[frame] : record.sharedState;
    }

    VkExtent2D resolveExtent(const ImageExtent& extent) {
        if (auto* fixed = std::get_if<FixedExtent>(&extent)) {
            return fixed->extent;
        }
        const float scale = std::get<SurfaceRelative>(extent).scale;
        const VkExtent2D surface = swapchain_->getExtent();
        return {std::max(1u, static_cast<uint32_t>(surface.width * scale)),
                std::max(1u, static_cast<uint32_t>(surface.height * scale))};
    }

    static VkImageAspectFlagBits aspectFor(VkFormat format) {
        switch (format) {
            case VK_FORMAT_D16_UNORM: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_X8_D24_UNORM_PACK32:
            case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
                return VK_IMAGE_ASPECT_DEPTH_BIT;
            default:
                return VK_IMAGE_ASPECT_COLOR_BIT;
        }
    }

    // Creates (or recreates) graph-owned resources whose usage or size changed
    void prepare() {
        if (swapchain_) {
            const VkExtent2D extent = swapchain_->getExtent();
            if (swapchain_->getFormat() != surfaceFormat_) {
                surfaceFormat_ = swapchain_->getFormat();
                plans_.clear();   // Material formats are checked against the surface's when compiling
            }
            surfaceExtent_ = extent;
        }
        if (!structureDirty_ && realizedSurfaceExtent_.width == surfaceExtent_.width
            && realizedSurfaceExtent_.height == surfaceExtent_.height) {
            return;
        }
        if (structureDirty_) {
            plans_.clear();
            structureDirty_ = false;
        }
        realizedSurfaceExtent_ = surfaceExtent_;

        // Usage from every node's declared accesses, enabled or not, so toggling never reallocates
        std::vector<VkImageUsageFlags> imageUsage(images_.size(), 0);
        std::vector<VkBufferUsageFlags> bufferUsage(buffers_.size(), 0);
        for (const auto& node : nodes_) {
            for (const auto& use : node.uses) {
                if (use.image) {
                    imageUsage[use.index] |= requiredImageUsage(use.access);
                } else {
                    bufferUsage[use.index] |= requiredBufferUsage(use.access);
                }
            }
        }
        for (uint32_t i = 0; i < images_.size(); ++i) {
            auto& record = *images_[i];
            if (record.kind != ImageKind::Created) {
                continue;
            }
            const VkImageUsageFlags usage = imageUsage[i] | record.extraUsage;
            const VkExtent2D extent = resolveExtent(record.extent);
            const bool exists = record.perFrame ? record.perFrameImages != nullptr : static_cast<bool>(record.shared);
            if (exists && usage == record.usage && extent.width == record.realizedExtent.width
                && extent.height == record.realizedExtent.height) {
                continue;
            }
            retireImage(record);
            record.usage = usage;
            record.realizedExtent = extent;
            auto create = [this, format = record.format, extent, usage, name = record.name]() {
                Handle<Image> image;
                if (usage == 0) {
                    throw GraphError("Image '" + name + "' is never used");
                }
                VK_SUCCESS_OR_THROW(Image::create(image, *context_, extent.width, extent.height, format,
                                                  VK_IMAGE_TILING_OPTIMAL, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                  aspectFor(format)),
                                    "Failed to create graph image");
                return image;
            };
            if (usage == 0) {
                continue;   // Unused images are created once something uses them
            }
            if (record.perFrame) {
                record.perFrameImages = std::make_unique<PerFrame<Handle<Image>>>(*context_, [create](uint32_t) {
                    return create();
                });
                for (auto& state : *record.frameStates) {
                    state = ResourceSyncState::forImage();
                }
            } else {
                record.shared = create();
                record.sharedState = ResourceSyncState::forImage();
            }
        }
        for (uint32_t i = 0; i < buffers_.size(); ++i) {
            auto& record = *buffers_[i];
            if (record.kind != BufferKind::Created) {
                continue;
            }
            const VkBufferUsageFlags usage = bufferUsage[i] | record.extraUsage;
            const bool exists = record.perFrame ? record.perFrameBuffers != nullptr : static_cast<bool>(record.shared);
            if ((exists && usage == record.usage) || usage == 0) {
                continue;
            }
            retireBuffer(record);
            record.usage = usage;
            auto create = [this, size = record.size, usage, memory = record.memory]() {
                Handle<Buffer<uint8_t>> buffer;
                VK_SUCCESS_OR_THROW(Buffer<uint8_t>::create(buffer, *context_, size, usage, memory),
                                    "Failed to create graph buffer");
                return buffer;
            };
            if (record.perFrame) {
                record.perFrameBuffers = std::make_unique<PerFrame<Handle<Buffer<uint8_t>>>>(*context_, [create](uint32_t) {
                    return create();
                });
                for (auto& state : *record.frameStates) {
                    state = ResourceSyncState::forBuffer();
                }
            } else {
                record.shared = create();
                record.sharedState = ResourceSyncState::forBuffer();
            }
        }
    }

    // Destroys a created image once the work that may use it has finished
    void retireImage(ImageRecord& record) {
        auto shared = std::exchange(record.shared, nullptr);
        std::shared_ptr<PerFrame<Handle<Image>>> perFrame(record.perFrameImages.release());
        if (shared || perFrame) {
            context_->retire([shared, perFrame]() mutable {
                shared.reset();
                perFrame.reset();
            });
        }
    }

    void retireBuffer(BufferRecord& record) {
        auto shared = std::exchange(record.shared, nullptr);
        std::shared_ptr<PerFrame<Handle<Buffer<uint8_t>>>> perFrame(record.perFrameBuffers.release());
        if (shared || perFrame) {
            context_->retire([shared, perFrame]() mutable {
                shared.reset();
                perFrame.reset();
            });
        }
    }

    GraphDesc describe() {
        GraphDesc desc;
        desc.queues.clear();
        for (uint32_t queue = 0; queue < context_->queueCount(); ++queue) {
            desc.queues.push_back({context_->queueDescription(queue).flags});
        }
        const uint32_t imageCount = static_cast<uint32_t>(images_.size());
        for (const auto& image : images_) {
            desc.resources.push_back({image->name, true, image->kind == ImageKind::Surface, image->perFrame,
                                      image->kind == ImageKind::Imported});
        }
        for (const auto& buffer : buffers_) {
            desc.resources.push_back({buffer->name, false, false, buffer->perFrame, false});
        }
        for (const auto& node : nodes_) {
            CompilerNode compilerNode{node.name, node.node->kind(), node.enabled, node.pinToPrimaryQueue, {}};
            for (const auto& use : node.uses) {
                compilerNode.uses.push_back({use.image ? use.index : imageCount + use.index, use.access, use.shaderStages});
            }
            desc.nodes.push_back(std::move(compilerNode));
        }
        desc.dependencies = dependencies_;
        return desc;
    }

    // Checks what the compiler can't see: usage flags, formats, and images known twice
    void validate(const CompiledGraph& plan) {
        std::map<Image*, std::string> seen;
        for (uint32_t i = 0; i < images_.size(); ++i) {
            auto& record = *images_[i];
            if (record.kind == ImageKind::Surface || (record.kind == ImageKind::Created && !record.shared && !record.perFrameImages)) {
                continue;
            }
            Image* resolved = image({i}, context_->currentFrame()).get();
            auto [it, inserted] = seen.emplace(resolved, record.name);
            if (!inserted) {
                throw GraphError("Images '" + it->second + "' and '" + record.name + "' are the same image; import it once "
                                 "(materials' entries are matched to imported images by handle)");
            }
        }
        for (uint32_t node : plan.order) {
            const auto& record = nodes_[node];
            for (const auto& material : record.materials) {
                if (auto error = material->undeclaredWrites(); !error.empty()) {
                    throw GraphError("Node '" + record.name + "': " + error);
                }
            }
            if (record.node->kind() == NodeKind::Graphics && !record.target) {
                throw GraphError("Graphics node '" + record.name + "' has no render target (see NodeBuilder::renderTo)");
            }
            for (const auto& use : record.uses) {
                if (!use.image) {
                    continue;
                }
                const auto& image = *images_[use.index];
                VkImageUsageFlags usage = 0;
                switch (image.kind) {
                    case ImageKind::Surface: usage = swapchain_->getUsage(); break;
                    case ImageKind::Created: continue;   // Derived from the uses
                    case ImageKind::Imported: usage = this->image({use.index}, context_->currentFrame())->getUsage(); break;
                }
                const VkImageUsageFlags required = requiredImageUsage(use.access);
                if ((usage & required) != required) {
                    throw GraphError("Node '" + record.name + "' uses image '" + image.name + "' with access "
                                     + accessName(use.access) + ", which needs usage " + string_VkImageUsageFlags(required)
                                     + " the image wasn't created with");
                }
            }
            if (record.target) {
                const AttachmentFormats target = formats(*record.target);
                for (const auto& material : record.materials) {
                    auto materialFormats = material->attachmentFormats();
                    if (materialFormats && !(*materialFormats == target)) {
                        throw GraphError("Node '" + record.name + "' draws with a material built for attachment formats "
                                         + materialFormats->describe() + " into a target with formats "
                                         + target.describe());
                    }
                }
            }
        }
    }

    FrameBinding<DescriptorEntry> graphEntry(bool image, uint32_t index, DescriptorType type) {
        for (auto& entries : graphEntries_) {
            if (entries.image == image && entries.index == index && entries.type == type) {
                return entries.collection;
            }
        }
        auto collection = context_->resources().create<PerFrameHandles<DescriptorEntry>>(*context_, [](uint32_t) {
            return Handle<DescriptorEntry>();
        });
        graphEntries_.push_back({image, index, type, collection});
        return collection;
    }

    // Makes the frame's graph entries describe the frame's current resources. Replaced entries
    // free their slots once in-flight work is done.
    void refreshEntries(uint32_t frame) {
        auto& heap = context_->descriptors();
        for (auto& entries : graphEntries_) {
            auto& entry = (*entries.collection)[frame];
            if (entries.image) {
                if (images_[entries.index]->kind == ImageKind::Surface) {
                    throw GraphError("The surface image has no descriptor entries");
                }
                Handle<Image> current = image({entries.index}, frame);
                if (!entry || entry->image() != current) {
                    entry = entries.type == DescriptorType::SampledImage ? heap.sampledImage(current)
                                                                         : heap.storageImage(current);
                }
            } else {
                auto& record = *buffers_[entries.index];
                const VkBuffer current = buffer({entries.index}, frame);
                if (!entry || entry->buffer() != current) {
                    entry = record.kind == BufferKind::Created ? heap.storageBuffer(createdBuffer({entries.index}, frame))
                                                               : record.makeEntry(frame, heap);
                }
            }
        }
    }

    // The graph resource a material binding refers to, importing it if the graph doesn't know it.
    // Empty for samplers.
    std::optional<std::pair<bool, uint32_t>> resolveBinding(const MaterialBinding& binding, const std::string& nodeName,
                                                            size_t bindingIndex) {
        const std::string name = nodeName + " binding " + std::to_string(bindingIndex);
        if (auto collection = binding.entry.collection()) {
            for (const auto& entries : graphEntries_) {
                if (entries.collection == collection) {
                    return std::make_pair(entries.image, entries.index);
                }
            }
            auto it = entryImports_.find(collection.get());
            if (it != entryImports_.end()) {
                return it->second;
            }
            auto first = collection->empty() ? Handle<DescriptorEntry>() : (*collection)[0];
            if (!first) {
                throw GraphError("Material " + name + " has no descriptor entry");
            }
            if (first->type() == DescriptorType::Sampler) {
                return std::nullopt;
            }
            std::pair<bool, uint32_t> resource;
            if (first->type() == DescriptorType::StorageBuffer) {
                BufferRecord record;
                record.name = name;
                record.kind = BufferKind::Imported;
                record.perFrame = true;
                record.resolveImported = [collection](uint32_t frame) { return (*collection)[frame]->buffer(); };
                resource = {false, addBuffer(std::move(record)).index};
            } else {
                ImageRecord record;
                record.name = name;
                record.kind = ImageKind::Imported;
                record.perFrame = true;
                record.resolveImported = [collection](uint32_t frame) { return (*collection)[frame]->image(); };
                record.resting = first->type() == DescriptorType::SampledImage ? ImageState::shaderReadOnly() : ImageState::general();
                resource = {true, addImage(std::move(record)).index};
            }
            entryImports_[collection.get()] = resource;
            return resource;
        }

        auto entry = binding.entry.shared();
        if (!entry) {
            throw GraphError("Material " + name + " has no descriptor entry");
        }
        switch (entry->type()) {
            case DescriptorType::Sampler:
                return std::nullopt;
            case DescriptorType::StorageBuffer: {
                for (uint32_t i = 0; i < buffers_.size(); ++i) {
                    auto& record = *buffers_[i];
                    if (!record.perFrame && record.kind == BufferKind::Imported && record.resolveImported(0) == entry->buffer()) {
                        return std::make_pair(false, i);
                    }
                }
                BufferRecord record;
                record.name = name;
                record.kind = BufferKind::Imported;
                record.resolveImported = [entry](uint32_t) { return entry->buffer(); };
                return std::make_pair(false, addBuffer(std::move(record)).index);
            }
            default: {
                for (uint32_t i = 0; i < images_.size(); ++i) {
                    auto& record = *images_[i];
                    if (!record.perFrame && record.kind == ImageKind::Imported && record.resolveImported(0) == entry->image()) {
                        return std::make_pair(true, i);
                    }
                    if (!record.perFrame && record.kind == ImageKind::Created && record.shared == entry->image()) {
                        return std::make_pair(true, i);
                    }
                }
                ImageRecord record;
                record.name = name;
                record.kind = ImageKind::Imported;
                record.resolveImported = [image = entry->image()](uint32_t) { return image; };
                record.resting = entry->type() == DescriptorType::SampledImage ? ImageState::shaderReadOnly() : ImageState::general();
                return std::make_pair(true, addImage(std::move(record)).index);
            }
        }
    }

    const CompiledGraph& currentPlan() {
        std::vector<bool> enabled;
        for (const auto& node : nodes_) {
            enabled.push_back(node.enabled);
        }
        auto it = plans_.find(enabled);
        if (it != plans_.end()) {
            return it->second;
        }
        lastDesc_ = describe();
        CompiledGraph plan = compileGraph(lastDesc_);
        validate(plan);
        ++compileCount_;
        for (const auto& warning : plan.warnings) {
            if (warningHandler_) {
                warningHandler_(warning);
            } else {
                std::cerr << "[RenderGraph warning] " << warning << std::endl;
            }
        }
        return plans_.emplace(std::move(enabled), std::move(plan)).first->second;
    }

    VkCommandBuffer nextCommandBuffer(FrameSlot& slot, uint32_t queue) {
        auto& entry = slot.queues[queue];
        if (!entry.pool) {
            entry.pool = context_->createCommandPool(queue, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT);
        }
        if (entry.used == entry.buffers.size()) {
            entry.buffers.push_back(entry.pool->allocateOne());
        }
        return entry.buffers[entry.used++];
    }

    uint64_t recordAndSubmit(const CompiledGraph& plan, uint32_t submissionIndex, FrameSlot& slot,
                             FrameContext& frameContext, const std::vector<uint64_t>& submissionValues,
                             const std::optional<VulkanContext::TimelinePoints>& foreignWork,
                             const Swapchain::Acquired& acquired) {
        const auto& submission = plan.submissions[submissionIndex];
        const uint32_t queue = submission.queue;
        const uint32_t frame = frameContext.frameIndex;
        VkCommandBuffer commandBuffer = nextCommandBuffer(slot, queue);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_SUCCESS_OR_THROW(vkBeginCommandBuffer(commandBuffer, &beginInfo), "Failed to begin command buffer");

        // The universal descriptor set, for every material recorded here
        auto& heap = context_->descriptors();
        if (context_->queueDescription(queue).flags & VK_QUEUE_GRAPHICS_BIT) {
            heap.bind(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS);
        }
        heap.bind(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE);

        // Timeline waits on other queues, merged per queue
        std::map<uint32_t, std::pair<uint64_t, VkPipelineStageFlags2>> waits;
        auto addWait = [&](uint32_t waitQueue, uint64_t value, VkPipelineStageFlags2 stages) {
            if (waitQueue == queue || value == 0) {
                return;
            }
            if (value == TimelinePoint::kPendingValue) {
                throw std::logic_error("Render graph waited on work that wasn't submitted yet");
            }
            auto& wait = waits[waitQueue];
            wait.first = std::max(wait.first, value);
            wait.second |= stages;
        };
        if (foreignWork) {
            for (uint32_t other = 0; other < foreignWork->size(); ++other) {
                addWait(other, (*foreignWork)[other], VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
            }
        }
        VkPipelineStageFlags2 acquireStages = VK_PIPELINE_STAGE_2_NONE;
        std::vector<ResourceSyncState*> touched;

        struct Barriers {
            std::vector<VkImageMemoryBarrier2> images;
            std::vector<VkBufferMemoryBarrier2> buffers;
            std::vector<VkMemoryBarrier2> memory;
        };
        auto applyImageUse = [&](uint32_t index, const SyncUse& use, Barriers& barriers) {
            ResourceSyncState& state = imageState(index, frame);
            if (images_[index]->kind == ImageKind::Surface && state.externalSemaphore) {
                acquireStages |= use.stages;
            }
            SyncOps ops = applyUse(state, use);
            touched.push_back(&state);
            for (const auto& wait : ops.waits) {
                addWait(wait.point.queue, wait.point.value, wait.stages);
            }
            if (ops.barrier) {
                VkImageMemoryBarrier2 barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                barrier.srcStageMask = ops.barrier->srcStages;
                barrier.srcAccessMask = ops.barrier->srcAccess;
                barrier.dstStageMask = ops.barrier->dstStages;
                barrier.dstAccessMask = ops.barrier->dstAccess;
                barrier.oldLayout = ops.barrier->oldLayout;
                barrier.newLayout = ops.barrier->newLayout;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                VkFormat format;
                if (images_[index]->kind == ImageKind::Surface) {
                    barrier.image = swapchain_->getImage(acquired.imageIndex);
                    format = swapchain_->getFormat();
                } else {
                    auto image = this->image({index}, frame);
                    barrier.image = image->getImage();
                    format = image->getFormat();
                }
                barrier.subresourceRange = {static_cast<VkImageAspectFlags>(aspectFor(format)), 0, 1, 0, 1};
                barriers.images.push_back(barrier);
            }
        };
        auto applyBufferUse = [&](uint32_t index, const SyncUse& use, Barriers& barriers) {
            ResourceSyncState& state = bufferState(index, frame);
            SyncOps ops = applyUse(state, use);
            touched.push_back(&state);
            for (const auto& wait : ops.waits) {
                addWait(wait.point.queue, wait.point.value, wait.stages);
            }
            if (ops.barrier) {
                VkBufferMemoryBarrier2 barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                barrier.srcStageMask = ops.barrier->srcStages;
                barrier.srcAccessMask = ops.barrier->srcAccess;
                barrier.dstStageMask = ops.barrier->dstStages;
                barrier.dstAccessMask = ops.barrier->dstAccess;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.buffer = buffer({index}, frame);
                barrier.offset = 0;
                barrier.size = VK_WHOLE_SIZE;
                barriers.buffers.push_back(barrier);
            }
        };
        auto recordBarriers = [&](Barriers& barriers) {
            if (barriers.images.empty() && barriers.buffers.empty() && barriers.memory.empty()) {
                return;
            }
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.memoryBarrierCount = static_cast<uint32_t>(barriers.memory.size());
            dependency.pMemoryBarriers = barriers.memory.data();
            dependency.bufferMemoryBarrierCount = static_cast<uint32_t>(barriers.buffers.size());
            dependency.pBufferMemoryBarriers = barriers.buffers.data();
            dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.images.size());
            dependency.pImageMemoryBarriers = barriers.images.data();
            vkCmdPipelineBarrier2(commandBuffer, &dependency);
        };

        for (uint32_t node : submission.nodes) {
            auto& record = nodes_[node];
            Barriers barriers;

            // Explicit dependencies: a global barrier on the same queue, a timeline wait otherwise
            for (uint32_t predecessor : plan.explicitPredecessors[node]) {
                const int32_t predecessorSubmission = plan.nodeSubmission[predecessor];
                if (predecessorSubmission < 0) {
                    continue;   // A CPU node, which ran before this submission was recorded
                }
                if (plan.submissions[predecessorSubmission].queue == queue) {
                    VkMemoryBarrier2 barrier{};
                    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
                    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
                    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                    barriers.memory.push_back(barrier);
                } else {
                    addWait(plan.submissions[predecessorSubmission].queue, submissionValues[predecessorSubmission],
                            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
                }
            }

            const VkPipelineStageFlags2 defaultStages = defaultShaderStages(record.node->kind());
            for (const auto& use : record.uses) {
                SyncUse syncUse = SyncUse::of(use.access, use.shaderStages ? use.shaderStages : defaultStages,
                                              queue, use.discard);
                if (use.image) {
                    applyImageUse(use.index, syncUse, barriers);
                } else {
                    applyBufferUse(use.index, syncUse, barriers);
                }
            }
            recordBarriers(barriers);

            RecordContext recordContext{frameContext, commandBuffer, {0, 0}};
            if (record.node->kind() == NodeKind::Graphics) {
                recordContext.renderExtent = beginRendering(commandBuffer, *record.target, frame, acquired);
                record.node->record(recordContext);
                vkCmdEndRendering(commandBuffer);
            } else {
                record.node->record(recordContext);
            }
        }

        // Return imported images to their resting states, and the surface for presentation
        Barriers finalBarriers;
        for (uint32_t resource : submission.finalTransitions) {
            const auto& image = *images_[resource];
            SyncUse rest;
            rest.queue = queue;
            rest.stages = image.resting.stages;
            rest.access = image.resting.access;
            rest.layout = image.resting.layout;
            rest.read = true;
            applyImageUse(resource, rest, finalBarriers);
        }
        if (submission.presentsSurface) {
            SyncUse present;
            present.queue = queue;
            present.stages = VK_PIPELINE_STAGE_2_NONE;
            present.access = VK_ACCESS_2_NONE;
            present.layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            present.read = true;
            applyImageUse(surfaceImage_.index, present, finalBarriers);
        }
        recordBarriers(finalBarriers);

        VK_SUCCESS_OR_THROW(vkEndCommandBuffer(commandBuffer), "Failed to record command buffer");

        VulkanContext::SubmitBatch batch;
        VkCommandBufferSubmitInfo commandBufferInfo{};
        commandBufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        commandBufferInfo.commandBuffer = commandBuffer;
        batch.commandBuffers.push_back(commandBufferInfo);
        for (const auto& [waitQueue, wait] : waits) {
            VkSemaphoreSubmitInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            info.semaphore = context_->timelineSemaphore(waitQueue);
            info.value = wait.first;
            info.stageMask = wait.second ? wait.second : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            batch.waits.push_back(info);
        }
        if (submission.acquiresSurface) {
            VkSemaphoreSubmitInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            info.semaphore = acquired.semaphore;
            info.stageMask = acquireStages ? acquireStages : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            batch.waits.push_back(info);
        }
        if (submission.presentsSurface) {
            VkSemaphoreSubmitInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            info.semaphore = swapchain_->getRenderFinishedSemaphore(acquired.imageIndex);
            info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            batch.signals.push_back(info);
        }
        const uint64_t value = context_->submit(queue, batch);
        submittedPoints_[queue] = value;
        for (auto* state : touched) {
            state->resolvePending(queue, value);
        }
        return value;
    }

    VkImageView attachmentView(ImageId id, uint32_t frame, const Swapchain::Acquired& acquired) {
        if (images_[id.index]->kind == ImageKind::Surface) {
            return swapchain_->getImageView(acquired.imageIndex);
        }
        return image(id, frame)->getImageView();
    }

    // Begins dynamic rendering into target, with a full-target viewport and scissor
    VkExtent2D beginRendering(VkCommandBuffer commandBuffer, const RenderTarget& target, uint32_t frame,
                              const Swapchain::Acquired& acquired) {
        std::vector<VkRenderingAttachmentInfo> colors;
        VkExtent2D extent{0, 0};
        for (const auto& attachment : target.color) {
            VkRenderingAttachmentInfo info{};
            info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            info.imageView = attachmentView(attachment.image, frame, acquired);
            info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            info.loadOp = attachment.load;
            info.storeOp = attachment.store;
            info.clearValue.color = {{attachment.clear[0], attachment.clear[1], attachment.clear[2], attachment.clear[3]}};
            colors.push_back(info);
            extent = imageExtent(attachment.image, frame);
        }
        VkRenderingAttachmentInfo depth{};
        if (target.depth) {
            depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depth.imageView = attachmentView(target.depth->image, frame, acquired);
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth.loadOp = target.depth->load;
            depth.storeOp = target.depth->store;
            depth.clearValue.depthStencil = {target.depth->clear, 0};
            if (colors.empty()) {
                extent = imageExtent(target.depth->image, frame);
            }
        }

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea = {{0, 0}, extent};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colors.size());
        renderingInfo.pColorAttachments = colors.data();
        renderingInfo.pDepthAttachment = target.depth ? &depth : nullptr;
        vkCmdBeginRendering(commandBuffer, &renderingInfo);

        VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        return extent;
    }

    VulkanContext* context_;
    Swapchain* swapchain_;
    ImageId surfaceImage_;
    ResourceSyncState surfaceState_ = ResourceSyncState::forImage();
    uint32_t acquiredImage_ = 0;
    VkFormat surfaceFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D surfaceExtent_{0, 0};
    VkExtent2D realizedSurfaceExtent_{0, 0};
    std::map<VkFormat, ImageId> surfaceDepth_;

    std::vector<NodeRecord> nodes_;
    std::vector<std::unique_ptr<ImageRecord>> images_;
    std::vector<std::unique_ptr<BufferRecord>> buffers_;
    std::vector<std::pair<uint32_t, uint32_t>> dependencies_;
    std::vector<GraphEntries> graphEntries_;
    // Resources imported automatically for materials' per-frame entries, by entry collection
    std::map<const void*, std::pair<bool, uint32_t>> entryImports_;

    bool structureDirty_ = true;
    std::map<std::vector<bool>, CompiledGraph> plans_;
    GraphDesc lastDesc_;
    uint64_t compileCount_ = 0;
    std::function<void(const std::string&)> warningHandler_;

    std::optional<uint64_t> lastFrameNumber_;
    VulkanContext::TimelinePoints submittedPoints_;
    PerFrame<FrameSlot> slots_;
};

// NodeBuilder's methods, which need RenderGraph

inline NodeBuilder& NodeBuilder::read(ImageId image, Access access, VkPipelineStageFlags2 shaderStages) {
    if (accessInfo(access, 0).write) {
        throw GraphError(std::string("read() with writing access ") + accessName(access) + "; use write()");
    }
    graph_->imageRecord(image);
    graph_->nodes_[node_].uses.push_back({true, image.index, access, shaderStages, false});
    graph_->structureChanged();
    return *this;
}

inline NodeBuilder& NodeBuilder::write(ImageId image, Access access, VkPipelineStageFlags2 shaderStages) {
    if (!accessInfo(access, 0).write) {
        throw GraphError(std::string("write() with read-only access ") + accessName(access) + "; use read()");
    }
    graph_->imageRecord(image);
    graph_->nodes_[node_].uses.push_back({true, image.index, access, shaderStages, false});
    graph_->structureChanged();
    return *this;
}

inline NodeBuilder& NodeBuilder::read(BufferId buffer, Access access, VkPipelineStageFlags2 shaderStages) {
    if (accessInfo(access, 0).write) {
        throw GraphError(std::string("read() with writing access ") + accessName(access) + "; use write()");
    }
    graph_->bufferRecord(buffer);
    graph_->nodes_[node_].uses.push_back({false, buffer.index, access, shaderStages, false});
    graph_->structureChanged();
    return *this;
}

inline NodeBuilder& NodeBuilder::write(BufferId buffer, Access access, VkPipelineStageFlags2 shaderStages) {
    if (!accessInfo(access, 0).write) {
        throw GraphError(std::string("write() with read-only access ") + accessName(access) + "; use read()");
    }
    graph_->bufferRecord(buffer);
    graph_->nodes_[node_].uses.push_back({false, buffer.index, access, shaderStages, false});
    graph_->structureChanged();
    return *this;
}

inline NodeBuilder& NodeBuilder::renderTo(const RenderTarget& target) {
    auto& record = graph_->nodes_[node_];
    if (record.node->kind() != NodeKind::Graphics) {
        throw GraphError("Only graphics nodes render to targets ('" + record.name + "')");
    }
    if (record.target) {
        throw GraphError("Node '" + record.name + "' already has a render target");
    }
    record.target = target;
    for (const auto& attachment : target.color) {
        graph_->imageRecord(attachment.image);
        const bool discard = attachment.load != VK_ATTACHMENT_LOAD_OP_LOAD;
        record.uses.push_back({true, attachment.image.index, Access::ColorAttachment, 0, discard});
    }
    if (target.depth) {
        graph_->imageRecord(target.depth->image);
        const bool discard = target.depth->load != VK_ATTACHMENT_LOAD_OP_LOAD;
        record.uses.push_back({true, target.depth->image.index, Access::DepthAttachment, 0, discard});
    }
    graph_->structureChanged();
    return *this;
}

inline NodeBuilder& NodeBuilder::uses(Handle<Material> material) {
    auto& record = graph_->nodes_[node_];
    if (std::find(record.materials.begin(), record.materials.end(), material) != record.materials.end()) {
        return *this;
    }
    // The material's resources, as its bindings declare them
    const auto& bindings = material->bindings();
    for (size_t i = 0; i < bindings.size(); ++i) {
        auto resource = graph_->resolveBinding(bindings[i], record.name, i);
        if (!resource) {
            continue;
        }
        const VkPipelineStageFlags2 stages = bindings[i].stages ? bindings[i].stages : material->defaultStages();
        graph_->nodes_[node_].uses.push_back({resource->first, resource->second, bindings[i].access, stages, false});
    }
    graph_->nodes_[node_].materials.push_back(std::move(material));
    graph_->structureChanged();
    return *this;
}

inline NodeBuilder& NodeBuilder::onPrimaryQueue() {
    graph_->nodes_[node_].pinToPrimaryQueue = true;
    graph_->structureChanged();
    return *this;
}

inline NodeBuilder& NodeBuilder::after(NodeId node) {
    graph_->addDependency(node, {node_});
    return *this;
}
