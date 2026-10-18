#pragma once

#include "core/Contract.h"
#include "graph/RenderGraph.h"
#include "materials/Material.h"

#include <cstring>
#include <vector>

/*
 Dispatches several compute materials in order, as one node: the steps of an algorithm (e.g. a
 prefix sum's reduce, scan and apply), which the graph schedules, enables and synchronizes with
 other nodes as a whole. Between steps, every compute shader write is made visible to the next
 step's reads and writes, so a step may read what earlier ones wrote to any buffer or image.

 A step can replace part of its material's push constants for its own dispatch (e.g. a pass index,
 so passes share a material and its pipeline): `push` overwrites the bytes from `pushOffset`.

 A step whose material dispatches no workgroups this frame is skipped, barrier and all, so a node
 can hold alternative algorithms and size the one it uses (e.g. one workgroup for few elements).
 */
class DispatchSequenceNode : public ComputeNode {
public:
    struct Step {
        Handle<ComputeMaterial> material;
        uint32_t pushOffset = 0;
        std::vector<uint8_t> push;
    };

    // A step dispatching material with the uint32_t at pushOffset replaced by value
    static Step withPushedUint(Handle<ComputeMaterial> material, uint32_t pushOffset, uint32_t value) {
        std::vector<uint8_t> bytes(sizeof(value));
        std::memcpy(bytes.data(), &value, sizeof(value));
        return {std::move(material), pushOffset, std::move(bytes)};
    }

    explicit DispatchSequenceNode(std::vector<Step> steps) : steps_(std::move(steps)) {
        if (steps_.empty()) {
            contractViolation("DispatchSequenceNode was given no steps");
        }
        for (const auto& step : steps_) {
            if (!step.material) {
                contractViolation("DispatchSequenceNode was given a step without a material");
            }
            if (step.pushOffset + step.push.size() > DescriptorHeap::kPushConstantSize) {
                contractViolation("DispatchSequenceNode step pushes past the push constants");
            }
        }
    }

    void setup(NodeBuilder& builder) override {
        for (const auto& step : steps_) {
            builder.uses(step.material);
        }
    }

    void record(RecordContext& context) override {
        VkCommandBuffer cmd = context.commandBuffer;
        bool dispatched = false;
        for (const Step& step : steps_) {
            const glm::vec3 groups = step.material->getDispatchDimensions();
            if (groups.x < 1.0f || groups.y < 1.0f || groups.z < 1.0f) {
                continue;
            }
            if (dispatched) {
                VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.memoryBarrierCount = 1;
                dependency.pMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(cmd, &dependency);
            }
            step.material->bind(cmd, context.frameIndex);
            if (!step.push.empty()) {
                vkCmdPushConstants(cmd, step.material->getPipelineLayout(), VK_SHADER_STAGE_ALL, step.pushOffset,
                                   static_cast<uint32_t>(step.push.size()), step.push.data());
            }
            vkCmdDispatch(cmd, static_cast<uint32_t>(groups.x), static_cast<uint32_t>(groups.y),
                          static_cast<uint32_t>(groups.z));
            dispatched = true;
        }
    }

private:
    std::vector<Step> steps_;
};
