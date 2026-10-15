#pragma once

#include "core/VkTypes.h"
#include "core/VkUtil.h"
#include "core/VulkanContext.h"

#include <unordered_set>

enum NodeDevice {
    GPU,
    CPU
};

struct RenderEvalContext {
    const uint32_t frameIndex;
    const VkExtent2D swapchainExtent;
    uint32_t imageIndex;
    std::vector<VkFramebuffer> frameBuffers;
    VkSwapchainKHR swapChain;
    bool shouldRecreateSwapChain;
    // One per swapchain image, signaled by the node whose work is presented and waited on by the
    // present. They are per image rather than per frame: the presentation engine may still be
    // using an image's semaphore until that image is acquired again.
    std::vector<VkSemaphore> renderFinishedSemaphores = {};
};

class RenderGraph;

class RenderNode {
    friend class RenderGraph;
public:
    virtual ~RenderNode() = default;
    
    virtual void submit(RenderEvalContext& ctx) = 0;

protected:
    virtual NodeDevice getDeviceType() = 0;

    // Whether this node presents the swapchain image. Its parents then signal the acquired
    // image's render finished semaphore for it, rather than per-frame semaphores.
    virtual bool presentsImage() {
        return false;
    }

    RenderNode(VulkanContext& context) : context_(&context) {}
    
    void addSemaphoreEdgeTo(RenderNode* other){
        if (other->presentsImage()) {
            signalsRenderFinished_ = true;
            other->waitsForRenderFinished_ = true;
            other->parents_.push_back(this);
            children_.push_back(other);
            return;
        }
        
        // If this is our first outgoing per-frame semaphore edge, create our semaphores
        if (signalSemaphores_.empty()) {
            signalSemaphores_ = PerFrame<std::unique_ptr<VulkanSemaphore>>(*context_, [device = device()](uint32_t) {
                VkSemaphoreCreateInfo semaphoreInfo{};
                semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                std::unique_ptr<VulkanSemaphore> semaphore;
                VK_SUCCESS_OR_THROW(VulkanSemaphore::create(semaphore, device, semaphoreInfo),
                                    "Failed to create semaphore");
                return semaphore;
            });
        }
        
        other->semaphoreParents_.push_back(this);
        other->parents_.push_back(this);
        children_.push_back(other);
    }
    
    void addFenceEdgeTo(RenderNode* other, bool createSignaled = false) {
        // If this is our first outgoing edge, create our fences
        if (signalFences_.empty()) {
            signalFences_ = PerFrame<std::unique_ptr<VulkanFence>>(*context_, [device = device(), createSignaled](uint32_t) {
                VkFenceCreateInfo fenceInfo{};
                fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                fenceInfo.flags = createSignaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0;
                std::unique_ptr<VulkanFence> fence;
                VK_SUCCESS_OR_THROW(VulkanFence::create(fence, device, fenceInfo),
                                    "Failed to create fence");
                return fence;
            });
        }
        
        other->fenceParents_.push_back(this);
        other->parents_.push_back(this);
        children_.push_back(other);
    }
    
    // Semaphores signaled for this frame by the nodes this node waits on
    std::vector<VkSemaphore> getWaitSemaphores(const RenderEvalContext& ctx) {
        std::vector<VkSemaphore> semaphores;
        for (auto* parent : semaphoreParents_) {
            semaphores.push_back(**parent->signalSemaphores_[ctx.frameIndex]);
        }
        if (waitsForRenderFinished_) {
            semaphores.push_back(ctx.renderFinishedSemaphores.at(ctx.imageIndex));
        }
        return semaphores;
    }
    
    // Fences signaled for this frame by the nodes this node waits on
    std::vector<VkFence> getWaitFences(uint32_t frameIndex) {
        std::vector<VkFence> fences;
        for (auto* parent : fenceParents_) {
            fences.push_back(**parent->signalFences_[frameIndex]);
        }
        return fences;
    }
    
    // Semaphores to signal when this node's work completes, for use in VkSubmitInfo.
    // Empty if the node has no outgoing semaphore edges.
    std::vector<VkSemaphore> getSignalSemaphores(const RenderEvalContext& ctx) {
        std::vector<VkSemaphore> semaphores;
        if (!signalSemaphores_.empty()) {
            semaphores.push_back(getFrameSignalSemaphore(ctx.frameIndex));
        }
        if (signalsRenderFinished_) {
            semaphores.push_back(ctx.renderFinishedSemaphores.at(ctx.imageIndex));
        }
        return semaphores;
    }
    
    // This node's per-frame signal semaphore, which children other than present nodes wait on
    VkSemaphore getFrameSignalSemaphore(uint32_t frameIndex) {
        return **signalSemaphores_[frameIndex];
    }
    
    // Fence to signal when this node's work completes, or VK_NULL_HANDLE if no node waits on it
    VkFence getSignalFence(uint32_t frameIndex) {
        return signalFences_.empty() ? VK_NULL_HANDLE : **signalFences_[frameIndex];
    }
    
    const std::vector<RenderNode*> getChildren() {
        return children_;
    }

