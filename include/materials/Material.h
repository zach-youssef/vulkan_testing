#pragma once

#include "core/Contract.h"
#include "core/Result.h"
#include "core/VkTypes.h"
#include "core/VkUtil.h"
#include "core/VulkanContext.h"
#include "core/FrameResources.h"
#include "descriptors/DescriptorArray.h"
#include "descriptors/DescriptorHeap.h"
#include "graph/GraphTypes.h"
#include "materials/AttachmentFormats.h"
#include "shaders/ShaderLibrary.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <optional>
#include <string>
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

 Every material uses the universal pipeline layout. Constructing a material can't fail: its
 pipeline is built from its Shaders by ensurePipeline(), which the render graph calls when it
 compiles a plan using the material, so pipeline errors come back from compile() and execute().
 Pipelines are rebuilt when any of the shaders is reloaded (ShaderLibrary::reload); if rebuilding
 fails the material keeps its previous pipeline and the reload reports the error. Replaced
 pipelines are destroyed once in-flight work has finished.

 Declared accesses are all the render graph synchronizes on: synchronization validation can't see
 accesses through the bindless arrays. So materials check their shaders' code: a material whose
 shaders write storage images (or storage buffers) must have a binding declaring such a write, or
 the graph fails to compile (and reloading such a shader fails). Which binding a write goes
 through isn't checked, so declare each binding's access accurately.
 */

struct MaterialBinding {
    FrameBinding<DescriptorEntry> entry;
    // How the material's shaders use the resource (ignored for samplers)
    Access access = Access::SampledRead;
    // The shader stages using it; 0 for the material's (vertex and fragment, or compute)
    VkPipelineStageFlags2 stages = 0;
    // An array binding's elements (entry is then the array's own buffer)
    Handle<DescriptorArray> descriptorArray;

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

    // An array of sampled images, storage images or storage buffers, all used with one access
    // (that of their kind, as above). It pushes the index of the array's buffer of indices (see
    // DescriptorArray), and a node using the material uses every element.
    static MaterialBinding array(Handle<DescriptorArray> elements, Access access, VkPipelineStageFlags2 stages = 0) {
        if (!elements) {
            contractViolation("MaterialBinding::array was given no DescriptorArray");
        }
        return {elements->entries(), access, stages, std::move(elements)};
    }
};

class Material {
public:
    virtual ~Material() {
        for (auto& [shader, listener] : shaderListeners_) {
            shader->removeListener(listener);
        }
        retirePipeline();
    }

    // Called by the render graph once per frame, e.g. to write uniform buffers: just before the
    // first submission recording a node that uses the material, so after CPU nodes ordered before
    // that node (by declared uses or addDependency). Materials of disabled nodes aren't updated.
    // surfaceExtent is the graph's surfaceExtent(): {0, 0} without a surface or one set headless.
    virtual void update(uint32_t frameIndex, VkExtent2D surfaceExtent) = 0;

