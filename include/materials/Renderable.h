#pragma once

#include "core/VkTypes.h"
#include "core/VulkanContext.h"
#include "materials/Material.h"
#include "resources/Buffer.h"

#include <cstring>
#include <type_traits>

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
    virtual VkIndexType getIndexType() = 0;
    virtual uint32_t getIndexCount() = 0;
    // The draw's first instance, which vertex shaders read as gl_InstanceIndex
    virtual uint32_t getFirstInstance() { return 0; }

protected:
    Renderable(Handle<Material> material)
    : material_(std::move(material)) {}

protected:
    Handle<Material> material_;
};

/*
 Vertices and 16- or 32-bit indices in device-local buffers, shared by any number of renderables
 (e.g. a shadow pass and a lit pass drawing the same geometry with different materials):

    TRY_ASSIGN(auto mesh, Mesh::create(ctx, vertices, indices));
    auto lit = ctx.resources().create<MeshRenderable>(mesh, litMaterial);
    auto shadow = ctx.resources().create<MeshRenderable>(mesh, shadowMaterial);
 */
class Mesh {
public:
    // Uploads the vertices and indices, blocking until they're on the device. Drawing it, vertex
    // shaders read instanceIndex as gl_InstanceIndex, e.g. to choose the mesh's material.
    template<typename Vertex, typename Index>
    static Result<Handle<Mesh>> create(VulkanContext& context, const std::vector<Vertex>& vertices,
                                       const std::vector<Index>& indices, uint32_t instanceIndex = 0) {
        static_assert(std::is_trivially_copyable_v<Vertex>);
        std::unique_ptr<Mesh> mesh(new Mesh(indexTypeOf<Index>(), static_cast<uint32_t>(indices.size()), instanceIndex));
        TRY_ASSIGN(mesh->vertexBuffer_, Buffer<uint8_t>::createAndInitialize(context, bytesOf(vertices),
                                                                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT));
        TRY_ASSIGN(mesh->indexBuffer_, Buffer<uint8_t>::createAndInitialize(context, bytesOf(indices),
                                                                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT));
        return context.resources().adopt(std::move(mesh));
    }

    VkBuffer vertexBuffer() { return vertexBuffer_->getBuffer(); }
    VkBuffer indexBuffer() { return indexBuffer_->getBuffer(); }
    VkIndexType indexType() const { return indexType_; }
    uint32_t indexCount() const { return indexCount_; }
    uint32_t instanceIndex() const { return instanceIndex_; }

private:
    Mesh(VkIndexType indexType, uint32_t indexCount, uint32_t instanceIndex)
    : indexType_(indexType), indexCount_(indexCount), instanceIndex_(instanceIndex) {}

    template<typename Index>
    static constexpr VkIndexType indexTypeOf() {
        static_assert(std::is_same_v<Index, uint16_t> || std::is_same_v<Index, uint32_t>,
                      "Mesh indices are uint16_t or uint32_t");
        return std::is_same_v<Index, uint16_t> ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    }

    template<typename T>
    static std::vector<uint8_t> bytesOf(const std::vector<T>& data) {
        std::vector<uint8_t> bytes(sizeof(T) * data.size());
        if (!bytes.empty()) {
            memcpy(bytes.data(), data.data(), bytes.size());
        }
        return bytes;
    }

    Handle<Buffer<uint8_t>> vertexBuffer_;
    Handle<Buffer<uint8_t>> indexBuffer_;
    VkIndexType indexType_;
    uint32_t indexCount_;
    uint32_t instanceIndex_;
};

// A mesh drawn with a material
class MeshRenderable : public Renderable {
public:
    MeshRenderable(Handle<Mesh> mesh, Handle<Material> material)
    : Renderable(std::move(material)), mesh_(std::move(mesh)) {}

    VkBuffer getVertexBuffer() override { return mesh_->vertexBuffer(); }
    VkBuffer getIndexBuffer() override { return mesh_->indexBuffer(); }
    VkIndexType getIndexType() override { return mesh_->indexType(); }
    uint32_t getIndexCount() override { return mesh_->indexCount(); }
    uint32_t getFirstInstance() override { return mesh_->instanceIndex(); }

    const Handle<Mesh>& mesh() const { return mesh_; }

private:
    Handle<Mesh> mesh_;
};
