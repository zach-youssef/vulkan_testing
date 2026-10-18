#define VK_WRAP_UTIL_IMPL

#include "app/VulkanApp.h"
#include "materials/Renderable.h"
#include "descriptors/DescriptorHeap.h"
#include "materials/GraphicsMaterial.h"
#include "graph/RenderGraph.h"
#include "graph/nodes/DispatchNode.h"
#include "graph/nodes/RenderablesNode.h"
#include "shaders/ShaderLibrary.h"
#include "resources/Image.h"
#include "zgal/Window.h"

// Generated from simple.zgal, in namespace simple: Camera, Vertex, the Tutorial and CopyTexture
// materials and SimpleApp
#include "simple.zgal.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>


// Defined by the Makefile
#ifndef SIMPLE_EXAMPLE_TEXTURE_DIR
#error "SIMPLE_EXAMPLE_TEXTURE_DIR must be defined to the directory containing the example's textures"
#endif
const std::string textureDir = SIMPLE_EXAMPLE_TEXTURE_DIR;

// Draws a texture with a rotating camera (Tutorial in simple.zgal), at the app's angle. Owns the
// camera's per-frame uniform buffers, which update() writes.
class TutorialMaterial : public simple::Tutorial {
public:
    using Mapping = Buffer<simple::Camera>::Mapping;

