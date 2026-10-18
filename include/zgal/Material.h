#pragma once

#include "core/Contract.h"
#include "core/DeviceFeatures.h"
#include "core/VulkanContext.h"
#include "descriptors/DescriptorHeap.h"
#include "materials/GraphicsMaterial.h"
#include "materials/Material.h"
#include "shaders/ShaderLibrary.h"

#include <span>
#include <string>
#include <vector>

/*
 What the material classes ZGAL generates have in common: the checks every one makes on how it's
 built, and the shaders it's built from. Generated code uses these; applications use the generated
 classes.
 */
namespace zgal {

// A graphics material's shaders, as its loadShaders gives them. Only Material can make them, so a
// material can't be built from shaders with another interface.
template<typename Material>
class GraphicsShaders {
public:
    const Handle<Shader>& vertex() const { return vertex_; }
    const Handle<Shader>& fragment() const { return fragment_; }

private:
    friend Material;
    GraphicsShaders(Handle<Shader> vertex, Handle<Shader> fragment)
    : vertex_(std::move(vertex)), fragment_(std::move(fragment)) {}
    Handle<Shader> vertex_;
    Handle<Shader> fragment_;
};

// A compute material's shader, as its loadShaders gives it (see GraphicsShaders)
template<typename Material>
class ComputeShaders {
public:
    const Handle<Shader>& compute() const { return compute_; }

private:
    friend Material;
    explicit ComputeShaders(Handle<Shader> compute) : compute_(std::move(compute)) {}
    Handle<Shader> compute_;
};

// A binding the application left without an entry
inline void requireBound(const FrameBinding<DescriptorEntry>& binding, const char* material, const char* name) {
    if (!binding.isPerFrame() && !binding.shared()) {
        contractViolation(std::string(material) + "'s binding '" + name + "' has no descriptor entry");
    }
}

inline void requireBound(const Handle<DescriptorArray>& binding, const char* material, const char* name) {
    if (!binding) {
        contractViolation(std::string(material) + "'s binding '" + name + "' has no DescriptorArray");
    }
}

// Checks a frame's entries are of the kinds the shaders index (types and names in binding order):
// an entry of the wrong kind would index the wrong descriptor array
inline void checkDescriptorTypes(const std::vector<MaterialBinding>& bindings, uint32_t frameIndex,
                                 std::span<const DescriptorType> types, std::span<const char* const> names,
                                 const char* material) {
    for (size_t i = 0; i < bindings.size(); ++i) {
        const auto& binding = bindings[i];
        // An array's elements
        std::vector<FrameBinding<DescriptorEntry>> entries{binding.entry};
        if (binding.descriptorArray) {
            entries = binding.descriptorArray->elements();
        }
        for (const auto& element : entries) {
            auto entry = element.get(frameIndex);
            if (entry && entry->type() != types[i]) {
                contractViolation(std::string(material) + "'s binding '" + names[i] + "' needs a " + descriptorTypeName(types[i])
                                  + " entry, but was given a " + descriptorTypeName(entry->type()) + " entry");
            }
        }
    }
}

// The formats of a target with as many colour attachments as the fragment shader writes
inline AttachmentFormats checkFormats(AttachmentFormats formats, uint32_t colorOutputs, const char* material) {
    if (formats.color.size() != colorOutputs) {
        contractViolation(std::string(material) + "'s fragment shader writes " + std::to_string(colorOutputs)
                          + " colour attachments, but its target has " + std::to_string(formats.color.size()));
    }
    return formats;
}

// options with the material's vertex layout (none without vertex input), which the application
// mustn't give: it's declared in the ZGAL file, source
inline GraphicsPipelineOptions withVertexInput(GraphicsPipelineOptions options, std::optional<VertexLayout> layout,
                                               const char* material, const char* source) {
    if (options.vertexLayout) {
        contractViolation(std::string(material) + "'s vertex input is declared in " + source
                          + ": leave GraphicsPipelineOptions::vertexLayout empty");
    }
    options.vertexLayout = std::move(layout);
    return options;
}

// The shaders can't be loaded without the device features they use, so a context lacking them is a
// mistake
inline void requireFeatures(VulkanContext& context, const DeviceFeatures& features, const char* material) {
    const auto missing = context.enabledFeatures().missing(features);
    if (!missing.empty()) {
        std::string message = std::string(material) + " needs device features the context doesn't enable:";
        for (const auto& feature : missing) {
            message += " " + feature;
        }
        contractViolation(message + ". Add " + material + "::requiredFeatures() to the context's features.");
    }
}

// Nor without subgroup operations in the stages using them; message names them
inline void requireSubgroups(VulkanContext& context, VkShaderStageFlags stages, VkSubgroupFeatureFlags operations,
                             const char* message) {
    const auto& subgroups = context.subgroupProperties();
    if ((subgroups.supportedStages & stages) != stages || (subgroups.supportedOperations & operations) != operations) {
        contractViolation(message);
    }
}

} // namespace zgal
