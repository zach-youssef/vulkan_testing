#include "VkTestContext.h"
#include "TestShaders.h"
#include "Renderable.h"

#include <cstdlib>

namespace {

constexpr uint kFrames = 1;

class TestComputeMaterial : public ComputeMaterial<kFrames> {
public:
    TestComputeMaterial(VulkanContext& context,
                        std::vector<Handle<Descriptor>> descriptors,
                        const std::vector<char>& shaderCode,
                        glm::vec3 dispatchDimensions)
    : ComputeMaterial<kFrames>(context, descriptors, shaderCode),
    dispatchDimensions_(dispatchDimensions) {}

    void update(uint32_t, VkExtent2D) override {}

    glm::vec3 getDispatchDimensions() override {
        return dispatchDimensions_;
    }

private:
    glm::vec3 dispatchDimensions_;
};

// Records and synchronously executes a single dispatch of the material
void dispatch(VulkanContext& ctx, ComputeMaterial<kFrames>& material) {
    ctx.submitImmediate([&material](VkCommandBuffer commandBuffer) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, material.getPipeline());
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                material.getPipelineLayout(),
                                0, 1, material.getDescriptorSet(0),
                                0, nullptr);
        auto groups = material.getDispatchDimensions();
        vkCmdDispatch(commandBuffer,
                      static_cast<uint32_t>(groups.x),
                      static_cast<uint32_t>(groups.y),
                      static_cast<uint32_t>(groups.z));
    });
}

// Storage images must be in the GENERAL layout when used by a shader
void makeStorageImage(VulkanContext& ctx, Image& image) {
    image.transitionLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
}

Handle<Image> createStorageImage(VulkanContext& ctx, VkFormat format, uint32_t width, uint32_t height) {
    Handle<Image> image;
    Image::createEmpty(image, ctx, format, width, height);
    makeStorageImage(ctx, *image);
    return image;
}

std::vector<uint8_t> readback(VulkanContext& ctx, Image& image) {
    return image.readback(VK_IMAGE_LAYOUT_GENERAL);
}

} // namespace

TEST_CASE_FIXTURE(GpuTest, "ComputeMaterial dispatch writes every texel of a storage image") {
    auto& ctx = context();
    // Not a multiple of the 8x8 workgroup, to exercise the shader's bounds check
    constexpr uint32_t width = 45;
    constexpr uint32_t height = 30;

    auto output = createStorageImage(ctx, VK_FORMAT_R8G8B8A8_UNORM, width, height);
    {
        std::vector<Handle<Descriptor>> descriptors{
            ctx.resources().create<StorageImageDescriptor<kFrames>>(VK_SHADER_STAGE_COMPUTE_BIT,
                                                                    std::array<Handle<Image>, kFrames>{output}),
        };
        auto material = ctx.resources().create<TestComputeMaterial>(
            ctx, descriptors, loadTestShader("pattern.comp"),
            glm::vec3{(width + 7) / 8, (height + 7) / 8, 1});

        dispatch(ctx, *material);
    }

    auto pixels = readback(ctx, *output);
    REQUIRE(pixels.size() == width * height * 4);

    int mismatches = 0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* pixel = &pixels[(y * width + x) * 4];
            const int expected[4] = {int(x % 256), int(y % 256), int((x + y) % 256), 255};
            for (int channel = 0; channel < 4; ++channel) {
                // Allow off-by-one in case of float -> UNORM rounding differences between devices
                if (std::abs(pixel[channel] - expected[channel]) > 1 && mismatches++ < 10) {
                    FAIL_CHECK("Texel (" << x << ", " << y << ") channel " << channel
                               << " = " << int(pixel[channel]) << ", expected " << expected[channel]);
                }
            }
        }
    }
    CHECK(mismatches == 0);
}

TEST_CASE_FIXTURE(GpuTest, "Example copy shader (test.comp) copies its input image to its output") {
    auto& ctx = context();
    // test.comp uses 32x32 workgroups without a bounds check, so use multiples of 32
    constexpr uint32_t width = 64;
    constexpr uint32_t height = 32;

    std::vector<uint8_t> source(width * height * 4);
    for (size_t i = 0; i < source.size(); ++i) {
        source[i] = static_cast<uint8_t>((i * 7) % 251);
    }

    Handle<Image> input;
    Image::createFromUcharBuffer(input, ctx, source.data(), width, height, source.size());
    makeStorageImage(ctx, *input);
    // Copy into an image of the same format as the input, so the bytes should match exactly
    auto output = createStorageImage(ctx, input->getFormat(), width, height);
    {
        std::vector<Handle<Descriptor>> descriptors{
            ctx.resources().create<StorageImageDescriptor<kFrames>>(VK_SHADER_STAGE_COMPUTE_BIT,
                                                                    std::array<Handle<Image>, kFrames>{input}),
            ctx.resources().create<StorageImageDescriptor<kFrames>>(VK_SHADER_STAGE_COMPUTE_BIT,
                                                                    std::array<Handle<Image>, kFrames>{output}),
        };
        auto material = ctx.resources().create<TestComputeMaterial>(
            ctx, descriptors, loadTestShader("test.comp"),
            glm::vec3{width / 32, height / 32, 1});

        dispatch(ctx, *material);
    }

    CHECK(readback(ctx, *output) == source);
}
