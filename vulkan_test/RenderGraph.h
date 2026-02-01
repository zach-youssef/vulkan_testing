#pragma once

#include "Renderable.h"
#include "VkTypes.h"

#include <unordered_set>

enum NodeDevice {
    GPU,
    CPU
};

template<uint MAX_FRAMES>
class RenderNode {
public:
    virtual ~RenderNode<MAX_FRAMES>() = default;
    
protected:
    virtual NodeDevice getDeviceType() = 0;
    
    RenderNode<MAX_FRAMES>(VkDevice device) : device_(device) {}
    
    void addSemaphoreEdgeTo(RenderNode<MAX_FRAMES>* other){
        // If this is our first outgoing edge, initialize our semaphores
        if (signalSemaphores_[0] == nullptr) {
            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            for (uint frameIndex = 0; frameIndex < MAX_FRAMES; ++frameIndex) {
                VK_SUCCESS_OR_THROW(VulkanSemaphore::create(signalSemaphores_[frameIndex], device_, semaphoreInfo),
                                    "Failed to create semaphore");
            }
        }
        
        other->addSemaphoreWait(unwrap(signalSemaphores_));
        children_.push_back(other);
    }
    
    void addSemaphoreWait(std::array<VkSemaphore, MAX_FRAMES> semaphore) {
        for (uint idx = 0; idx < MAX_FRAMES; ++idx) {
            waitSemaphores_[idx].push_back(semaphore[idx]);
        }
    }
    
    void addFenceEdgeTo(RenderNode<MAX_FRAMES>* other, bool createSignaled = false) {
        // If this is our first outgoing edge, initialize our fences
        if (signalFences_[0] == nullptr) {
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fenceInfo.flags = createSignaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0;
            
            for (uint frameIndex = 0; frameIndex < MAX_FRAMES; ++frameIndex) {
                VK_SUCCESS_OR_THROW(VulkanFence::create(signalFences_[frameIndex], device_, fenceInfo),
                                    "Failed to create fence");
            }
        }
        
        other->addFenceWait(unwrap(signalFences_));
        children_.push_back(other);
    }
    
    void addFenceWait(std::array<VkFence, MAX_FRAMES> fence) {
        for (uint idx = 0; idx < MAX_FRAMES; ++idx) {
            waitFences_[idx].push_back(fence[idx]);
        }
    }
    
    const std::vector<RenderNode<MAX_FRAMES>*> getChildren() {
        return children_;
    }
    
    virtual void submit(uint32_t frameIndex);
    
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
};

// TODO: fill in render node functions on RenderGraph
template<uint MAX_FRAMES>
class RenderGraph : public RenderNode<MAX_FRAMES> {
public:
    using NodeHandle = uint32_t;
    
    NodeHandle addNode(std::unique_ptr<RenderNode<MAX_FRAMES>>&& node) {
        nodes_.emplace_back(std::move(node));
        startNodes_.emplace_back(true);
        return nodes_.size() - 1;
    }
    
    int addEdge(NodeHandle from, NodeHandle to) {
        startNodes_[to] = false;
        if (nodes_.at(from).getDeviceType() == NodeDevice::GPU) {
            if (nodes_.at(to).getDeviceType() == NodeDevice::GPU) {
                // Semaphore case
                nodes_.at(from)->addSemaphoreEdgeTo(nodes_.at(to).get());
            } else if (nodes_.at(to).getDeviceType() == NodeDevice::CPU) {
                // Fence case
                nodes_.at(from)->addFenceEdgeTo(nodes_.at(to).get());
            }
        } else if (nodes_.at(from).getDeviceType() == NodeDevice::CPU) {
            if (nodes_.at(to).getDeviceType() == NodeDevice::GPU) {
                // TODO CPU -> GPU ???
                return EXIT_FAILURE;
            } else if (nodes_.at(to).getDeviceType() == NodeDevice::CPU) {
                // TODO CPU -> CPU ??
                return EXIT_FAILURE;
            }
        }
        return 0;
    }
    
    // 'Presenter' here implies the last node(s) in the graph that must
    // be completed before the graph is executed again.
    void flagNodeAsPresenter(NodeHandle node) {
        nodes_[node]->addFenceEdgeTo(this, true /* create signaled */);
    }
    
    void waitUntilComplete(uint32_t frameIndex) {
        // Wait for the previous frame to finish
        for (VkFence& fence : RenderNode<MAX_FRAMES>::waitFences_[frameIndex]) {
            vkWaitForFences(RenderNode<MAX_FRAMES>::device_, 1, fence, VK_TRUE, UINT64_MAX);
        }
    }
    
    void execute(uint32_t frameIndex) {
        // Reset our fences
        for (VkFence& fence : RenderNode<MAX_FRAMES>::waitFences_[frameIndex]) {
            vkResetFences(RenderNode<MAX_FRAMES>::device_, 1, fence);
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
            
            // Make sure we haven't been here already
            if (visited.find(node) == visited.end()) {
                visited.emplace(node);
            } else {
                continue;
            }
            
            // Kick off the node's work
            node->submit(frameIndex);

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
