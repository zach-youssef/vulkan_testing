#define VK_WRAP_UTIL_IMPL

#include "app/VulkanApp.h"
#include "materials/Renderable.h"
#include "descriptors/DescriptorHeap.h"
#include "resources/Ubo.h"
#include "materials/GraphicsMaterial.h"
#include "resources/Ubo.h"
#include "graph/RenderGraph.h"
#include "graph/nodes/DispatchNode.h"
#include "graph/nodes/RenderablesNode.h"
#include "shaders/ShaderLibrary.h"
#include "resources/Image.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>


// Defined by the Makefile
#ifndef SIMPLE_EXAMPLE_SHADER_DIR
#error "SIMPLE_EXAMPLE_SHADER_DIR must be defined to the directory containing the compiled shaders"
#endif
#ifndef SIMPLE_EXAMPLE_TEXTURE_DIR
#error "SIMPLE_EXAMPLE_TEXTURE_DIR must be defined to the directory containing the example's textures"
#endif
const std::string shaderDir = SIMPLE_EXAMPLE_SHADER_DIR;
const std::string textureDir = SIMPLE_EXAMPLE_TEXTURE_DIR;

struct Vertex {
    glm::vec2 pos;
    glm::vec3 color;
    glm::vec2 texCoord0;
    
    // One interleaved buffer of these, read by shader.vert's inputs at locations 0-2
    static VertexLayout layout() {
        return VertexLayout::interleaved(sizeof(Vertex), {
            {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, pos)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, texCoord0)},
        });
    }
};

// Draws a texture with a rotating camera. Its shaders (shader.vert, shader.frag) find a uniform
// buffer, the texture and a sampler by the indices of its bindings, in that order.
class TutorialMaterial : public GraphicsMaterial {
public:
    TutorialMaterial(VulkanContext& context,
                     AttachmentFormats formats,
                     FrameBinding<DescriptorEntry> texture,
                     FrameBinding<DescriptorEntry> sampler,
                     Handle<Shader> vertexShader,
                     Handle<Shader> fragmentShader)
    :  GraphicsMaterial(context,
                        {},
                        std::move(formats),
                        std::move(vertexShader),
                        std::move(fragmentShader),
                        GraphicsPipelineOptions{.vertexLayout = Vertex::layout()}) {
        // A uniform buffer per frame (read by the vertex shader as a storage buffer), and its
        // descriptor entries and mappings. They are created in that order, so they are also resized
        // in that order when the number of frames in flight changes.
        uniformBuffers_ = context.resources().create<PerFrameHandles<Buffer<UniformBufferObject>>>(
            context, [ctx = &context](uint32_t) { return createUniformBuffer(*ctx); });
        uniformEntries_ = context.resources().create<PerFrameHandles<DescriptorEntry>>(
            context, [ctx = &context, buffers = uniformBuffers_](uint32_t frameIdx) {
                return ctx->descriptors().storageBuffer((*buffers)[frameIdx]);
            });
        mappedUniformBuffers_ = PerFrame<Mapping>(context, [this](uint32_t frameIdx) {
            return (*uniformBuffers_)[frameIdx]->getPersistentMapping(0, sizeof(UniformBufferObject));
        });

        bindings_ = {
            MaterialBinding::storageBuffer(uniformEntries_, Access::StorageBufferRead, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT),
            MaterialBinding::sampled(std::move(texture), VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT),
            MaterialBinding::sampler(std::move(sampler)),
        };
    }
    
    void update(uint32_t currentImage, VkExtent2D swapChainExtent) {
        static auto startTime = std::chrono::high_resolution_clock::now();
        
        auto currentTime = std::chrono::high_resolution_clock::now();
        float time = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - startTime).count();
        