    bool allParentsVisited(const std::unordered_set<RenderNode*>& visited) {
        for (auto parent : parents_) {
            if (visited.find(parent) == visited.end()) {
                return false;
            }
        }
        return true;
    }
    
protected:
    VkDevice device() {
        return context_->device();
    }
    
    // A command buffer per frame in flight, allocated from pool (which must outlive them)
    PerFrame<VkCommandBuffer> allocateCommandBuffers(CommandPool& pool) {
        return PerFrame<VkCommandBuffer>(*context_,
                                         [&pool](uint32_t) { return pool.allocateOne(); },
                                         [&pool](VkCommandBuffer& commandBuffer) { pool.free(commandBuffer); });
    }
    
    VulkanContext* context_;
    std::vector<RenderNode*> children_;

private:
    // Created on the first outgoing edge of each kind, with one per frame in flight
    PerFrame<std::unique_ptr<VulkanSemaphore>> signalSemaphores_;
    PerFrame<std::unique_ptr<VulkanFence>> signalFences_;
    std::vector<RenderNode*> semaphoreParents_;
    std::vector<RenderNode*> fenceParents_;
    bool signalsRenderFinished_ = false;
    bool waitsForRenderFinished_ = false;
    std::vector<RenderNode*> parents_;
};

class RenderGraph : public RenderNode {
public:
    using NodeHandle = uint32_t;
    
    RenderGraph(VulkanContext& context): RenderNode(context) {}
    
    NodeDevice getDeviceType() override {
        return NodeDevice::CPU;
    }
    
    NodeHandle addNode(std::unique_ptr<RenderNode>&& node) {
        nodes_.emplace_back(std::move(node));
        startNodes_.emplace_back(true);
        return static_cast<NodeHandle>(nodes_.size() - 1);
    }
    
    int addEdge(NodeHandle from, NodeHandle to) {
        startNodes_[to] = false;
        if (nodes_.at(from)->getDeviceType() == NodeDevice::GPU) {
            if (nodes_.at(to)->getDeviceType() == NodeDevice::GPU) {
                // Semaphore case
                nodes_.at(from)->addSemaphoreEdgeTo(nodes_.at(to).get());
            } else if (nodes_.at(to)->getDeviceType() == NodeDevice::CPU) {
                // Fence case
                nodes_.at(from)->addFenceEdgeTo(nodes_.at(to).get());
            }
        } else if (nodes_.at(from)->getDeviceType() == NodeDevice::CPU) {
            if (nodes_.at(to)->getDeviceType() == NodeDevice::GPU) {
                // TODO CPU -> GPU ???
                return EXIT_FAILURE;
            } else if (nodes_.at(to)->getDeviceType() == NodeDevice::CPU) {
                // TODO CPU -> CPU ??
                return EXIT_FAILURE;
            }
        }
        return 0;
    }
    
    // Marks a node as needing to be completed before
    // starting the next frame
    int flagNodeAsFrameBlocking(NodeHandle node) {
        if (nodes_[node]->getDeviceType() == NodeDevice::GPU) {
            nodes_[node]->addFenceEdgeTo(this, true /* createSignaled */);
        } else {
            // CPU -> CPU sync not supported yet
            return EXIT_FAILURE;
        }
        
        return 0;
    }
    
    void waitUntilComplete(uint32_t frameIndex) {
        // Wait for the previous frame to finish
        for (VkFence fence : getWaitFences(frameIndex)) {
            vkWaitForFences(device(), 1, &fence, VK_TRUE, UINT64_MAX);
        }
    }
    
    void submit(RenderEvalContext& ctx) override {
        // Reset our fences
        for (VkFence fence : getWaitFences(ctx.frameIndex)) {
            vkResetFences(device(), 1, &fence);
        }
        
        // Initialize the queue with all nodes without an incoming edge
        std::queue<RenderNode*> nodeQueue;
        for (uint idx = 0; idx < nodes_.size(); ++idx) {
            if (startNodes_[idx]) {
                nodeQueue.push(nodes_.at(idx).get());
            }
        }
        // Keep track of nodes that have been visited
        std::unordered_set<RenderNode*> visited;
        visited.emplace(this);

        // Traverse the graph submitting work as we go
        while(nodeQueue.size() != 0) {
            // Grab the next node from the queue
            auto* node = nodeQueue.front();
            nodeQueue.pop();

            // If this node's parents haven't been submitted,
            // move it to the back of the queue
            if (!node->allParentsVisited(visited)) {
                nodeQueue.push(node);
                continue;
            }
            
            // Make sure we haven't been here already
            if (visited.find(node) == visited.end()) {
                visited.emplace(node);
            } else {
                continue;
            }

            // Kick off the node's work
            node->submit(ctx);

            // Add the node's children to the queue
            for (auto& child : node->getChildren()) {
                if (visited.find(child) == visited.end()) {
                    nodeQueue.push(child);
                }
            }
        }
    }
    
private:
    std::vector<std::unique_ptr<RenderNode>> nodes_;
    std::vector<bool> startNodes_;
};
