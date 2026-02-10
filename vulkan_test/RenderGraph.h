#pragma once

#include "VkTypes.h"
#include "VkUtil.h"

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
};

template<uint MAX_FRAMES>
class RenderGraph;

template<uint MAX_FRAMES>
class RenderNode {
    friend class RenderGraph<MAX_FRAMES>;
public:
    virtual ~RenderNode<MAX_FRAMES>() = default;
    
    virtual void submit(RenderEvalContext& ctx) = 0;

protected:
    virtual NodeDevice getDeviceType() = 0;

    RenderNode<MAX_FRAMES>(VkDevice device) : device_(device) {
        // Initialize signal objects to VK_NULL_HANDLE
        for (uint frameIdx = 0; frameIdx < MAX_FRAMES; ++frameIdx) {
            signalFences_[frameIdx] = std::make_unique<VulkanFence>();
            signalSemaphores_[frameIdx] = std::make_unique<VulkanSemaphore>();
        }
    }
    
    void addSemaphoreEdgeTo(RenderNode<MAX_FRAMES>* other){
        // If this is our first outgoing edge, initialize our semaphores
        if (**signalSemaphores_[0] == VK_NULL_HANDLE) {
            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            for (uint frameIndex = 0; frameIndex < MAX_FRAMES; ++frameIndex) {
                VK_SUCCESS_OR_THROW(VulkanSemaphore::create(signalSemaphores_[frameIndex], device_, semaphoreInfo),
                                    "Failed to create semaphore");
            }
        }
        
        other->addSemaphoreWait(unwrap<VkSemaphore, VulkanSemaphore>(signalSemaphores_), this);
        children_.push_back(other);
    }
    
    void addSemaphoreWait(std::array<VkSemaphore, MAX_FRAMES> semaphore, RenderNode<MAX_FRAMES>* parent) {
        for (uint idx = 0; idx < MAX_FRAMES; ++idx) {
            waitSemaphores_[idx].push_back(semaphore[idx]);
        }
        parents_.push_back(parent);
    }
    
    void addFenceEdgeTo(RenderNode<MAX_FRAMES>* other, bool createSignaled = false) {
        // If this is our first outgoing edge, initialize our fences
        if (**signalFences_[0] == VK_NULL_HANDLE) {
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fenceInfo.flags = createSignaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0;
            
            for (uint frameIndex = 0; frameIndex < MAX_FRAMES; ++frameIndex) {
                VK_SUCCESS_OR_THROW(VulkanFence::create(signalFences_[frameIndex], device_, fenceInfo),
                                    "Failed to create fence");
            }
        }
        
        other->addFenceWait(unwrap<VkFence, VulkanFence>(signalFences_), this);
        children_.push_back(other);
    }
    
    void addFenceWait(std::array<VkFence, MAX_FRAMES> fence, RenderNode<MAX_FRAMES>* parent) {
        for (uint idx = 0; idx < MAX_FRAMES; ++idx) {
            waitFences_[idx].push_back(fence[idx]);
        }
        parents_.push_back(parent);
    }
    
    const std::vector<RenderNode<MAX_FRAMES>*> getChildren() {
        return children_;
    }

    bool allParentsVisited(const std::unordered_set<RenderNode<MAX_FRAMES>*>& visited) {
        for (auto parent : parents_) {
            if (visited.find(parent) == visited.end()) {
                return false;
            }
        }
        return true;
    }
    
private:
    template<typename VkType, typename WrapperType>
    static std::array<VkType, MAX_FRAMES> unwrap(std::array<std::unique_ptr<WrapperType>, MAX_FRAMES>& wrapped) {
        std::array<VkType, MAX_FRAMES> unwrapped;
        for (uint idx = 0; idx < MAX_FRAMES; ++idx) {
            unwrapped[idx] = **wrapped[idx];
        }
        return unwrapped;
    }
    
protected:
    VkDevice device_;
    std::vector<RenderNode<MAX_FRAMES>*> children_;
    std::array<std::unique_ptr<VulkanSemaphore>, MAX_FRAMES> signalSemaphores_;
    std::array<std::vector<VkSemaphore>, MAX_FRAMES> waitSemaphores_;
    std::array<std::unique_ptr<VulkanFence>, MAX_FRAMES> signalFences_;
    std::array<std::vector<VkFence>, MAX_FRAMES> waitFences_;

private:
    std::vector<RenderNode<MAX_FRAMES>*> parents_;
};

template<uint MAX_FRAMES>
class RenderGraph : public RenderNode<MAX_FRAMES> {
public:
    using NodeHandle = uint32_t;
    
    RenderGraph<MAX_FRAMES>(VkDevice device): RenderNode<MAX_FRAMES>(device) {}
    
    NodeDevice getDeviceType() override {
        return NodeDevice::CPU;
    }
    
    NodeHandle addNode(std::unique_ptr<RenderNode<MAX_FRAMES>>&& node) {
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
        auto& fences = RenderNode<MAX_FRAMES>::waitFences_[frameIndex];
        for (uint idx = 0; idx < fences.size(); ++idx) {
            vkWaitForFences(RenderNode<MAX_FRAMES>::device_, 1, &fences[idx], VK_TRUE, UINT64_MAX);
        }
    }
    
    void submit(RenderEvalContext& ctx) override {
        // Reset our fences
        for (VkFence& fence : RenderNode<MAX_FRAMES>::waitFences_[ctx.frameIndex]) {
            vkResetFences(RenderNode<MAX_FRAMES>::device_, 1, &fence);
        }
        
        // Initialize the queue with all nodes without an incoming edge
        std::queue<RenderNode<MAX_FRAMES>*> nodeQueue;
        for (uint idx = 0; idx < nodes_.size(); ++idx) {
            if (startNodes_[idx]) {
                nodeQueue.push(nodes_.at(idx).get());
            }
        }
        // Keep track of nodes that have been visited
        std::unordered_set<RenderNode<MAX_FRAMES>*> visited;
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
    std::vector<std::unique_ptr<RenderNode<MAX_FRAMES>>> nodes_;
    std::vector<bool> startNodes_;
};