        auto model = glm::rotate(glm::mat4(1.0f), time * glm::radians(90.f), glm::vec3(0.0f, 0.0f, 1.0f));
        auto view =
        glm::lookAt(glm::vec3(2.0f, 2.0f, 2.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        auto projection = glm::perspective(glm::radians(45.0f), swapChainExtent.width / (float) swapChainExtent.height, 0.1f, 10.0f);
        
        // GLM was originally designed for OpenGL where the Y coordinate of the clip coordinates is inverted
        projection[1][1] *= -1;

        auto ubo = UniformBufferObject::fromModelViewProjection(model, view, projection);
        
        memcpy(mappedUniformBuffers_[currentImage].get(), &ubo, sizeof(ubo));
    }
private:
    static Handle<Buffer<UniformBufferObject>> createUniformBuffer(VulkanContext& context) {
        Handle<Buffer<UniformBufferObject>> buffer;
        VK_SUCCESS_OR_THROW(Buffer<UniformBufferObject>::create(buffer,
                                                                context,
                                                                1,
                                                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
                            "Failed to create uniform buffer.");
        return buffer;
    }
private:
    using Mapping = std::unique_ptr<UniformBufferObject, std::function<void(UniformBufferObject*)>>;
    Handle<PerFrameHandles<Buffer<UniformBufferObject>>> uniformBuffers_;
    Handle<PerFrameHandles<DescriptorEntry>> uniformEntries_;
    PerFrame<Mapping> mappedUniformBuffers_;
};

// Copies a texture into a storage image (test.comp)
class TestComputeMat : public ComputeMaterial {
public:
    TestComputeMat(VulkanContext& context,
                   Handle<Shader> computeShader,
                   std::vector<MaterialBinding> bindings,
                   uint32_t imageWidth, uint32_t imageHeight)
    : ComputeMaterial(context, std::move(bindings), std::move(computeShader)),
    imageWidth_(imageWidth),
    imageHeight_(imageHeight){}
    
    void update(uint32_t currentImage, VkExtent2D swapChainExtent) {
        // no-op
    }
    
    glm::vec3 getDispatchDimensions() {
        // "32" here matches layout size defined in shader
        return glm::vec3{imageWidth_ / 32u, imageHeight_ / 32u, 1};
    }
    
private:
    uint32_t imageWidth_;
    uint32_t imageHeight_;
};

const std::vector<Vertex> vertexData = {
    {{-0.5f, -0.5f},{1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
    {{0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
    {{0.5f, 0.5f},  {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
    {{-0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f}}
};
const std::vector<uint16_t> indexData = {
    0, 1, 2,
    2, 3, 0
};

std::vector<Vertex> transformVertices(const std::vector<Vertex>& vertices, float scale, glm::vec2 offset) {
    std::vector<Vertex> transformed = vertices;
    for (auto& vertex : transformed) {
        vertex.pos = vertex.pos * scale + offset;
    }
    return transformed;
}

void createTutorialRenderable(Handle<MeshRenderable<Vertex>>& outPtr,
                              const std::vector<Vertex>& vertices,
                              Handle<Material> material,
                              VulkanApp& app) {
    auto& resourceManager = app.getContext().resources();
    outPtr = resourceManager.create<MeshRenderable<Vertex>>(vertices, indexData,
                                                            std::move(material),
                                                            app.getContext());
}

int main() {
    // Initialize Window & Vulkan
    AppConfig config;
    config.applicationName = "Simple example";
    config.window = WindowConfig{"Simple example", 800, 600};
    config.enableValidation = true;
    // SPIR-V compiled by the Makefile; rebuilding it (make shaders) while the example runs reloads it
    config.shaderSearchPaths = {shaderDir};
    config.autoReloadShaders = true;

    VulkanApp app;
    app.init(config);
    auto& context = app.getContext();
    auto& descriptors = context.descriptors();
    
    // Load texture file, and create a sampler
    Handle<Image> texture;
    Image::createFromFile(texture,
                          context,
                          textureDir + "/texture.jpg");
    Handle<VulkanSampler> sampler;
    VK_SUCCESS_OR_THROW(VulkanSampler::createWithAddressMode(sampler,
                                                             context,
                                                             VK_SAMPLER_ADDRESS_MODE_REPEAT),
                        "Failed to create sampler");
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

    // A compute material copying the texture, and a material drawing the copy, shared by two
    // renderables. Their bindings tell the graph which resources they use, and how.
    auto computeMaterial = context.resources().create<TestComputeMat>(
        context, context.shaders().get("test.comp"),
        std::vector<MaterialBinding>{
            MaterialBinding::sampled(textureEntry),
            MaterialBinding::sampler(samplerEntry),
            MaterialBinding::storageImage(graph.storageEntry(copied), Access::StorageWrite),
        },
        texture->getWidth(), texture->getHeight());
    auto material = context.resources().create<TutorialMaterial>(context, graph.formats(screen),
                                                                 graph.sampledEntry(copied), samplerEntry,
                                                                 context.shaders().get("shader.vert"),
                                                                 context.shaders().get("shader.frag"));

    std::vector<Handle<Renderable>> renderables;
    for (glm::vec2 offset : {glm::vec2{-0.3f, 0.0f}, glm::vec2{0.3f, 0.0f}}) {
        Handle<MeshRenderable<Vertex>> renderable;
        createTutorialRenderable(renderable, transformVertices(vertexData, 0.5f, offset), material, app);
        renderables.push_back(std::move(renderable));
    }

    // Nodes. The graph orders them, picks queues and synchronizes them from the materials'
    // bindings, and acquires and presents the surface around the draw.
    graph.add<DispatchNode>("copy texture", computeMaterial);
    graph.add<RenderablesNode>("draw quads", std::move(renderables), screen);
    app.setActiveGraph(graph);

    // Run
    try {
        app.run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
