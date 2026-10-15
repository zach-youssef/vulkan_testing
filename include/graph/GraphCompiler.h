#pragma once

#include "graph/GraphTypes.h"

#include <algorithm>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <vector>

/*
 The render graph compiler: a pure function from a description of nodes, resource uses and queues
 to a plan of what runs where and in which order. It touches no Vulkan objects, so it is tested on
 the CPU.

 Compiling:
    1. Drops disabled nodes.
    2. Derives dependencies from resource uses in insertion order (read after write, write after
       read, write after write), adds explicit dependencies, and sorts topologically (ties keep
       insertion order). Cycles, which only explicit dependencies can create, throw.
    3. Checks the surface: at most one writer, and no reads before it.
    4. Assigns queues. Graphics nodes, nodes using the surface and pinned nodes use the primary
       queue (0). Compute and transfer nodes go to the queue whose latest node is an ancestor (no
       parallelism lost), else an unused queue (independent work runs in parallel), else the
       queue with the fewest nodes.
    5. Groups nodes into submissions: a node joins its queue's open submission unless it depends on
       a submission or CPU node created after it. Submissions therefore only depend on earlier
       ones and are issued in creation order.
    6. Places the surface's acquire wait and present, and imported images' final transitions,
       in the submissions holding their first and last uses.

 Barriers are not part of the plan: the runtime derives them each frame from tracked resource
 state (see GraphSync.h).
 */

struct CompilerUse {
    uint32_t resource;
    Access access;
    VkPipelineStageFlags2 shaderStages = 0;   // 0: the node kind's default
};

struct CompilerNode {
    std::string name;
    NodeKind kind;
    bool enabled = true;
    bool pinToPrimaryQueue = false;
    std::vector<CompilerUse> uses;
};

struct CompilerResource {
    std::string name;
    bool image = true;
    bool surface = false;
    bool perFrame = false;
    // Imported images return to their resting state at the end of every frame
    bool returnsToRestingState = false;
};

struct CompilerQueue {
    VkQueueFlags flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
};

struct GraphDesc {
    std::vector<CompilerNode> nodes;
    std::vector<CompilerResource> resources;
    std::vector<std::pair<uint32_t, uint32_t>> dependencies;   // {before, after}
    std::vector<CompilerQueue> queues = {CompilerQueue{}};      // Index 0 is the primary queue
};

struct CompiledSubmission {
    uint32_t queue = 0;
    uint32_t step = 0;                      // Position in CompiledGraph::steps
    std::vector<uint32_t> nodes;            // In recording order
    // Imported images whose last use is in this submission; they return to their resting state
    // at its end
    std::vector<uint32_t> finalTransitions;
    bool acquiresSurface = false;           // Waits for the acquired surface image
    bool presentsSurface = false;           // Transitions the surface for presentation and signals its present
};

struct CompiledStep {
    bool cpu = false;
    uint32_t index = 0;                     // Submission index, or node index for CPU steps
};

struct CompiledGraph {
    std::vector<uint32_t> order;                         // Enabled nodes, sorted
    std::vector<int32_t> nodeQueue;                      // Per node; -1 for CPU and disabled nodes
    std::vector<int32_t> nodeSubmission;                 // Per node; -1 for CPU and disabled nodes
    std::vector<std::vector<uint32_t>> predecessors;     // Per node: direct dependencies (enabled)
    std::vector<std::vector<uint32_t>> explicitPredecessors; // Per node: explicit dependencies (enabled)
    std::vector<CompiledSubmission> submissions;
    std::vector<CompiledStep> steps;                     // In execution order
    bool usesSurface = false;
    std::vector<std::string> warnings;

    // Human readable summary, for debugging
    std::string describe(const GraphDesc& desc) const {
        std::ostringstream out;
        for (const auto& step : steps) {
            if (step.cpu) {
                out << "cpu: " << desc.nodes[step.index].name << "\n";
                continue;
            }
            const auto& submission = submissions[step.index];
            out << "submission " << step.index << " (queue " << submission.queue << "):";
            for (uint32_t node : submission.nodes) {
                out << " " << desc.nodes[node].name;
            }
            if (submission.acquiresSurface) out << " [acquire]";
            if (submission.presentsSurface) out << " [present]";
            out << "\n";
        }
        return out.str();
    }
};

namespace detail {

inline bool usesWrite(const CompilerUse& use) {
    return accessInfo(use.access, 0).write;
}

} // namespace detail

