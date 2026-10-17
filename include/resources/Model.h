#pragma once

#include "core/Result.h"
#include "core/VulkanContext.h"
#include "materials/Renderable.h"
#include "resources/Image.h"

#include <glm/glm.hpp>

// The loaders' implementations are compiled where the wrapper's are (VK_WRAP_UTIL_IMPL), so that
// translation unit must include this header if any uses it
#ifdef VK_WRAP_UTIL_IMPL
#define CGLTF_IMPLEMENTATION
#define TINYOBJLOADER_IMPLEMENTATION
#endif
#include <cgltf.h>
#include <tiny_obj_loader.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

/*
 Models loaded from files: glTF (.gltf, .glb) and OBJ (.obj, with its .mtl). ModelData is what a
 file holds, on the CPU: triangles with their transforms baked in (so a model is drawn without
 per-node matrices) and base colour materials (a factor and an optional texture, as in glTF's
 metallic-roughness model; OBJ's diffuse colour and texture). uploadModel makes meshes and textures
 of it in the vertex format the application chooses:

     TRY_ASSIGN(auto data, ModelData::load(ctx.findAsset("ship.gltf").value()));
     TRY_ASSIGN(auto model, uploadModel<MyVertex>(ctx, data, [](const ModelPrimitive& p) -> Result<std::vector<MyVertex>> {...}));

 Each mesh's instance index is its material's index, so vertex shaders read the material as
 gl_InstanceIndex (e.g. indexing baseColorFactors in a storage buffer and baseColorTextures in a
 DescriptorArray).

 Texture coordinates have glTF's convention (v = 0 at the top of the image); OBJ's are flipped to it.
 Front faces are counter-clockwise, as both formats specify.
 */

// Triangles with one material. Attributes are per vertex; ones the file lacks are empty.
struct ModelPrimitive {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;          // glTF's TEXCOORD_0
    std::vector<glm::vec4> colors;       // glTF's COLOR_0 (alpha 1 when it has three components)
    std::vector<glm::vec4> tangents;     // xyz and the bitangent's sign
    std::vector<uint32_t> indices;       // Three per triangle
    uint32_t material = 0;               // Into ModelData::materials
};

// An image a material uses: a file, or an encoded file's bytes (embedded in a glTF file)
struct ModelImage {
    std::filesystem::path path;          // Empty when encoded holds it
    std::vector<uint8_t> encoded;
    std::string description;             // For errors, e.g. "image 2 of ship.gltf"
};

struct ModelMaterial {
    std::string name;
    glm::vec4 baseColorFactor{1.0f};     // Linear
    std::optional<ModelImage> baseColorTexture;   // sRGB
};

struct ModelData {
    std::vector<ModelPrimitive> primitives;
    // The file's materials, then a default one (white, untextured) for primitives without any
    std::vector<ModelMaterial> materials;

    // Loads a .gltf, .glb or .obj file (by extension). Fails with ErrorCode::NotFound if it doesn't
    // exist, InvalidData if it can't be read, has no triangles or holds what isn't supported (e.g.
    // compressed meshes).
    static Result<ModelData> load(const std::filesystem::path& path) {
        if (!std::filesystem::exists(path)) {
            return Error(ErrorCode::NotFound, "Model '" + path.string() + "' not found");
        }
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return std::tolower(c); });
        auto load = [&]() -> Result<ModelData> {
            if (extension == ".obj") {
                return loadObj(path);
            }
            if (extension == ".gltf" || extension == ".glb") {
                return loadGltf(path);
            }
            return Error(ErrorCode::InvalidData, "Model '" + path.string() + "' isn't a .gltf, .glb or .obj file");
        };
        TRY_ASSIGN(auto model, load());
        if (model.primitives.empty()) {
            return Error(ErrorCode::InvalidData, "Model '" + path.string() + "' has no triangles");
        }
        return model;
    }

