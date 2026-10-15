#pragma once

#include "core/VkTypes.h"
#include "core/VulkanContext.h"
#include "materials/Material.h"
#include "resources/Buffer.h"

/*
 A Renderable is something a RenderablesNode draws: vertex and index buffers drawn with a material.
 */

class Renderable {
public:
    virtual ~Renderable() = default;
    
    Material* getMaterial() {
        return material_.get();
    }

    Handle<Material> getMaterialHandle() {
        return material_;
    }
    
    virtual VkBuffer getVertexBuffer() = 0;
    virtual VkBuffer getIndexBuffer() = 0;
    virtual uint32_t getIndexCount() = 0;
    
protected:
    Renderable(Handle<Material> material)
    : material_(std::move(material)) {}
    
protected:
    Handle<Material> material_;
};

template<typename VertexData>
class MeshRenderable : public Renderable {
public:
    MeshRenderable(const std::vector<VertexData>& vertexData,
                   const std::vector<uint16_t>& indexData,
                   Handle<Material> material,
                   VulkanContext& context)
    : Renderable(std::move(material)), indexCount_(static_cast<uint32_t>(indexData.size())) {
        Buffer<VertexData>::createAndInitialize(vertexBuffer_, context, vertexData, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        Buffer<uint16_t>::createAndInitialize(indexBuffer_, context, indexData, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    }

    VkBuffer getVertexBuffer() {
        return vertexBuffer_->getBuffer();
    }
    
    VkBuffer getIndexBuffer() {
        return indexBuffer_->getBuffer();
    }
    
    uint32_t getIndexCount() {
        return indexCount_;
    }

private:
    Handle<Buffer<VertexData>> vertexBuffer_;
    Handle<Buffer<uint16_t>> indexBuffer_;
    uint32_t indexCount_;
};
