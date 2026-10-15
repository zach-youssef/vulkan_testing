#pragma once

#include "materials/Material.h"

#include <optional>
#include <stdexcept>
#include <vector>

// Where a graphics pipeline's vertices come from: vertex buffers bound when drawing
struct VertexLayout {
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;

    // One buffer of interleaved vertices at binding 0, e.g.
    //     VertexLayout::interleaved(sizeof(Vertex), {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)}})
    // (each attribute is {location, binding, format, offset})
    static VertexLayout interleaved(uint32_t stride, std::vector<VkVertexInputAttributeDescription> attributes) {
        return {{{0, stride, VK_VERTEX_INPUT_RATE_VERTEX}}, std::move(attributes)};
    }
};

enum class BlendMode {
    Opaque,              // Replaces the attachment
    Alpha,               // src * a + dst * (1 - a)
    PremultipliedAlpha,  // src + dst * (1 - a)
    Additive,            // src + dst
};

struct DepthBias {
    float constantFactor = 0.0f;
    float slopeFactor = 0.0f;
    float clamp = 0.0f;
};

/*
 The fixed-function state of a graphics pipeline. Every graphics material takes one, so its choices
 are visible where the material is made; the defaults are the common case (indexed triangles from
 a vertex buffer, back faces culled, opaque, depth tested and written when the target has depth).
 */
struct GraphicsPipelineOptions {
    // std::nullopt for pipelines without vertex input, whose vertex shaders build vertices from
    // gl_VertexIndex (e.g. reading storage buffers, or a fullscreen triangle)
    std::optional<VertexLayout> vertexLayout;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    // Per colour attachment; a single mode applies to every attachment
    std::vector<BlendMode> blend = {BlendMode::Opaque};
    // Ignored when the target has no depth attachment
    bool depthTest = true;
    bool depthWrite = true;
    VkCompareOp depthCompare = VK_COMPARE_OP_LESS;
    std::optional<DepthBias> depthBias;
};

inline VkPipelineColorBlendAttachmentState blendAttachmentState(BlendMode mode) {
    VkPipelineColorBlendAttachmentState state{};
    state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
                           | VK_COLOR_COMPONENT_A_BIT;
    state.colorBlendOp = VK_BLEND_OP_ADD;
    state.alphaBlendOp = VK_BLEND_OP_ADD;
    switch (mode) {
        case BlendMode::Opaque:
            state.blendEnable = VK_FALSE;
            state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            state.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            break;
        case BlendMode::Alpha:
            state.blendEnable = VK_TRUE;
            state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case BlendMode::PremultipliedAlpha:
            state.blendEnable = VK_TRUE;
            state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case BlendMode::Additive:
            state.blendEnable = VK_TRUE;
            state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
    }
    return state;
}

/*
 A material drawing with a vertex and a fragment shader into targets of the given formats.
 Use it directly, or subclass it to write per-frame data in update() or extra push constants.
 */
class GraphicsMaterial : public Material {
public:
    GraphicsMaterial(VulkanContext& context,
                     std::vector<MaterialBinding> bindings,
                     AttachmentFormats formats,
                     Handle<Shader> vertexShader,
                     Handle<Shader> fragmentShader,
                     GraphicsPipelineOptions options)
    : Material(context, std::move(bindings)),
    formats_(std::move(formats)),
    vertexShader_(std::move(vertexShader)),
    fragmentShader_(std::move(fragmentShader)),
    options_(std::move(options)) {
        requireStage(vertexShader_, VK_SHADER_STAGE_VERTEX_BIT);
        requireStage(fragmentShader_, VK_SHADER_STAGE_FRAGMENT_BIT);
        if (options_.blend.size() != 1 && options_.blend.size() != formats_.color.size()) {
            throw std::invalid_argument("GraphicsPipelineOptions::blend needs one mode, or one per colour attachment ("
                                        + std::to_string(formats_.color.size()) + ")");
        }
        initPipeline({vertexShader_, fragmentShader_});
    }

    void update(uint32_t frameIndex, VkExtent2D surfaceExtent) override {}

    std::optional<AttachmentFormats> attachmentFormats() override {
        return formats_;
    }

    const GraphicsPipelineOptions& options() const {
        return options_;
    }

protected:
    VkPipeline createPipeline() override {
        VkPipelineShaderStageCreateInfo shaderStages[2]{};
        shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        shaderStages[0].module = vertexShader_->module();
        shaderStages[0].pName = vertexShader_->entryPoint();
        shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        shaderStages[1].module = fragmentShader_->module();
        shaderStages[1].pName = fragmentShader_->entryPoint();

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        if (options_.vertexLayout) {
            const auto& layout = *options_.vertexLayout;
            vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(layout.bindings.size());
            vertexInput.pVertexBindingDescriptions = layout.bindings.data();
            vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(layout.attributes.size());
            vertexInput.pVertexAttributeDescriptions = layout.attributes.data();
        }

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = options_.topology;

        const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = 2;
        dynamicState.pDynamicStates = dynamicStates;

        // Viewport and scissor are dynamic state, set when recording
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = options_.cullMode;
        rasterizer.frontFace = options_.frontFace;
        if (options_.depthBias) {
            rasterizer.depthBiasEnable = VK_TRUE;
            rasterizer.depthBiasConstantFactor = options_.depthBias->constantFactor;
            rasterizer.depthBiasSlopeFactor = options_.depthBias->slopeFactor;
            rasterizer.depthBiasClamp = options_.depthBias->clamp;
        }

        std::vector<VkPipelineColorBlendAttachmentState> blendAttachments;
        for (size_t i = 0; i < formats_.color.size(); ++i) {
            blendAttachments.push_back(blendAttachmentState(options_.blend[options_.blend.size() == 1 ? 0 : i]));
        }
        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
        colorBlending.pAttachments = blendAttachments.data();

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        multisampling.minSampleShading = 1.0f;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        const bool hasDepth = formats_.depth != VK_FORMAT_UNDEFINED;
        depthStencil.depthTestEnable = hasDepth && options_.depthTest;
        depthStencil.depthWriteEnable = hasDepth && options_.depthWrite;
        depthStencil.depthCompareOp = options_.depthCompare;

        // Dynamic rendering: the pipeline is built against attachment formats, not a render pass
        VkPipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        renderingInfo.colorAttachmentCount = static_cast<uint32_t>(formats_.color.size());
        renderingInfo.pColorAttachmentFormats = formats_.color.data();
        renderingInfo.depthAttachmentFormat = formats_.depth;

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shaderStages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = getPipelineLayout();
        pipelineInfo.basePipelineIndex = -1;

        VkPipeline pipeline = VK_NULL_HANDLE;
        VK_SUCCESS_OR_THROW(vkCreateGraphicsPipelines(device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline),
                            "Failed to create graphics pipeline");
        return pipeline;
    }

private:
    AttachmentFormats formats_;
    Handle<Shader> vertexShader_;
    Handle<Shader> fragmentShader_;
    GraphicsPipelineOptions options_;
};
