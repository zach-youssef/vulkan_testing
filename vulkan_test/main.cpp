#define VK_WRAP_UTIL_IMPL

#include "VulkanApp.h"
#include "Renderable.h"
#include "Ubo.h"
#include "BasicMaterial.h"
#include "RenderGraph.h"
#include "ComputeNode.h"
#include "RenderablesNode.h"
#include "PresentNode.h"
#include "AcquireImageNode.h"
#include "FileUtil.h"
#include "Image.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

const uint32_t WINDOW_WIDTH = 800;
const uint32_t WINDOW_HEIGHT = 600;
const int MAX_FRAMES_IN_FLIGHT = 2;

struct Vertex {
    glm::vec2 pos;
    glm::vec3 color;
    glm::vec2 texCoord0;
    
    static VkVertexInputBindingDescription getBindingDescription() {
        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;
        bindingDescription.stride = sizeof(Vertex);
        // Move to next data entry after each vertex (alternative is after each instance)
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return bindingDescription;
    }
    
    static std::array<VkVertexInputAttributeDescription, 3> getAttributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions{};
        
        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[0].offset = offsetof(Vertex, pos);
        
        attributeDescriptions[1].binding = 0;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[1].offset = offsetof(Vertex, color);
        
        attributeDescriptions[2].binding = 0;
        attributeDescriptions[2].location = 2;
        attributeDescriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[2].offset = offsetof(Vertex, texCoord0);

        return attributeDescriptions;
    }
};