    // Loads the shaders and creates the material's per-frame resources, which can fail, then the
    // material
    static Result<Handle<TutorialMaterial>> create(VulkanContext& context,
                                                   const simple::SimpleApp& app,
                                                   AttachmentFormats formats,
                                                   FrameBinding<DescriptorEntry> texture,
                                                   FrameBinding<DescriptorEntry> sampler) {
        TRY_ASSIGN(auto shaders, loadShaders(context));
        // A uniform buffer per frame (read by the vertex shader as a storage buffer), and its
        // descriptor entries and mappings. They are created in that order, so they are also resized
        // in that order when the number of frames in flight changes.
        TRY_ASSIGN(auto buffers, PerFrameHandles<Buffer<simple::Camera>>::createManaged(context, [&context](uint32_t) {
            return Buffer<simple::Camera>::create(context, 1, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }));
        auto entries = context.resources().create<PerFrameHandles<DescriptorEntry>>(
            context, [ctx = &context, buffers](uint32_t frameIdx) {
                return ctx->descriptors().storageBuffer((*buffers)[frameIdx]);
            });
        TRY_ASSIGN(auto mappings, PerFrame<Mapping>::create(context, [buffers](uint32_t frameIdx) {
            return (*buffers)[frameIdx]->getPersistentMapping(0, sizeof(simple::Camera));
        }));
        return context.resources().create<TutorialMaterial>(context, app, shaders, std::move(formats), std::move(texture),
                                                            std::move(sampler), std::move(buffers), std::move(entries),
                                                            std::move(mappings));
    }

    TutorialMaterial(VulkanContext& context,
                     const simple::SimpleApp& app,
                     const Shaders& shaders,
                     AttachmentFormats formats,
                     FrameBinding<DescriptorEntry> texture,
                     FrameBinding<DescriptorEntry> sampler,
                     Handle<PerFrameHandles<Buffer<simple::Camera>>> uniformBuffers,
                     Handle<PerFrameHandles<DescriptorEntry>> uniformEntries,
                     PerFrame<Mapping> mappedUniformBuffers)
    : simple::Tutorial(context, shaders,
               // The bindings, by name: the generated class gives each its access and stages
               Bindings{.camera = uniformEntries, .albedo = std::move(texture), .albedoSampler = std::move(sampler)},
               std::move(formats), GraphicsPipelineOptions{}),
      app_(&app),
      uniformBuffers_(std::move(uniformBuffers)),
      uniformEntries_(std::move(uniformEntries)),
      mappedUniformBuffers_(std::move(mappedUniformBuffers)) {}

    void update(uint32_t currentImage, VkExtent2D swapChainExtent) override {
        auto model = glm::rotate(glm::mat4(1.0f), app_->state().angle, glm::vec3(0.0f, 0.0f, 1.0f));
        auto view =
        glm::lookAt(glm::vec3(2.0f, 2.0f, 2.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        auto projection = glm::perspective(glm::radians(45.0f), swapChainExtent.width / (float) swapChainExtent.height, 0.1f, 10.0f);

        // GLM was originally designed for OpenGL where the Y coordinate of the clip coordinates is inverted
        projection[1][1] *= -1;

        const simple::Camera camera{.mvp = projection * view * model};
        memcpy(mappedUniformBuffers_[currentImage].get(), &camera, sizeof(camera));
    }

private:
    const simple::SimpleApp* app_;
    Handle<PerFrameHandles<Buffer<simple::Camera>>> uniformBuffers_;
    Handle<PerFrameHandles<DescriptorEntry>> uniformEntries_;
    PerFrame<Mapping> mappedUniformBuffers_;
};

// Copies a texture into a storage image (CopyTexture in simple.zgal), one invocation per pixel
class CopyTextureMaterial : public simple::CopyTexture {
public:
    CopyTextureMaterial(VulkanContext& context, const Shaders& shaders, Bindings bindings,
                        uint32_t imageWidth, uint32_t imageHeight)
    : simple::CopyTexture(context, shaders, std::move(bindings)), imageWidth_(imageWidth), imageHeight_(imageHeight) {}

    void update(uint32_t, VkExtent2D) override {}

    glm::vec3 getDispatchDimensions() override {
        // Rounded up to whole workgroups (the workgroup size is in simple.zgal)
        return workgroupsFor(imageWidth_, imageHeight_);
    }

private:
    uint32_t imageWidth_;
    uint32_t imageHeight_;
};

const std::vector<simple::Vertex> vertexData = {
    {{-0.5f, -0.5f},{1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
    {{0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
    {{0.5f, 0.5f},  {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
    {{-0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f}}
};
const std::vector<uint16_t> indexData = {
    0, 1, 2,
    2, 3, 0
};

std::vector<simple::Vertex> transformVertices(const std::vector<simple::Vertex>& vertices, float scale, glm::vec2 offset) {
    std::vector<simple::Vertex> transformed = vertices;
    for (auto& vertex : transformed) {
        vertex.position = vertex.position * scale + offset;
    }
    return transformed;
}

Result<Handle<Renderable>> createTutorialRenderable(const std::vector<simple::Vertex>& vertices,
                                                    Handle<Material> material,
                                                    VulkanApp& app) {
    auto& context = app.getContext();
    TRY_ASSIGN(auto mesh, Mesh::create(context, vertices, indexData));
    return context.resources().create<MeshRenderable>(std::move(mesh), std::move(material));
}

// Sets up the app and runs it until the window is closed. Errors (a missing file, a failed Vulkan
// call, a graph that doesn't compile) come back as the result.
Result<void> runExample() {
    // Initialize Window & Vulkan
    AppConfig config;
    config.applicationName = "Simple example";
    config.window = WindowConfig{"Simple example", 800, 600};
    config.enableValidation = true;
    // The SPIR-V the Makefile compiles is found beside the executable (shaders/, the default search
    // path); rebuilding it (make shaders) while the example runs reloads it
    config.autoReloadShaders = true;

    VulkanApp app;
    TRY(app.init(config));
    auto& context = app.getContext();
    auto& descriptors = context.descriptors();
    
    // Load texture file, and create a sampler
    TRY_ASSIGN(auto texture, Image::createFromFile(context, textureDir + "/texture.jpg", ColorSpace::Srgb));
    TRY_ASSIGN(auto sampler, VulkanSampler::createWithAddressMode(context, VK_SAMPLER_ADDRESS_MODE_REPEAT));
    auto textureEntry = descriptors.sampledImage(texture);
    auto samplerEntry = descriptors.sampler(sampler);

    // The render graph's resources: the texture, a copy of it per frame in flight (so frame N+1's
    // copy can overlap frame N's drawing) and the window's surface
    RenderGraph& graph = app.createGraph();
    graph.importImage("texture", texture, ImageState::shaderReadOnly());
    // Linear half floats: storage images cannot be sRGB, and 8 bits would band in linear space
    ImageId copied = graph.createImage("copied texture", FrameMode::PerFrame, VK_FORMAT_R16G16B16A16_SFLOAT,
                                       FixedExtent{{texture->getWidth(), texture->getHeight()}});
    RenderTarget screen = graph.surfaceTarget();

    // The app (SimpleApp in simple.zgal): the rotation's state, which its handlers advance each
    // frame and pause on space. It has no graph of its own: this one is built here.
    TRY_ASSIGN(auto simpleApp, simple::SimpleApp::create(context, graph, simple::SimpleApp::Inputs{
        .timestep = zgal::Timestep::realTime(0.25f),
    }));

    // A compute material copying the texture, and a material drawing the copy, shared by two
    // renderables, both from simple.zgal. Their bindings tell the graph which resources they use,
    // and how: the generated classes give each binding the access and stages its shaders use.
    TRY_ASSIGN(auto copyShaders, simple::CopyTexture::loadShaders(context));
    auto computeMaterial = context.resources().create<CopyTextureMaterial>(
        context, copyShaders,
        simple::CopyTexture::Bindings{.source = textureEntry, .sourceSampler = samplerEntry, .copy = graph.storageEntry(copied)},
        texture->getWidth(), texture->getHeight());
    TRY_ASSIGN(auto material, TutorialMaterial::create(context, *simpleApp, graph.formats(screen), graph.sampledEntry(copied),
                                                       samplerEntry));

    std::vector<Handle<Renderable>> renderables;
    for (glm::vec2 offset : {glm::vec2{-0.3f, 0.0f}, glm::vec2{0.3f, 0.0f}}) {
        TRY_ASSIGN(auto renderable, createTutorialRenderable(transformVertices(vertexData, 0.5f, offset), material, app));
        renderables.push_back(std::move(renderable));
    }

    // Nodes. The graph orders them, picks queues and synchronizes them from the materials'
    // bindings, and acquires and presents the surface around the draw.
    graph.add<DispatchNode>("copy texture", computeMaterial);
    graph.add<RenderablesNode>("draw quads", std::move(renderables), screen);

    // Run: the window's input goes to the app's handlers, and each frame runs its frame handler
    // before drawing the graph
    return zgal::run(app, *simpleApp);
}

int main() {
    return exitStatus(runExample());
}
