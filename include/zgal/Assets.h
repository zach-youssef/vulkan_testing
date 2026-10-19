#pragma once

#include "core/Result.h"
#include "core/VulkanContext.h"
#include "graph/RenderGraph.h"
#include "resources/Model.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace zgal {

// A graph's model: the file at path (found with VulkanContext::findAsset), loaded and uploaded in
// Vertex's format, which generated code converts to (Vertex::fromModel). what names it in errors.
template<typename Vertex>
Result<Model> loadModel(VulkanContext& context, const std::filesystem::path& path, const char* what) {
    const std::string loading = std::string("loading ") + what;
    TRY_ASSIGN(auto found, context.findAsset(path).withContext(loading));
    TRY_ASSIGN(auto data, ModelData::load(found).withContext(loading));
    return uploadModel<Vertex>(context, data, &Vertex::fromModel).withContext(loading + " ('" + found.string() + "')");
}

// Imports images (e.g. a model's textures, which materials share) into a graph, each distinct image
// once, named name and its first index: the ids, one per image given
inline std::vector<ImageId> importImages(RenderGraph& graph, const std::string& name, const std::vector<Handle<Image>>& images,
                                         ImageState resting) {
    std::vector<ImageId> ids;
    std::map<const Image*, ImageId> imported;
    for (size_t i = 0; i < images.size(); ++i) {
        auto found = imported.find(images[i].get());
        if (found == imported.end()) {
            found = imported.emplace(images[i].get(), graph.importImage(name + " " + std::to_string(i), images[i], resting)).first;
        }
        ids.push_back(found->second);
    }
    return ids;
}

} // namespace zgal
