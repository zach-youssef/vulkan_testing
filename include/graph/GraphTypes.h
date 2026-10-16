#pragma once

#include "core/Contract.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

/*
 Types shared by the render graph's compiler, synchronization and runtime.

 Graphs that can't be compiled (cycles, more than one surface writer, invalid uses, ...) fail with
 ErrorCode::InvalidGraph (see Result.h). Misusing the graph's API while building it is a contract
 violation (see Contract.h).
 */

struct NodeId {
    uint32_t index = UINT32_MAX;
    explicit operator bool() const { return index != UINT32_MAX; }
    bool operator==(const NodeId&) const = default;
};

struct ImageId {
    uint32_t index = UINT32_MAX;
    explicit operator bool() const { return index != UINT32_MAX; }
    bool operator==(const ImageId&) const = default;
};

struct BufferId {
    uint32_t index = UINT32_MAX;
    explicit operator bool() const { return index != UINT32_MAX; }
    bool operator==(const BufferId&) const = default;
};

enum class NodeKind {
    Graphics,   // Records inside dynamic rendering on the primary queue
    Compute,    // Records on any compute-capable queue
    Transfer,   // Records on any queue
    Cpu,        // Runs on the host
};

// Whether a graph-created resource has a copy per frame in flight. Deliberately has no default.
enum class FrameMode {
    // One copy per frame in flight: frames can overlap freely, at the cost of memory
    PerFrame,
    // One copy shared by every frame: a frame's use waits for the previous frame's conflicting use
    Shared,
};

// How a node uses a resource
enum class Access {
    // Images
    SampledRead,            // Sampled in shaders, SHADER_READ_ONLY_OPTIMAL
    StorageRead,            // Storage image loads, GENERAL
    StorageWrite,           // Storage image stores, GENERAL
    StorageReadWrite,       // Both, GENERAL
    ColorAttachment,        // Rendered to, COLOR_ATTACHMENT_OPTIMAL
    DepthAttachment,        // Depth tested and written, DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    DepthReadOnly,          // Depth tested without writes, or sampled, DEPTH_STENCIL_READ_ONLY_OPTIMAL
    // Images (with layouts) or buffers
    TransferSrc,            // Copied from
    TransferDst,            // Copied or cleared to
    // Buffers
    UniformRead,
    StorageBufferRead,
    StorageBufferWrite,
    StorageBufferReadWrite,
    VertexRead,
    IndexRead,
    IndirectRead,
    // Host (CPU nodes)
    HostWrite,
    HostRead,
};

// What an access means for synchronization
struct AccessInfo {
    VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;   // Images only
    bool read = false;
    bool write = false;
};

inline const char* accessName(Access access) {
    switch (access) {
        case Access::SampledRead: return "SampledRead";
        case Access::StorageRead: return "StorageRead";
        case Access::StorageWrite: return "StorageWrite";
        case Access::StorageReadWrite: return "StorageReadWrite";
        case Access::ColorAttachment: return "ColorAttachment";
        case Access::DepthAttachment: return "DepthAttachment";
        case Access::DepthReadOnly: return "DepthReadOnly";
        case Access::TransferSrc: return "TransferSrc";
        case Access::TransferDst: return "TransferDst";
        case Access::UniformRead: return "UniformRead";
        case Access::StorageBufferRead: return "StorageBufferRead";
        case Access::StorageBufferWrite: return "StorageBufferWrite";
        case Access::StorageBufferReadWrite: return "StorageBufferReadWrite";
        case Access::VertexRead: return "VertexRead";
        case Access::IndexRead: return "IndexRead";
        case Access::IndirectRead: return "IndirectRead";
        case Access::HostWrite: return "HostWrite";
        case Access::HostRead: return "HostRead";
    }
    return "?";
}

inline bool isImageAccess(Access access) {
    switch (access) {
        case Access::SampledRead: case Access::StorageRead: case Access::StorageWrite:
        case Access::StorageReadWrite: case Access::ColorAttachment: case Access::DepthAttachment:
        case Access::DepthReadOnly: case Access::TransferSrc: case Access::TransferDst:
            return true;
        default:
            return false;
    }
}

