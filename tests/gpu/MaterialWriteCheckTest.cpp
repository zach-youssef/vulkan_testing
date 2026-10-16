#include "VkTestContext.h"
#include "TestShaders.h"
#include "graph/RenderGraph.h"
#include "graph/nodes/DispatchNode.h"
#include "descriptors/DescriptorHeap.h"

#include <filesystem>
#include <unistd.h>

namespace {

// Dispatches a shader once with whatever bindings a test gives it
class AnyComputeMaterial : public ComputeMaterial {
public:
    AnyComputeMaterial(VulkanContext& context, std::vector<MaterialBinding> bindings, Handle<Shader> shader)
    : ComputeMaterial(context, std::move(bindings), std::move(shader)) {}

    void update(uint32_t, VkExtent2D) override {}
    glm::vec3 getDispatchDimensions() override { return {1, 1, 1}; }
};

Handle<Buffer<uint32_t>> storageBuffer(VulkanContext& ctx) {
    return Buffer<uint32_t>::create(ctx, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT).value();
}

} // namespace

TEST_CASE_FIXTURE(GpuTest, "The graph rejects materials whose shaders write what their bindings don't declare") {
    auto& ctx = context();
    auto& heap = ctx.descriptors();
    auto image = Image::createStorage(ctx, VK_FORMAT_R8G8B8A8_UNORM, 8, 8).value();
    auto buffer = storageBuffer(ctx);

    // pattern.comp stores to its image, which a read can't cover: no barrier would order the write
    {
        RenderGraph graph(ctx);
        graph.importImage("image", image, ImageState::general());
        graph.add<DispatchNode>("pattern", ctx.resources().create<AnyComputeMaterial>(ctx, std::vector<MaterialBinding>{
            MaterialBinding::storageImage(heap.storageImage(image), Access::StorageRead),
        }, testShader(ctx, "pattern.comp")));
        CHECK_ERROR(graph.compile(), ErrorCode::InvalidGraph, "'pattern.comp' writes storage images");
    }
    // Likewise for storage buffers; an image write doesn't cover a buffer write
    {
        RenderGraph graph(ctx);
        graph.importImage("image", image, ImageState::general());
        graph.importBuffer<uint32_t>("buffer", buffer);
        graph.add<DispatchNode>("sum", ctx.resources().create<AnyComputeMaterial>(ctx, std::vector<MaterialBinding>{
            MaterialBinding::storageImage(heap.storageImage(image), Access::StorageWrite),
            MaterialBinding::storageBuffer(heap.storageBuffer(buffer), Access::StorageBufferRead),
        }, testShader(ctx, "fill_buffer.comp")));
        CHECK_ERROR(graph.compile(), ErrorCode::InvalidGraph, "'fill_buffer.comp' writes storage buffers");
    }
    // Declared writes, and shaders that only read, are fine
    {
        RenderGraph graph(ctx);
        graph.importImage("image", image, ImageState::general());
        graph.importBuffer<uint32_t>("buffer", buffer);
        graph.add<DispatchNode>("pattern", ctx.resources().create<AnyComputeMaterial>(ctx, std::vector<MaterialBinding>{
            MaterialBinding::storageImage(heap.storageImage(image), Access::StorageWrite),
        }, testShader(ctx, "pattern.comp")));
        graph.add<DispatchNode>("read", ctx.resources().create<AnyComputeMaterial>(ctx, std::vector<MaterialBinding>{
            MaterialBinding::storageImage(heap.storageImage(image), Access::StorageRead),
            MaterialBinding::storageBuffer(heap.storageBuffer(buffer), Access::StorageBufferRead),
        }, testShader(ctx, "reflect_reads.comp")));
        CHECK_OK(graph.compile());
        graph.execute().value();
        ctx.waitIdleAll();
    }
}

TEST_CASE_FIXTURE(GpuTest, "A reloaded shader that writes what its material doesn't declare keeps the previous pipeline") {
    namespace fs = std::filesystem;
    auto& ctx = context();
    auto& heap = ctx.descriptors();
    const fs::path directory = fs::temp_directory_path() / ("vulkan_test_write_check_" + std::to_string(::getpid()));
    fs::create_directories(directory);
    fs::copy_file(fs::path(testShaderDir()) / "reflect_reads.comp.spv", directory / "write_check.comp.spv",
                  fs::copy_options::overwrite_existing);
    ctx.shaders().addSearchPath(directory);

    auto image = Image::createStorage(ctx, VK_FORMAT_R8G8B8A8_UNORM, 8, 8).value();
    auto buffer = storageBuffer(ctx);
    {
        auto material = ctx.resources().create<AnyComputeMaterial>(ctx, std::vector<MaterialBinding>{
            MaterialBinding::storageImage(heap.storageImage(image), Access::StorageRead),
            MaterialBinding::storageBuffer(heap.storageBuffer(buffer), Access::StorageBufferRead),
        }, ctx.shaders().get("write_check.comp").value());
        CHECK(material->undeclaredWrites().empty());
        CHECK_OK(material->ensurePipeline());
        CHECK(material->pipelineVersion() == 1);

        fs::copy_file(fs::path(testShaderDir()) / "pattern.comp.spv", directory / "write_check.comp.spv",
                      fs::copy_options::overwrite_existing);
        auto result = ctx.shaders().reload();
        REQUIRE(result.errors.size() == 1);
        CHECK(result.errors[0].code == ErrorCode::InvalidGraph);
        CHECK(result.errors[0].describe().find("writes storage images") != std::string::npos);
        CHECK(material->pipelineVersion() == 1);
        CHECK_FALSE(material->undeclaredWrites().empty());
    }
    ctx.shaders().forget("write_check.comp");
    std::error_code error;
    fs::remove_all(directory, error);
}
