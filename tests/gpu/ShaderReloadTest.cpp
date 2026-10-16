#include "VkTestContext.h"
#include "TestShaders.h"
#include "materials/Material.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kSize = 16;

class FillMaterial : public ComputeMaterial {
public:
    FillMaterial(VulkanContext& context, FrameBinding<DescriptorEntry> output, Handle<Shader> shader)
    : ComputeMaterial(context, {MaterialBinding::storageImage(std::move(output), Access::StorageWrite)}, std::move(shader)) {}

    void update(uint32_t, VkExtent2D) override {}
    glm::vec3 getDispatchDimensions() override { return {kSize / 8, kSize / 8, 1}; }
};

// A directory of its own for a test's shaders, removed afterwards
struct ShaderDirectory {
    VulkanContext& ctx;
    fs::path path;
    std::set<std::string> names;

    explicit ShaderDirectory(VulkanContext& context) : ctx(context) {
        static std::atomic<int> counter{0};
        path = fs::temp_directory_path() / ("vulkan_test_shaders_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        fs::create_directories(path);
    }

    ~ShaderDirectory() {
        // The shared library would otherwise keep trying to reload them
        for (const auto& name : names) {
            ctx.shaders().forget(name);
        }
        std::error_code error;
        fs::remove_all(path, error);
    }

    // Copies a compiled test shader into the directory as `name`.spv
    void install(const std::string& testShader, const std::string& name) {
        names.insert(name);
        fs::copy_file(fs::path(testShaderDir()) / (testShader + ".spv"), path / (name + ".spv"),
                      fs::copy_options::overwrite_existing);
    }

    void write(const std::string& name, const std::string& contents) {
        std::ofstream(path / (name + ".spv"), std::ios::binary) << contents;
    }
};

// Runs the material once and returns the first texel
std::vector<uint8_t> firstTexel(VulkanContext& ctx, Material& material, Image& image) {
    material.ensurePipeline().value();
    ctx.submitImmediate([&](VkCommandBuffer commandBuffer) {
        ctx.descriptors().bind(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE);
        material.bind(commandBuffer, 0);
        vkCmdDispatch(commandBuffer, kSize / 8, kSize / 8, 1);
    }).value();
    auto pixels = image.readback(VK_IMAGE_LAYOUT_GENERAL).value();
    return std::vector<uint8_t>(pixels.begin() + 4 * (kSize + 1), pixels.begin() + 4 * (kSize + 2));   // Texel (1, 1)
}

} // namespace

TEST_CASE_FIXTURE(GpuTest, "Shaders reload from disk and materials rebuild their pipelines") {
    auto& ctx = context();
    ShaderDirectory directory(ctx);
    directory.install("pattern.comp", "reload_a.comp");
    ctx.shaders().addSearchPath(directory.path);

    auto image = Image::createStorage(ctx, VK_FORMAT_R8G8B8A8_UNORM, kSize, kSize).value();
    auto shader = ctx.shaders().get("reload_a.comp").value();
    CHECK(shader->stage() == VK_SHADER_STAGE_COMPUTE_BIT);
    CHECK(shader->version() == 0);
    CHECK(ctx.shaders().get("reload_a.comp").value() == shader);
    auto material = ctx.resources().create<FillMaterial>(ctx, ctx.descriptors().storageImage(image), shader);
    CHECK(firstTexel(ctx, *material, *image) == std::vector<uint8_t>{1, 1, 2, 255});

    // Unchanged files reload nothing
    CHECK(ctx.shaders().reload().reloaded.empty());

    const VkPipeline before = material->getPipeline();
    directory.install("solid.comp", "reload_a.comp");
    auto result = ctx.shaders().reload();
    CHECK(result.errors.empty());
    CHECK(result.reloaded == std::vector<std::string>{"reload_a.comp"});
    CHECK(shader->version() == 1);
    CHECK(material->pipelineVersion() == 2);
    CHECK(material->getPipeline() != before);
    CHECK(firstTexel(ctx, *material, *image) == std::vector<uint8_t>{51, 102, 153, 255});
}

TEST_CASE_FIXTURE(GpuTest, "A shader that fails to reload keeps its previous code") {
    auto& ctx = context();
    ShaderDirectory directory(ctx);
    directory.install("pattern.comp", "reload_b.comp");
    ctx.shaders().addSearchPath(directory.path);

    auto image = Image::createStorage(ctx, VK_FORMAT_R8G8B8A8_UNORM, kSize, kSize).value();
    auto shader = ctx.shaders().get("reload_b.comp").value();
    auto material = ctx.resources().create<FillMaterial>(ctx, ctx.descriptors().storageImage(image), shader);

    directory.write("reload_b.comp", "this is not SPIR-V");
    auto result = ctx.shaders().reload();
    REQUIRE(result.errors.size() == 1);
    CHECK(result.errors[0].code == ErrorCode::InvalidData);
    CHECK(result.errors[0].describe().find("reload_b.comp") != std::string::npos);
    CHECK(result.reloaded.empty());
    CHECK(shader->version() == 0);
    CHECK(material->pipelineVersion() == 0);
    CHECK(firstTexel(ctx, *material, *image) == std::vector<uint8_t>{1, 1, 2, 255});

    // A missing file is an error too
    fs::remove(directory.path / "reload_b.comp.spv");
    auto missing = ctx.shaders().reload();
    REQUIRE(missing.errors.size() == 1);
    CHECK(missing.errors[0].code == ErrorCode::NotFound);
}

TEST_CASE_FIXTURE(GpuTest, "reloadModified only reloads shaders whose files changed") {
    auto& ctx = context();
    ShaderDirectory directory(ctx);
    directory.install("pattern.comp", "reload_c.comp");
    ctx.shaders().addSearchPath(directory.path);
    auto shader = ctx.shaders().get("reload_c.comp").value();

    CHECK(ctx.shaders().reloadModified().reloaded.empty());
    directory.install("solid.comp", "reload_c.comp");
    // Make sure the modification time differs even on coarse file systems
    fs::last_write_time(directory.path / "reload_c.comp.spv", fs::file_time_type::clock::now() + std::chrono::seconds(5));
    CHECK(ctx.shaders().reloadModified().reloaded == std::vector<std::string>{"reload_c.comp"});
    CHECK(shader->version() == 1);

    // A broken file is reported once, not on every check
    directory.write("reload_c.comp", "broken");
    fs::last_write_time(directory.path / "reload_c.comp.spv", fs::file_time_type::clock::now() + std::chrono::seconds(10));
    CHECK(ctx.shaders().reloadModified().errors.size() == 1);
    CHECK(ctx.shaders().reloadModified().errors.empty());
    CHECK(shader->version() == 1);
}

TEST_CASE_FIXTURE(GpuTest, "The shader library reports missing shaders and wrong stages") {
    auto& ctx = context();
    CHECK_ERROR(ctx.shaders().get("missing.comp"), ErrorCode::NotFound, "not found; searched");
    CHECK_THROWS_WITH_AS(ctx.shaders().get("shader.glsl"), doctest::Contains("Can't tell the stage"), ContractViolation);

    auto image = Image::createStorage(ctx, VK_FORMAT_R8G8B8A8_UNORM, kSize, kSize).value();
    CHECK_THROWS_WITH_AS(FillMaterial(ctx, ctx.descriptors().storageImage(image), testShader(ctx, "shader.vert")),
                         doctest::Contains("VK_SHADER_STAGE_COMPUTE_BIT shader is needed"), ContractViolation);
}