inline bool isBufferAccess(Access access) {
    switch (access) {
        case Access::SampledRead: case Access::StorageRead: case Access::StorageWrite:
        case Access::StorageReadWrite: case Access::ColorAttachment: case Access::DepthAttachment:
        case Access::DepthReadOnly:
            return false;
        default:
            return true;
    }
}

// Shader stages that shader accesses use when a node doesn't say
inline VkPipelineStageFlags2 defaultShaderStages(NodeKind kind) {
    switch (kind) {
        case NodeKind::Graphics: return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        case NodeKind::Compute: return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        default: return VK_PIPELINE_STAGE_2_NONE;
    }
}

// The stages, access and layout of an access. shaderStages applies to shader accesses
// (sampled, storage and uniform).
inline AccessInfo accessInfo(Access access, VkPipelineStageFlags2 shaderStages) {
    switch (access) {
        case Access::SampledRead:
            return {shaderStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true, false};
        case Access::StorageRead:
            return {shaderStages, VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, true, false};
        case Access::StorageWrite:
            return {shaderStages, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, false, true};
        case Access::StorageReadWrite:
            return {shaderStages, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, true, true};
        case Access::ColorAttachment:
            return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true, true};
        case Access::DepthAttachment:
            return {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, true, true};
        case Access::DepthReadOnly:
            return {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | shaderStages,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | (shaderStages ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0),
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, true, false};
        case Access::TransferSrc:
            return {VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, true, false};
        case Access::TransferDst:
            return {VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, false, true};
        case Access::UniformRead:
            return {shaderStages, VK_ACCESS_2_UNIFORM_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED, true, false};
        case Access::StorageBufferRead:
            return {shaderStages, VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED, true, false};
        case Access::StorageBufferWrite:
            return {shaderStages, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, false, true};
        case Access::StorageBufferReadWrite:
            return {shaderStages, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, true, true};
        case Access::VertexRead:
            return {VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, true, false};
        case Access::IndexRead:
            return {VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED, true, false};
        case Access::IndirectRead:
            return {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, true, false};
        case Access::HostWrite:
            return {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, false, true};
        case Access::HostRead:
            return {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED, true, false};
    }
    contractViolation("Unknown access");
}

// Image usage an access requires
inline VkImageUsageFlags requiredImageUsage(Access access) {
    switch (access) {
        case Access::SampledRead: return VK_IMAGE_USAGE_SAMPLED_BIT;
        case Access::StorageRead: case Access::StorageWrite: case Access::StorageReadWrite:
            return VK_IMAGE_USAGE_STORAGE_BIT;
        case Access::ColorAttachment: return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        case Access::DepthAttachment: return VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        case Access::DepthReadOnly: return VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        case Access::TransferSrc: return VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        case Access::TransferDst: return VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        default: return 0;
    }
}

// Buffer usage an access requires
inline VkBufferUsageFlags requiredBufferUsage(Access access) {
    switch (access) {
        case Access::TransferSrc: return VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        case Access::TransferDst: return VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        case Access::UniformRead: return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        case Access::StorageBufferRead: case Access::StorageBufferWrite: case Access::StorageBufferReadWrite:
            return VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        case Access::VertexRead: return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        case Access::IndexRead: return VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        case Access::IndirectRead: return VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        default: return 0;
    }
}

// The state an imported image rests in between frames: the graph expects it there at the start of
// every frame and returns it there at the end of every frame
struct ImageState {
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;

    // Sampled by shaders, e.g. a loaded texture
    static ImageState shaderReadOnly() {
        return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    }

    // Storage images (as Image::createStorage leaves them)
    static ImageState general() {
        return {VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    }

    // Copied from, e.g. read back after the frame
    static ImageState transferSrc() {
        return {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
    }
};
