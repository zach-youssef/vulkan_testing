#pragma once

#include "core/VkTypes.h"
#include "core/VkUtil.h"
#include "core/VulkanContext.h"
#include "core/FrameResources.h"
#include "descriptors/DescriptorHeap.h"
#include "graph/GraphTypes.h"
#include "materials/AttachmentFormats.h"

#include <glm/glm.hpp>

#include <optional>
#include <vector>

/*
 Materials: a pipeline and the resources its shaders use.

 Resources are entries of the universal descriptor set (see DescriptorHeap). A material lists them
 as MaterialBindings; binding the material pushes their indices as push constants, as 32-bit
 unsigned integers in the bindings' order, followed by any data writePushConstants() adds. Shaders
 declare a matching push constant block and index the arrays in bindless.glsl.

 Each binding is either shared by every frame or has an entry per frame (FrameBinding), e.g. a
 uniform buffer per frame in flight. Bindings also say how the shaders use their resources, so a
 render graph node that uses the material (NodeBuilder::uses) declares those uses automatically.

 Every material uses the universal pipeline layout.
 */

struct MaterialBinding {
    FrameBinding<DescriptorEntry> entry;
    // How the material's shaders use the resource (ignored for samplers)
    Access access = Access::SampledRead;
    // The shader stages using it; 0 for the material's (vertex and fragment, or compute)
    VkPipelineStageFlags2 stages = 0;

    static MaterialBinding sampled(FrameBinding<DescriptorEntry> entry, VkPipelineStageFlags2 stages = 0) {
        return {std::move(entry), Access::SampledRead, stages};
    }

    static MaterialBinding sampler(FrameBinding<DescriptorEntry> entry) {
        return {std::move(entry), Access::SampledRead, 0};
    }

    // access is StorageRead, StorageWrite or StorageReadWrite
    static MaterialBinding storageImage(FrameBinding<DescriptorEntry> entry, Access access,
                                        VkPipelineStageFlags2 stages = 0) {
        return {std::move(entry), access, stages};
    }

    // access is StorageBufferRead, StorageBufferWrite or StorageBufferReadWrite
    static MaterialBinding storageBuffer(FrameBinding<DescriptorEntry> entry, Access access = Access::StorageBufferRead,
                                         VkPipelineStageFlags2 stages = 0) {
        return {std::move(entry), access, stages};
    }
};

class Material {
public:
    virtual ~Material() = default;

    // Called by the render graph once per frame before any node records, e.g. to write uniform
    // buffers. surfaceExtent is {0, 0} without a surface.
    virtual void update(uint32_t frameIndex, VkExtent2D surfaceExtent) = 0;

    // Binds the pipeline and pushes the frame's binding indices (and any extra push constants).
    // The universal descriptor set must be bound (the render graph does this).
    void bind(VkCommandBuffer commandBuffer, uint32_t frameIndex) {
        vkCmdBindPipeline(commandBuffer, bindPoint(), getPipeline());
        auto data = pushConstants(frameIndex);
        if (!data.empty()) {
            vkCmdPushConstants(commandBuffer, getPipelineLayout(), VK_SHADER_STAGE_ALL, 0,
                               static_cast<uint32_t>(data.size()), data.data());
        }
    }

    // The push constants for a frame: the bindings' indices, then writePushConstants' data
    std::vector<uint8_t> pushConstants(uint32_t frameIndex) {
        std::vector<uint8_t> data(bindings_.size() * sizeof(uint32_t));
        for (size_t i = 0; i < bindings_.size(); ++i) {
            auto entry = bindings_[i].entry.get(frameIndex);
            if (!entry) {
                throw std::logic_error("Material binding " + std::to_string(i) + " has no descriptor entry for frame "
                                       + std::to_string(frameIndex));
            }
            const uint32_t index = entry->index();
            memcpy(data.data() + i * sizeof(uint32_t), &index, sizeof(index));
        }
        writePushConstants(frameIndex, data);
        if (data.size() > DescriptorHeap::kPushConstantSize) {
            throw std::logic_error("Material push constants exceed " + std::to_string(DescriptorHeap::kPushConstantSize)
                                   + " bytes");
        }
        return data;
    }

    const std::vector<MaterialBinding>& bindings() const {
        return bindings_;
    }

    virtual VkPipelineBindPoint bindPoint() {
        return VK_PIPELINE_BIND_POINT_GRAPHICS;
    }

    // Stages bindings without explicit stages are used in
    virtual VkPipelineStageFlags2 defaultStages() {
        return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    }

    // The formats a graphics material's pipeline renders to; empty for compute materials
    virtual std::optional<AttachmentFormats> attachmentFormats() {
        return std::nullopt;
    }

    virtual VkPipeline getPipeline() {
        return **pipeline_;
    }

    VkPipelineLayout getPipelineLayout() {
        return context_->descriptors().pipelineLayout();
    }

protected:
    Material(VulkanContext& context, std::vector<MaterialBinding> bindings)
    : context_(&context), bindings_(std::move(bindings)) {}

    // Appends push constant data after the binding indices (data already holds them)
    virtual void writePushConstants(uint32_t frameIndex, std::vector<uint8_t>& data) {}

    std::unique_ptr<VulkanShaderModule> createShaderModule(const std::vector<char>& code) {
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

        std::unique_ptr<VulkanShaderModule> shaderModule;
        VK_SUCCESS_OR_THROW(VulkanShaderModule::create(shaderModule, device(), createInfo),
                            "Failed to create shader module.");
        return shaderModule;
    }

    VkDevice device() {
        return context_->device();
    }

    VulkanContext* context_;
    // Derived classes may add bindings in their constructors (before the material is used)
    std::vector<MaterialBinding> bindings_;
    std::unique_ptr<VulkanGraphicsPipeline> pipeline_;
};

class ComputeMaterial : public Material {
public:
    virtual glm::vec3 getDispatchDimensions() = 0;

    VkPipeline getPipeline() override {
        return **computePipeline_;
    }

    VkPipelineBindPoint bindPoint() override {
        return VK_PIPELINE_BIND_POINT_COMPUTE;
    }

    VkPipelineStageFlags2 defaultStages() override {
        return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    }

protected:
    ComputeMaterial(VulkanContext& context,
                    std::vector<MaterialBinding> bindings,
                    const std::vector<char>& computeShaderCode)
    : Material(context, std::move(bindings)) {
        createComputePipeline(computeShaderCode);
    }

private:
    void createComputePipeline(const std::vector<char>& computeShaderCode) {
        auto computeShaderModule = createShaderModule(computeShaderCode);

        VkPipelineShaderStageCreateInfo computeShaderStageInfo{};
        computeShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        computeShaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        computeShaderStageInfo.module = **computeShaderModule;
        computeShaderStageInfo.pName = "main";

        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.layout = getPipelineLayout();
        pipelineInfo.stage = computeShaderStageInfo;

        VK_SUCCESS_OR_THROW(VulkanComputePipeline::create(computePipeline_, device(), pipelineInfo),
                            "Failed to create compute pipeline");
    }

protected:
    std::unique_ptr<VulkanComputePipeline> computePipeline_;
};