inline CompiledGraph compileGraph(const GraphDesc& desc) {
    const uint32_t nodeCount = static_cast<uint32_t>(desc.nodes.size());
    CompiledGraph compiled;
    compiled.nodeQueue.assign(nodeCount, -1);
    compiled.nodeSubmission.assign(nodeCount, -1);
    compiled.predecessors.resize(nodeCount);
    compiled.explicitPredecessors.resize(nodeCount);

    if (desc.queues.empty()) {
        throw GraphError("A graph needs at least one queue");
    }
    for (const auto& node : desc.nodes) {
        for (const auto& use : node.uses) {
            if (use.resource >= desc.resources.size()) {
                throw GraphError("Node '" + node.name + "' uses an unknown resource");
            }
            const auto& resource = desc.resources[use.resource];
            if (resource.image && !isImageAccess(use.access)) {
                throw GraphError("Node '" + node.name + "' uses image '" + resource.name + "' with buffer access "
                                 + accessName(use.access));
            }
            if (!resource.image && !isBufferAccess(use.access)) {
                throw GraphError("Node '" + node.name + "' uses buffer '" + resource.name + "' with image access "
                                 + accessName(use.access));
            }
            if (node.kind == NodeKind::Cpu && use.access != Access::HostRead && use.access != Access::HostWrite) {
                throw GraphError("CPU node '" + node.name + "' may only declare host accesses");
            }
            if (node.kind != NodeKind::Cpu && (use.access == Access::HostRead || use.access == Access::HostWrite)) {
                throw GraphError("Node '" + node.name + "' declares a host access but is not a CPU node");
            }
        }
    }

    // 1. & 2. Dependencies between enabled nodes
    std::vector<std::set<uint32_t>> edges(nodeCount);   // edges[before] contains after
    auto addEdge = [&](uint32_t before, uint32_t after) {
        if (before != after) {
            edges[before].insert(after);
        }
    };
    struct Tracking {
        std::optional<uint32_t> lastWriter;
        std::vector<uint32_t> readers;   // Since the last write
    };
    std::vector<Tracking> tracking(desc.resources.size());
    for (uint32_t node = 0; node < nodeCount; ++node) {
        if (!desc.nodes[node].enabled) {
            continue;
        }
        // A node's own uses don't depend on each other: gather what it reads and writes first
        std::set<uint32_t> reads, writes;
        for (const auto& use : desc.nodes[node].uses) {
            (detail::usesWrite(use) ? writes : reads).insert(use.resource);
            if (accessInfo(use.access, 0).read) {
                reads.insert(use.resource);
            }
        }
        for (uint32_t resource : reads) {
            if (tracking[resource].lastWriter) {
                addEdge(*tracking[resource].lastWriter, node);   // Read after write
            }
        }
        for (uint32_t resource : writes) {
            for (uint32_t reader : tracking[resource].readers) {
                addEdge(reader, node);                           // Write after read
            }
            if (tracking[resource].lastWriter) {
                addEdge(*tracking[resource].lastWriter, node);   // Write after write
            }
        }
        for (uint32_t resource : reads) {
            if (!writes.count(resource)) {
                tracking[resource].readers.push_back(node);
            }
        }
        for (uint32_t resource : writes) {
            tracking[resource].lastWriter = node;
            tracking[resource].readers.clear();
        }
    }
    for (auto [before, after] : desc.dependencies) {
        if (before >= nodeCount || after >= nodeCount) {
            throw GraphError("Dependency on an unknown node");
        }
        if (desc.nodes[before].enabled && desc.nodes[after].enabled) {
            if (before == after) {
                throw GraphError("Node '" + desc.nodes[before].name + "' depends on itself");
            }
            addEdge(before, after);
            compiled.explicitPredecessors[after].push_back(before);
        }
    }

    // Kahn's algorithm, ties broken by insertion order
    std::vector<uint32_t> incoming(nodeCount, 0);
    for (uint32_t node = 0; node < nodeCount; ++node) {
        for (uint32_t after : edges[node]) {
            ++incoming[after];
            compiled.predecessors[after].push_back(node);
        }
    }
    std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<>> ready;
    uint32_t enabledCount = 0;
    for (uint32_t node = 0; node < nodeCount; ++node) {
        if (desc.nodes[node].enabled) {
            ++enabledCount;
            if (incoming[node] == 0) {
                ready.push(node);
            }
        }
    }
    while (!ready.empty()) {
        uint32_t node = ready.top();
        ready.pop();
        compiled.order.push_back(node);
        for (uint32_t after : edges[node]) {
            if (--incoming[after] == 0) {
                ready.push(after);
            }
        }
    }
    if (compiled.order.size() != enabledCount) {
        std::string names;
        for (uint32_t node = 0; node < nodeCount; ++node) {
            if (desc.nodes[node].enabled && incoming[node] > 0) {
                names += (names.empty() ? "" : ", ") + desc.nodes[node].name;
            }
        }
        throw GraphError("The graph's dependencies form a cycle through: " + names);
    }
    std::vector<uint32_t> position(nodeCount, UINT32_MAX);
    for (uint32_t i = 0; i < compiled.order.size(); ++i) {
        position[compiled.order[i]] = i;
    }

    // 3. Surface checks
    std::vector<bool> usesSurface(nodeCount, false);
    {
        std::vector<uint32_t> writers;
        std::optional<uint32_t> firstReader;
        for (uint32_t node : compiled.order) {
            for (const auto& use : desc.nodes[node].uses) {
                if (!desc.resources[use.resource].surface) {
                    continue;
                }
                usesSurface[node] = true;
                if (detail::usesWrite(use)) {
                    if (std::find(writers.begin(), writers.end(), node) == writers.end()) {
                        writers.push_back(node);
                    }
                } else if (!firstReader && writers.empty()) {
                    firstReader = node;
                }
            }
        }
        if (writers.size() > 1) {
            throw GraphError("More than one node writes the surface: " + desc.nodes[writers[0]].name + ", "
                             + desc.nodes[writers[1]].name);
        }
        if (firstReader) {
            throw GraphError("Node '" + desc.nodes[*firstReader].name + "' reads the surface before it is written");
        }
        compiled.usesSurface = !writers.empty();
    }

    // 4. Queue assignment
    const uint32_t queueCount = static_cast<uint32_t>(desc.queues.size());
    std::vector<std::vector<bool>> ancestors(nodeCount, std::vector<bool>(nodeCount, false));
    for (uint32_t node : compiled.order) {
        for (uint32_t predecessor : compiled.predecessors[node]) {
            ancestors[node][predecessor] = true;
            for (uint32_t i = 0; i < nodeCount; ++i) {
                if (ancestors[predecessor][i]) {
                    ancestors[node][i] = true;
                }
            }
        }
    }
    std::vector<std::optional<uint32_t>> lastOnQueue(queueCount);
    std::vector<uint32_t> nodesOnQueue(queueCount, 0);
    for (uint32_t node : compiled.order) {
        const auto& info = desc.nodes[node];
        if (info.kind == NodeKind::Cpu) {
            continue;
        }
        const VkQueueFlags needed = info.kind == NodeKind::Graphics ? VK_QUEUE_GRAPHICS_BIT
                                    : info.kind == NodeKind::Compute ? VK_QUEUE_COMPUTE_BIT
                                    : VkQueueFlags(0);
        std::optional<uint32_t> chosen;
        if (info.kind == NodeKind::Graphics || info.pinToPrimaryQueue || usesSurface[node] || queueCount == 1) {
            chosen = 0;
        } else {
            auto allowed = [&](uint32_t queue) { return (desc.queues[queue].flags & needed) == needed; };
            // A queue whose latest node is an ancestor: continuing there loses no parallelism
            for (uint32_t queue = 0; queue < queueCount; ++queue) {
                if (allowed(queue) && lastOnQueue[queue] && ancestors[node][*lastOnQueue[queue]]
                    && (!chosen || position[*lastOnQueue[queue]] > position[*lastOnQueue[*chosen]])) {
                    chosen = queue;
                }
            }
            // An unused queue other than the primary: independent work runs in parallel
            for (uint32_t queue = 1; queue < queueCount && !chosen; ++queue) {
                if (allowed(queue) && !lastOnQueue[queue]) {
                    chosen = queue;
                }
            }
            // The least used queue, preferring queues other than the primary
            for (uint32_t i = 0; i < queueCount && !chosen; ++i) {
                uint32_t best = UINT32_MAX;
                for (uint32_t offset = 1; offset <= queueCount; ++offset) {
                    uint32_t queue = offset % queueCount;
                    if (allowed(queue) && (best == UINT32_MAX || nodesOnQueue[queue] < nodesOnQueue[best])) {
                        best = queue;
                    }
                }
                if (best != UINT32_MAX) {
                    chosen = best;
                }
            }
            if (!chosen) {
                throw GraphError("No queue can run node '" + info.name + "'");
            }
        }
        compiled.nodeQueue[node] = static_cast<int32_t>(*chosen);
        lastOnQueue[*chosen] = node;
        ++nodesOnQueue[*chosen];
    }

    // 5. Submissions and CPU steps
    std::vector<int32_t> nodeStep(nodeCount, -1);
    std::vector<int32_t> openSubmission(queueCount, -1);
    for (uint32_t node : compiled.order) {
        int32_t latestDependency = -1;
        for (uint32_t predecessor : compiled.predecessors[node]) {
            latestDependency = std::max(latestDependency, nodeStep[predecessor]);
        }
        if (desc.nodes[node].kind == NodeKind::Cpu) {
            nodeStep[node] = static_cast<int32_t>(compiled.steps.size());
            compiled.steps.push_back({true, node});
            continue;
        }
        const uint32_t queue = static_cast<uint32_t>(compiled.nodeQueue[node]);
        int32_t submission = openSubmission[queue];
        if (submission < 0 || latestDependency > static_cast<int32_t>(compiled.submissions[submission].step)) {
            submission = static_cast<int32_t>(compiled.submissions.size());
            CompiledSubmission created;
            created.queue = queue;
            created.step = static_cast<uint32_t>(compiled.steps.size());
            compiled.submissions.push_back(created);
            compiled.steps.push_back({false, static_cast<uint32_t>(submission)});
            openSubmission[queue] = submission;
        }
        compiled.submissions[submission].nodes.push_back(node);
        compiled.nodeSubmission[node] = submission;
        nodeStep[node] = static_cast<int32_t>(compiled.submissions[submission].step);
    }

    // 6. Surface and final transitions: the submission (in execution order) holding the first and
    // last uses. Final transitions go in the last created submission using the resource, so any
    // waits they need are on earlier submissions.
    std::vector<int32_t> lastSubmission(desc.resources.size(), -1);
    std::optional<uint32_t> firstSurfaceSubmission;
    for (const auto& step : compiled.steps) {
        if (step.cpu) {
            continue;
        }
        const auto& submission = compiled.submissions[step.index];
        for (uint32_t node : submission.nodes) {
            for (const auto& use : desc.nodes[node].uses) {
                lastSubmission[use.resource] = static_cast<int32_t>(step.index);
                if (desc.resources[use.resource].surface && !firstSurfaceSubmission) {
                    firstSurfaceSubmission = step.index;
                }
            }
        }
    }
    for (uint32_t resource = 0; resource < desc.resources.size(); ++resource) {
        if (lastSubmission[resource] < 0) {
            continue;
        }
        if (desc.resources[resource].surface) {
            if (compiled.usesSurface) {
                compiled.submissions[lastSubmission[resource]].presentsSurface = true;
            }
        } else if (desc.resources[resource].returnsToRestingState) {
            compiled.submissions[lastSubmission[resource]].finalTransitions.push_back(resource);
        }
    }
    if (compiled.usesSurface && firstSurfaceSubmission) {
        compiled.submissions[*firstSurfaceSubmission].acquiresSurface = true;
    }

    // Warnings: host writes to shared resources the GPU reads may race with earlier frames still running
    for (uint32_t node : compiled.order) {
        if (desc.nodes[node].kind != NodeKind::Cpu) {
            continue;
        }
        for (const auto& use : desc.nodes[node].uses) {
            if (use.access == Access::HostWrite && !desc.resources[use.resource].perFrame) {
                for (uint32_t other : compiled.order) {
                    if (desc.nodes[other].kind == NodeKind::Cpu) {
                        continue;
                    }
                    for (const auto& otherUse : desc.nodes[other].uses) {
                        if (otherUse.resource == use.resource) {
                            compiled.warnings.push_back("CPU node '" + desc.nodes[node].name + "' writes '"
                                                        + desc.resources[use.resource].name
                                                        + "', which the GPU reads and is shared between frames: the "
                                                          "host may overwrite data an earlier frame is still using");
                            goto nextUse;
                        }
                    }
                }
            }
        nextUse:;
        }
    }

    return compiled;
}