class TutorialMaterial : public BasicMaterial<MAX_FRAMES_IN_FLIGHT, 3> {
public:
    TutorialMaterial(VulkanContext& context,
                     VkDevice device,
                     VkPhysicalDevice physicalDevice,
                     VkExtent2D swapchainExtent,
                     VkRenderPass renderPass,
                     std::vector<Handle<Descriptor>> descriptors,
                     const std::vector<char>& vertSpirv,
                     const std::vector<char>& fragSpirv)
    :  BasicMaterial<MAX_FRAMES_IN_FLIGHT, 3>(device,
                                              physicalDevice,
                                              descriptors,
                                              swapchainExtent,
                                              renderPass,
                                              vertSpirv,
                                              fragSpirv,
                                              Vertex::getBindingDescription(),
                                              Vertex::getAttributeDescriptions()){
        // JANK: Find the first uniform buffer object descriptor
        using UboDesc = UniformBufferDescriptor<UniformBufferObject, MAX_FRAMES_IN_FLIGHT>;
        Handle<UboDesc> uboDesc = nullptr;
        for (auto& descriptor : Material<MAX_FRAMES_IN_FLIGHT>::descriptors_) {
            if (descriptor->getType() == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                uboDesc = static_handle_cast<UboDesc>(descriptor);
                break;
            }
        }
        
        // Create & bind our uniform buffers
        for (uint frameIdx = 0; frameIdx < MAX_FRAMES_IN_FLIGHT; ++frameIdx) {
            createUniformBuffer(context, frameIdx);
            uboDesc->bindBuffer(frameIdx, uniformBuffers_.at(frameIdx));
            populateDescriptorSet(frameIdx);
        }
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
    void createUniformBuffer(VulkanContext& context, uint32_t frameIndex) {
        VK_SUCCESS_OR_THROW(Buffer<UniformBufferObject>::create(uniformBuffers_[frameIndex],
                                                                context,
                                                                1,
                                                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT),
                            "Failed to create uniform buffer.");
        mappedUniformBuffers_[frameIndex] = uniformBuffers_[frameIndex]->getPersistentMapping(0, sizeof(UniformBufferObject));
    }
private:
    std::array<Handle<Buffer<UniformBufferObject>>, MAX_FRAMES_IN_FLIGHT> uniformBuffers_;
    std::array<std::unique_ptr<UniformBufferObject, std::function<void(UniformBufferObject*)>>, MAX_FRAMES_IN_FLIGHT> mappedUniformBuffers_;
};

class TestComputeMat : public ComputeMaterial<MAX_FRAMES_IN_FLIGHT> {
public:
    TestComputeMat(const std::vector<char>& computeShaderCode,
                   std::vector<Handle<Descriptor>> descriptors,
                   uint32_t imageWidth, uint32_t imageHeight,
                   VkDevice device,
                   VkPhysicalDevice physicalDevice)
    : ComputeMaterial<MAX_FRAMES_IN_FLIGHT>(device, physicalDevice, descriptors, computeShaderCode),
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

void createTestComputeMaterial(Handle<TestComputeMat>& outPtr,
                               std::array<Handle<Image>, MAX_FRAMES_IN_FLIGHT> inImages,
                               std::array<Handle<Image>, MAX_FRAMES_IN_FLIGHT> outImages,
                               uint32_t width, uint32_t height,
                               VulkanApp<MAX_FRAMES_IN_FLIGHT>& app){
    const static std::string shaderPath = "/Users/zyoussef/code/vulkan_test/vulkan_test/shaders";
    auto computeShaderCode = readFile(shaderPath + "/compTest.spv");
    
    std::vector<Handle<Descriptor>> descriptors;
    auto& resourceManager = app.getResourceManager();
    descriptors.push_back(resourceManager.create<StorageImageDescriptor<MAX_FRAMES_IN_FLIGHT>>(VK_SHADER_STAGE_COMPUTE_BIT, inImages));
    descriptors.push_back(resourceManager.create<StorageImageDescriptor<MAX_FRAMES_IN_FLIGHT>>(VK_SHADER_STAGE_COMPUTE_BIT, outImages));

    outPtr = resourceManager.create<TestComputeMat>(computeShaderCode,
                                                    descriptors,
                                                    width, height,
                                                    app.getDevice(),
                                                    app.getPhysicalDevice());
}

void createTutorialMaterial(Handle<TutorialMaterial>& outPtr,
                            std::array<Handle<Image>, MAX_FRAMES_IN_FLIGHT> images,
                            Handle<VulkanSampler> sampler,
                            VulkanApp<MAX_FRAMES_IN_FLIGHT>& app) {
    const static std::string shaderPath = "/Users/zyoussef/code/vulkan_test/vulkan_test/shaders";
    auto vertShaderCode = readFile(shaderPath + "/vert.spv");
    auto fragShaderCode = readFile(shaderPath + "/frag.spv");
    
    std::vector<Handle<Descriptor>> descriptors;
    auto& resourceManager = app.getResourceManager();
    descriptors.push_back(resourceManager.create<UniformBufferDescriptor<UniformBufferObject, MAX_FRAMES_IN_FLIGHT>>(std::array<Handle<Buffer<UniformBufferObject>>, MAX_FRAMES_IN_FLIGHT>{},
                                                                                                                     VK_SHADER_STAGE_VERTEX_BIT));
    descriptors.push_back(resourceManager.create<CombinedImageSamplerDescriptor<MAX_FRAMES_IN_FLIGHT>>(VK_SHADER_STAGE_FRAGMENT_BIT, images, sampler,
                                                                                                       VK_IMAGE_LAYOUT_GENERAL));

    outPtr = resourceManager.create<TutorialMaterial>(app.getContext(),
                                                      app.getDevice(),
                                                      app.getPhysicalDevice(),
                                                      app.getSwapchainExtent(),
                                                      app.getRenderPass(),
                                                      descriptors,
                                                      vertShaderCode,
                                                      fragShaderCode);
}

void createTutorialRenderable(Handle<MeshRenderable<Vertex, MAX_FRAMES_IN_FLIGHT>>& outPtr,
                              const std::vector<Vertex>& vertices,
                              Handle<Material<MAX_FRAMES_IN_FLIGHT>> material,
                              VulkanApp<MAX_FRAMES_IN_FLIGHT>& app) {
    auto& resourceManager = app.getResourceManager();
    outPtr = resourceManager.create<MeshRenderable<Vertex, MAX_FRAMES_IN_FLIGHT>>(vertices, indexData,
                                                                                  std::move(material),
                                                                                  app.getContext());
}

int main() {
    VulkanApp<MAX_FRAMES_IN_FLIGHT> app(WINDOW_HEIGHT, WINDOW_WIDTH);
    
    // Initialize Window & Vulkan
    app.init(false /* forceFullFloatColor */, true /* enableValidation */);
    
    // Load texture file
    Handle<Image> texture;
    Image::createFromFile(texture,
                          app.getContext(),
                          "/Users/zyoussef/code/vulkan_test/vulkan_test/textures/texture.jpg");
    // The texture is read by the compute shader as a storage image, which must be in the GENERAL layout
    texture->transitionLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    std::array<Handle<Image>, MAX_FRAMES_IN_FLIGHT> inputImages = {texture, texture};

    // Create output texture images
    std::array<Handle<Image>, MAX_FRAMES_IN_FLIGHT> outImages;
    for (int frameIdx = 0; frameIdx < MAX_FRAMES_IN_FLIGHT; ++frameIdx) {
        Image::createEmptyRGBA(outImages[frameIdx],
                               app.getContext(),
                               texture->getWidth(),
                               texture->getHeight());
        // Written by the compute shader as a storage image and then sampled by the fragment
        // shader, so keep it in the GENERAL layout (valid for both uses)
        outImages[frameIdx]->transitionLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    }
    

    // Create texture sampler
    Handle<VulkanSampler> sampler;
    VulkanSampler::createWithAddressMode(sampler,
                                         app.getContext(),
                                         VK_SAMPLER_ADDRESS_MODE_REPEAT);
    

    // Create a single material shared by two renderables
    Handle<TutorialMaterial> material;
    createTutorialMaterial(material, outImages, sampler, app);
    
    std::vector<Handle<Renderable<MAX_FRAMES_IN_FLIGHT>>> renderables;
    for (glm::vec2 offset : {glm::vec2{-0.3f, 0.0f}, glm::vec2{0.3f, 0.0f}}) {
        Handle<MeshRenderable<Vertex, MAX_FRAMES_IN_FLIGHT>> renderable;
        createTutorialRenderable(renderable, transformVertices(vertexData, 0.5f, offset), material, app);
        renderables.push_back(std::move(renderable));
    }
    
    
    // Create compute material
    Handle<TestComputeMat> computeMaterial;
    createTestComputeMaterial(computeMaterial,
                              inputImages,
                              outImages,
                              texture->getWidth(),
                              texture->getHeight(),
                              app);
    
    // Instantiate our render graph
    std::unique_ptr<RenderGraph<MAX_FRAMES_IN_FLIGHT>> renderGraph = std::make_unique<RenderGraph<MAX_FRAMES_IN_FLIGHT>>(app.getDevice());
    
    // Add Nodes
    auto acquireImageNode = renderGraph->addNode(std::make_unique<AcquireImageNode<MAX_FRAMES_IN_FLIGHT>>(app.getDevice()));
    auto computeNode = renderGraph->addNode(std::make_unique<ComputeNode<MAX_FRAMES_IN_FLIGHT>>(std::move(computeMaterial),
                                                                                               app.getDevice(),
                                                                                               app.getComputeQueue(),
                                                                                               app.getComputeCommandBuffers()));
    auto graphicsNode = renderGraph->addNode(std::make_unique<RenderablesNode<MAX_FRAMES_IN_FLIGHT>>(std::move(renderables),
                                                                                                     app.getDevice(),
                                                                                                     app.getGraphicsQueue(),
                                                                                                     app.getRenderPass(),
                                                                                                     app.getGraphicsCommandBuffers()));
    auto presentNode = renderGraph->addNode(std::make_unique<PresentNode<MAX_FRAMES_IN_FLIGHT>>(app.getDevice(),
                                                                                                app.getPresentQueue()));
    
    // Setup Edges
    renderGraph->addEdge(acquireImageNode, graphicsNode);
    renderGraph->addEdge(computeNode, graphicsNode);
    renderGraph->addEdge(graphicsNode, presentNode);
    renderGraph->flagNodeAsFrameBlocking(graphicsNode);

    // Set on app
    app.setRenderGraph(std::move(renderGraph));

    // Run
    try {
        app.run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