private:
    static ModelMaterial defaultMaterial() {
        return ModelMaterial{"default", glm::vec4(1.0f), std::nullopt};
    }

    static Result<ModelData> loadGltf(const std::filesystem::path& path) {
        const std::string name = path.string();
        cgltf_options options{};
        cgltf_data* parsed = nullptr;
        if (cgltf_parse_file(&options, name.c_str(), &parsed) != cgltf_result_success) {
            return Error(ErrorCode::InvalidData, "Failed to parse glTF file '" + name + "'");
        }
        std::unique_ptr<cgltf_data, void (*)(cgltf_data*)> gltf(parsed, cgltf_free);
        if (cgltf_load_buffers(&options, gltf.get(), name.c_str()) != cgltf_result_success) {
            return Error(ErrorCode::InvalidData, "Failed to load the buffers of glTF file '" + name + "'");
        }
        if (cgltf_validate(gltf.get()) != cgltf_result_success) {
            return Error(ErrorCode::InvalidData, "glTF file '" + name + "' is invalid");
        }

        ModelData model;
        for (cgltf_size i = 0; i < gltf->materials_count; ++i) {
            const cgltf_material& m = gltf->materials[i];
            ModelMaterial material{m.name ? m.name : "material " + std::to_string(i), glm::vec4(1.0f), std::nullopt};
            if (m.has_pbr_metallic_roughness) {
                const auto& pbr = m.pbr_metallic_roughness;
                material.baseColorFactor = glm::vec4(pbr.base_color_factor[0], pbr.base_color_factor[1],
                                                     pbr.base_color_factor[2], pbr.base_color_factor[3]);
                if (pbr.base_color_texture.texture && pbr.base_color_texture.texture->image) {
                    TRY_ASSIGN(material.baseColorTexture,
                               gltfImage(*gltf, *pbr.base_color_texture.texture->image, path, &options));
                }
            }
            model.materials.push_back(std::move(material));
        }
        const uint32_t defaultIndex = static_cast<uint32_t>(model.materials.size());
        model.materials.push_back(defaultMaterial());

        // The default scene's nodes (or, without scenes, every node), with their world transforms
        std::vector<const cgltf_node*> nodes;
        const cgltf_scene* scene = gltf->scene ? gltf->scene : (gltf->scenes_count > 0 ? &gltf->scenes[0] : nullptr);
        std::function<void(const cgltf_node*)> visit = [&](const cgltf_node* node) {
            nodes.push_back(node);
            for (cgltf_size i = 0; i < node->children_count; ++i) {
                visit(node->children[i]);
            }
        };
        if (scene) {
            for (cgltf_size i = 0; i < scene->nodes_count; ++i) {
                visit(scene->nodes[i]);
            }
        } else {
            for (cgltf_size i = 0; i < gltf->nodes_count; ++i) {
                nodes.push_back(&gltf->nodes[i]);
            }
        }

        for (const cgltf_node* node : nodes) {
            if (!node->mesh) {
                continue;
            }
            glm::mat4 world;
            cgltf_node_transform_world(node, &world[0][0]);
            const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(world)));
            // A mirroring transform turns counter-clockwise triangles clockwise
            const bool mirrored = glm::determinant(glm::mat3(world)) < 0.0f;
            const cgltf_mesh& mesh = *node->mesh;
            for (cgltf_size p = 0; p < mesh.primitives_count; ++p) {
                const cgltf_primitive& source = mesh.primitives[p];
                const std::string what = "primitive " + std::to_string(p) + " of mesh "
                                         + (mesh.name ? std::string(mesh.name) : std::to_string(cgltf_mesh_index(gltf.get(), &mesh)))
                                         + " in '" + name + "'";
                if (source.has_draco_mesh_compression) {
                    return Error(ErrorCode::InvalidData, what + " is Draco compressed, which isn't supported");
                }
                if (source.type != cgltf_primitive_type_triangles) {
                    return Error(ErrorCode::InvalidData, what + " isn't triangles (only triangle lists are supported)");
                }
                ModelPrimitive primitive;
                primitive.material = source.material ? static_cast<uint32_t>(cgltf_material_index(gltf.get(), source.material))
                                                     : defaultIndex;
                for (cgltf_size a = 0; a < source.attributes_count; ++a) {
                    const cgltf_attribute& attribute = source.attributes[a];
                    if (attribute.index != 0) {
                        continue;
                    }
                    const cgltf_accessor& accessor = *attribute.data;
                    const cgltf_size components = cgltf_num_components(accessor.type);
                    std::vector<float> values(accessor.count * components);
                    cgltf_accessor_unpack_floats(&accessor, values.data(), values.size());
                    const auto at = [&](size_t i, size_t c, float otherwise) {
                        return c < components ? values[i * components + c] : otherwise;
                    };
                    for (size_t i = 0; i < accessor.count; ++i) {
                        switch (attribute.type) {
                            case cgltf_attribute_type_position:
                                primitive.positions.push_back(glm::vec3(world * glm::vec4(at(i, 0, 0), at(i, 1, 0), at(i, 2, 0), 1.0f)));
                                break;
                            case cgltf_attribute_type_normal:
                                primitive.normals.push_back(glm::normalize(normalMatrix * glm::vec3(at(i, 0, 0), at(i, 1, 0), at(i, 2, 0))));
                                break;
                            case cgltf_attribute_type_tangent: {
                                const glm::vec3 t = glm::normalize(glm::mat3(world) * glm::vec3(at(i, 0, 0), at(i, 1, 0), at(i, 2, 0)));
                                primitive.tangents.push_back(glm::vec4(t, at(i, 3, 1.0f) * (mirrored ? -1.0f : 1.0f)));
                                break;
                            }
                            case cgltf_attribute_type_texcoord:
                                primitive.uvs.push_back(glm::vec2(at(i, 0, 0), at(i, 1, 0)));
                                break;
                            case cgltf_attribute_type_color:
                                primitive.colors.push_back(glm::vec4(at(i, 0, 0), at(i, 1, 0), at(i, 2, 0), at(i, 3, 1.0f)));
                                break;
                            default:
                                break;
                        }
                    }
                }
                if (primitive.positions.empty()) {
                    return Error(ErrorCode::InvalidData, what + " has no positions");
                }
                if (source.indices) {
                    primitive.indices.resize(source.indices->count);
                    cgltf_accessor_unpack_indices(source.indices, primitive.indices.data(), sizeof(uint32_t), primitive.indices.size());
                } else {
                    primitive.indices.resize(primitive.positions.size());
                    for (uint32_t i = 0; i < primitive.indices.size(); ++i) {
                        primitive.indices[i] = i;
                    }
                }
                primitive.indices.resize(primitive.indices.size() / 3 * 3);
                if (mirrored) {
                    for (size_t i = 0; i < primitive.indices.size(); i += 3) {
                        std::swap(primitive.indices[i + 1], primitive.indices[i + 2]);
                    }
                }
                if (!primitive.indices.empty()) {
                    model.primitives.push_back(std::move(primitive));
                }
            }
        }
        return model;
    }

    // A glTF image: a file beside it, a data URI, or bytes in a buffer view (.glb)
    static Result<ModelImage> gltfImage(const cgltf_data& gltf, const cgltf_image& image, const std::filesystem::path& path,
                                        const cgltf_options* options) {
        ModelImage result;
        result.description = "image " + std::to_string(cgltf_image_index(&gltf, &image)) + " of '" + path.string() + "'";
        if (image.buffer_view) {
            const uint8_t* bytes = cgltf_buffer_view_data(image.buffer_view);
            result.encoded.assign(bytes, bytes + image.buffer_view->size);
            return result;
        }
        if (!image.uri) {
            return Error(ErrorCode::InvalidData, result.description + " has no data");
        }
        const std::string uri = image.uri;
        if (uri.rfind("data:", 0) == 0) {
            const size_t comma = uri.find(";base64,");
            if (comma == std::string::npos) {
                return Error(ErrorCode::InvalidData, result.description + " is a data URI that isn't base64");
            }
            const char* base64 = uri.c_str() + comma + 8;
            const size_t length = std::strlen(base64);
            const size_t padding = (length >= 1 && base64[length - 1] == '=') + (length >= 2 && base64[length - 2] == '=');
            const size_t size = length / 4 * 3 - padding;
            void* decoded = nullptr;
            if (cgltf_load_buffer_base64(options, size, base64, &decoded) != cgltf_result_success) {
                return Error(ErrorCode::InvalidData, result.description + " has an invalid data URI");
            }
            result.encoded.assign(static_cast<uint8_t*>(decoded), static_cast<uint8_t*>(decoded) + size);
            free(decoded);
            return result;
        }
        std::string relative = uri;
        relative.resize(cgltf_decode_uri(relative.data()));
        result.path = path.parent_path() / relative;
        return result;
    }

    static Result<ModelData> loadObj(const std::filesystem::path& path) {
        const std::string name = path.string();
        tinyobj::attrib_t attrib;
        std::vector<tinyobj::shape_t> shapes;
        std::vector<tinyobj::material_t> materials;
        std::string warnings, errors;
        const std::string directory = path.parent_path().string() + "/";
        if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warnings, &errors, name.c_str(), directory.c_str(), true, false)) {
            return Error(ErrorCode::InvalidData, "Failed to load OBJ file '" + name + "': " + errors);
        }
        // A missing .mtl is reported as an error but still loads
        if (!errors.empty()) {
            return Error(ErrorCode::InvalidData, "OBJ file '" + name + "': " + errors);
        }

        ModelData model;
        for (const auto& m : materials) {
            ModelMaterial material{m.name, glm::vec4(m.diffuse[0], m.diffuse[1], m.diffuse[2], m.dissolve), std::nullopt};
            if (!m.diffuse_texname.empty()) {
                material.baseColorTexture = ModelImage{path.parent_path() / m.diffuse_texname, {},
                                                       "texture '" + m.diffuse_texname + "' of '" + name + "'"};
            }
            model.materials.push_back(std::move(material));
        }
        const uint32_t defaultIndex = static_cast<uint32_t>(model.materials.size());
        model.materials.push_back(defaultMaterial());

        // A primitive per shape and material. OBJ indexes each attribute separately, so vertices
        // are the distinct combinations.
        const bool hasColors = !attrib.colors.empty();
        for (const auto& shape : shapes) {
            std::map<int, ModelPrimitive> byMaterial;
            std::map<int, std::map<std::tuple<int, int, int>, uint32_t>> vertexIds;
            for (size_t face = 0; face < shape.mesh.material_ids.size(); ++face) {
                const int materialId = shape.mesh.material_ids[face];
                const uint32_t material = materialId >= 0 ? static_cast<uint32_t>(materialId) : defaultIndex;
                ModelPrimitive& primitive = byMaterial[static_cast<int>(material)];
                primitive.material = material;
                auto& ids = vertexIds[static_cast<int>(material)];
                for (size_t corner = 0; corner < 3; ++corner) {
                    const tinyobj::index_t index = shape.mesh.indices[face * 3 + corner];
                    const auto key = std::make_tuple(index.vertex_index, index.normal_index, index.texcoord_index);
                    auto found = ids.find(key);
                    if (found == ids.end()) {
                        const uint32_t id = static_cast<uint32_t>(primitive.positions.size());
                        const float* v = &attrib.vertices[3 * index.vertex_index];
                        primitive.positions.push_back({v[0], v[1], v[2]});
                        if (index.normal_index >= 0) {
                            const float* n = &attrib.normals[3 * index.normal_index];
                            primitive.normals.push_back({n[0], n[1], n[2]});
                        }
                        if (index.texcoord_index >= 0) {
                            const float* t = &attrib.texcoords[2 * index.texcoord_index];
                            primitive.uvs.push_back({t[0], 1.0f - t[1]});
                        }
                        if (hasColors) {
                            const float* c = &attrib.colors[3 * index.vertex_index];
                            primitive.colors.push_back({c[0], c[1], c[2], 1.0f});
                        }
                        found = ids.emplace(key, id).first;
                    }
                    primitive.indices.push_back(found->second);
                }
            }
            for (auto& [material, primitive] : byMaterial) {
                // An attribute some corners lack is one the primitive lacks
                if (primitive.normals.size() != primitive.positions.size()) {
                    primitive.normals.clear();
                }
                if (primitive.uvs.size() != primitive.positions.size()) {
                    primitive.uvs.clear();
                }
                model.primitives.push_back(std::move(primitive));
            }
        }
        return model;
    }
};

