#pragma once

#include "graph/GraphTypes.h"
#include "targets/RenderTarget.h"
#include "core/ResourceManager.h"

#include <functional>
#include <memory>
#include <string>

class RenderGraph;
class VulkanContext;
class Material;

/*
 Nodes are the units of work in a RenderGraph.

    setup(NodeBuilder&)      once, when the node is added: declares the resources it uses (and how),
                             its render target, the materials it draws with and explicit dependencies
    record(RecordContext&)   every frame, for GPU nodes: records commands. Graphics nodes record
                             inside dynamic rendering, with the viewport and scissor already set.
    run(FrameContext&)       every frame, for CPU nodes: host work, in graph order

 The graph derives ordering, queues, barriers and semaphores from the declarations, so a node must
 declare every resource it touches (until step 6, including those its materials' descriptors
 reference; add them with graph.node(id).read(...)).
 */

struct FrameContext {
    VulkanContext& context;
    RenderGraph& graph;
    uint32_t frameIndex;        // context.currentFrame(): indexes PerFrame collections
    uint64_t frameNumber;       // context.frameNumber() when the frame started
    VkExtent2D surfaceExtent;   // RenderGraph::surfaceExtent(): {0, 0} without a surface or one set headless
};

struct RecordContext : FrameContext {
    VkCommandBuffer commandBuffer;
    VkExtent2D renderExtent;    // Graphics nodes: the render target's extent
};

class NodeBuilder {
public:
    NodeBuilder(RenderGraph& graph, uint32_t node) : graph_(&graph), node_(node) {}

    // Declares a use of a resource. shaderStages applies to shader accesses (sampled, storage,
    // uniform); 0 means the node kind's default (compute shader, or vertex and fragment shaders).
    // read() requires an access that doesn't write, write() one that does.
    NodeBuilder& read(ImageId image, Access access, VkPipelineStageFlags2 shaderStages = 0);
    NodeBuilder& write(ImageId image, Access access, VkPipelineStageFlags2 shaderStages = 0);
    NodeBuilder& read(BufferId buffer, Access access, VkPipelineStageFlags2 shaderStages = 0);
    NodeBuilder& write(BufferId buffer, Access access, VkPipelineStageFlags2 shaderStages = 0);

    // Graphics nodes: the attachments to render to. Declares their uses.
    NodeBuilder& renderTo(const RenderTarget& target);

    // A material the node draws or dispatches with. The graph calls its update() once per frame,
    // however many nodes use it, and checks graphics materials match the render target's formats.
    NodeBuilder& uses(Handle<Material> material);

    // Runs a compute or transfer node on the primary queue instead of letting the graph choose
    NodeBuilder& onPrimaryQueue();

    // An explicit dependency, for ordering the graph can't see from resource uses
    NodeBuilder& after(NodeId node);

    RenderGraph& graph() { return *graph_; }
    NodeId id() const { return {node_}; }

private:
    RenderGraph* graph_;
    uint32_t node_;
};

class Node {
public:
    virtual ~Node() = default;
    virtual NodeKind kind() const = 0;
    virtual void setup(NodeBuilder&) {}
    virtual void record(RecordContext&) {}
    virtual void run(FrameContext&) {}
};

class GraphicsNode : public Node {
public:
    NodeKind kind() const final { return NodeKind::Graphics; }
};

class ComputeNode : public Node {
public:
    NodeKind kind() const final { return NodeKind::Compute; }
};

class TransferNode : public Node {
public:
    NodeKind kind() const final { return NodeKind::Transfer; }
};

class CpuNode : public Node {
public:
    NodeKind kind() const final { return NodeKind::Cpu; }
};

// A node defined by functions, for small one-off nodes (see RenderGraph::addGraphics etc.)
class LambdaNode : public Node {
public:
    using SetupFn = std::function<void(NodeBuilder&)>;
    using RecordFn = std::function<void(RecordContext&)>;
    using RunFn = std::function<void(FrameContext&)>;

    LambdaNode(NodeKind kind, SetupFn setup, RecordFn record, RunFn run)
    : kind_(kind), setup_(std::move(setup)), record_(std::move(record)), run_(std::move(run)) {}

    NodeKind kind() const override { return kind_; }
    void setup(NodeBuilder& builder) override { if (setup_) setup_(builder); }
    void record(RecordContext& context) override { if (record_) record_(context); }
    void run(FrameContext& context) override { if (run_) run_(context); }

private:
    NodeKind kind_;
    SetupFn setup_;
    RecordFn record_;
    RunFn run_;
};