    // Binds the pipeline and pushes the frame's binding indices (and any extra push constants).
    // The universal descriptor set must be bound (the render graph does this), and the pipeline
    // built (see ensurePipeline).
    void bind(VkCommandBuffer commandBuffer, uint32_t frameIndex) {
        if (pipeline_ == VK_NULL_HANDLE) {
            contractViolation("Material bound before its pipeline was built (render graphs build it when compiling; "
                              "otherwise call ensurePipeline())");
        }
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
            if (bindings_[i].descriptorArray) {
                bindings_[i].descriptorArray->update(frameIndex);
            }
            auto entry = bindings_[i].entry.get(frameIndex);
            if (!entry) {
                contractViolation("Material binding " + std::to_string(i) + " has no descriptor entry for frame "
                                  + std::to_string(frameIndex));
            }
            const uint32_t index = entry->index();
            memcpy(data.data() + i * sizeof(uint32_t), &index, sizeof(index));
        }
        writePushConstants(frameIndex, data);
        if (data.size() > DescriptorHeap::kPushConstantSize) {
            contractViolation("Material push constants exceed " + std::to_string(DescriptorHeap::kPushConstantSize)
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

    // The pipeline, or VK_NULL_HANDLE until it has been built (see ensurePipeline)
    VkPipeline getPipeline() {
        return pipeline_;
    }

    // Incremented whenever the pipeline is rebuilt (e.g. after a shader reload)
    uint64_t pipelineVersion() const {
        return pipelineVersion_;
    }

    VkPipelineLayout getPipelineLayout() {
        return context_->descriptors().pipelineLayout();
    }

    // Why the bindings don't declare what the shaders write, or empty if they do
    std::string undeclaredWrites() {
        struct Kind {
            DescriptorType type;
            const char* name;
            Access write;
            Access readWrite;
        };
        const Kind kinds[] = {
            {DescriptorType::StorageImage, "storage images", Access::StorageWrite, Access::StorageReadWrite},
            {DescriptorType::StorageBuffer, "storage buffers", Access::StorageBufferWrite, Access::StorageBufferReadWrite},
        };
        for (const auto& shader : shaders_) {
            const SpirvDescriptorWrites writes = shader->descriptorWrites();
            for (const auto& kind : kinds) {
                if (!writes.writes(static_cast<uint32_t>(kind.type))) {
                    continue;
                }
                const bool declared = std::any_of(bindings_.begin(), bindings_.end(), [&kind](const MaterialBinding& binding) {
                    return binding.access == kind.write || binding.access == kind.readWrite;
                });
                if (!declared) {
                    return std::string("Shader '") + shader->name() + "' writes " + kind.name
                           + ", but none of its material's bindings declares a write (Access::" + accessName(kind.write)
                           + " or " + accessName(kind.readWrite) + "), so the writes wouldn't be synchronized";
                }
            }
        }
        return {};
    }

    // Builds the pipeline unless it has been built. Render graphs call this when compiling a plan
    // that uses the material.
    Result<void> ensurePipeline() {
        if (pipeline_ != VK_NULL_HANDLE) {
            return {};
        }
        TRY_ASSIGN(pipeline_, createPipeline());
        ++pipelineVersion_;
        return {};
    }

    // Rebuilds the pipeline from the shaders' current modules. Fails (keeping the previous
    // pipeline) if that fails, or if the shaders now write what the bindings don't declare.
    Result<void> rebuildPipeline() {
        if (auto error = undeclaredWrites(); !error.empty()) {
            return Error(ErrorCode::InvalidGraph, error);
        }
        TRY_ASSIGN(VkPipeline pipeline, createPipeline());
        retirePipeline();
        pipeline_ = pipeline;
        ++pipelineVersion_;
        return {};
    }

protected:
    Material(VulkanContext& context, std::vector<MaterialBinding> bindings)
    : context_(&context), bindings_(std::move(bindings)) {}

    // Creates a pipeline (with the universal layout) from the material's shaders
    virtual Result<VkPipeline> createPipeline() = 0;

    // Records the shaders the pipeline is built from, and rebuilds it (once built) whenever one of
    // them is reloaded. Derived constructors call this.
    void initPipeline(const std::vector<Handle<Shader>>& shaders) {
        shaders_ = shaders;
        for (const auto& shader : shaders) {
            auto id = shader->addListener([this](Shader&) -> Result<void> {
                // Not built yet: it will be built from the new code
                return pipeline_ == VK_NULL_HANDLE ? Result<void>() : rebuildPipeline();
            });
            shaderListeners_.emplace_back(shader, id);
        }
    }

    static void requireStage(const Handle<Shader>& shader, VkShaderStageFlagBits stage) {
        if (!shader) {
            contractViolation("Material shader is missing");
        }
        if (shader->stage() != stage) {
            contractViolation("Shader '" + shader->name() + "' is a " + string_VkShaderStageFlagBits(shader->stage())
                              + " shader, but a " + string_VkShaderStageFlagBits(stage) + " shader is needed");
        }
    }

    // Appends push constant data after the binding indices (data already holds them)
    virtual void writePushConstants(uint32_t frameIndex, std::vector<uint8_t>& data) {}

    VkDevice device() {
        return context_->device();
    }

    VulkanContext* context_;
    // Derived classes may add bindings in their constructors (before the material is used)
    std::vector<MaterialBinding> bindings_;

private:
    void retirePipeline() {
        if (pipeline_ != VK_NULL_HANDLE) {
            context_->retire([device = device(), pipeline = pipeline_] { vkDestroyPipeline(device, pipeline, nullptr); });
            pipeline_ = VK_NULL_HANDLE;
        }
    }

    VkPipeline pipeline_ = VK_NULL_HANDLE;
    uint64_t pipelineVersion_ = 0;
    std::vector<Handle<Shader>> shaders_;
    std::vector<std::pair<Handle<Shader>, Shader::ListenerId>> shaderListeners_;
};

class ComputeMaterial : public Material {
public:
    virtual glm::vec3 getDispatchDimensions() = 0;

    VkPipelineBindPoint bindPoint() override {
        return VK_PIPELINE_BIND_POINT_COMPUTE;
    }

    VkPipelineStageFlags2 defaultStages() override {
        return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    }

protected:
    ComputeMaterial(VulkanContext& context,
                    std::vector<MaterialBinding> bindings,
                    Handle<Shader> shader)
    : Material(context, std::move(bindings)), shader_(std::move(shader)) {
        requireStage(shader_, VK_SHADER_STAGE_COMPUTE_BIT);
        initPipeline({shader_});
    }

    Result<VkPipeline> createPipeline() override {
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.layout = getPipelineLayout();
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = shader_->module();
        pipelineInfo.stage.pName = shader_->entryPoint();

        VkPipeline pipeline = VK_NULL_HANDLE;
        VK_TRY(vkCreateComputePipelines(device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline),
               "Failed to create the compute pipeline of shader '" + shader_->name() + "'");
        return pipeline;
    }

    Handle<Shader> shader_;
};