// A model on the device: a mesh per primitive and, per material, its base colour factor and texture
struct Model {
    std::vector<Handle<Mesh>> meshes;                  // Instance index: the mesh's material
    std::vector<glm::vec4> baseColorFactors;           // One per material
    std::vector<Handle<Image>> baseColorTextures;      // One per material (white without a texture)
};

// Uploads a model, converting each primitive to vertices (as many as it has positions) with
// toVertices, which fails when it can't (e.g. the vertex format needs an attribute the file lacks).
// Indices are 16-bit when the vertices allow. Textures are sRGB; one image several materials use is
// loaded once.
template<typename Vertex>
Result<Model> uploadModel(VulkanContext& context, const ModelData& data,
                          const std::function<Result<std::vector<Vertex>>(const ModelPrimitive&)>& toVertices) {
    Model model;
    for (const ModelPrimitive& primitive : data.primitives) {
        TRY_ASSIGN(auto vertices, toVertices(primitive));
        if (vertices.size() != primitive.positions.size()) {
            contractViolation("uploadModel's conversion must give a vertex per position");
        }
        auto upload = [&]() -> Result<Handle<Mesh>> {
            if (vertices.size() <= 65536) {
                const std::vector<uint16_t> indices(primitive.indices.begin(), primitive.indices.end());
                return Mesh::create(context, vertices, indices, primitive.material);
            }
            return Mesh::create(context, vertices, primitive.indices, primitive.material);
        };
        TRY_ASSIGN(auto uploaded, upload());
        model.meshes.push_back(std::move(uploaded));
    }

    std::map<std::string, Handle<Image>> loaded;
    Handle<Image> white;
    for (const ModelMaterial& material : data.materials) {
        model.baseColorFactors.push_back(material.baseColorFactor);
        if (!material.baseColorTexture) {
            if (!white) {
                uint8_t pixel[4] = {255, 255, 255, 255};
                TRY_ASSIGN(white, Image::createFromUcharBuffer(context, pixel, 1, 1, 4, ColorSpace::Srgb));
            }
            model.baseColorTextures.push_back(white);
            continue;
        }
        const ModelImage& image = *material.baseColorTexture;
        const std::string key = image.path.empty() ? image.description : image.path.string();
        auto found = loaded.find(key);
        if (found == loaded.end()) {
            Result<Handle<Image>> texture = image.path.empty()
                ? Image::createFromEncoded(context, image.encoded.data(), image.encoded.size(), ColorSpace::Srgb, image.description)
                : Image::createFromFile(context, image.path, ColorSpace::Srgb);
            TRY_ASSIGN(auto created, std::move(texture).withContext("loading the base colour texture of material '" + material.name + "'"));
            found = loaded.emplace(key, std::move(created)).first;
        }
        model.baseColorTextures.push_back(found->second);
    }
    return model;
}
