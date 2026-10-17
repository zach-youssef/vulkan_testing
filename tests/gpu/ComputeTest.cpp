#include "VkTestContext.h"
#include "TestShaders.h"
#include "materials/Material.h"
#include "descriptors/DescriptorHeap.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

class TestComputeMaterial : public ComputeMaterial {
public:
    TestComputeMaterial(VulkanContext& context,
                        std::vector<MaterialBinding> bindings,
                        Handle<Shader> shader,
                        glm::vec3 dispatchDimensions,
                        std::vector<uint32_t> extraPushConstants = {})
    : ComputeMaterial(context, std::move(bindings), std::move(shader)),
    dispatchDimensions_(dispatchDimensions), extraPushConstants_(std::move(extraPushConstants)) {}

    void update(uint32_t, VkExtent2D) override {}

    glm::vec3 getDispatchDimensions() override {
        return dispatchDimensions_;
    }

protected:
    void writePushConstants(uint32_t, std::vector<uint8_t>& data) override {
        const size_t offset = data.size();
        data.resize(offset + extraPushConstants_.size() * sizeof(uint32_t));
        if (!extraPushConstants_.empty()) {
            memcpy(data.data() + offset, extraPushConstants_.data(), extraPushConstants_.size() * sizeof(uint32_t));
        }
    }

private:
    glm::vec3 dispatchDimensions_;
    std::vector<uint32_t> extraPushConstants_;
};

// Records and synchronously executes a single dispatch of the material
void dispatch(VulkanContext& ctx, ComputeMaterial& material) {
    material.ensurePipeline().value();
    ctx.submitImmediate([&ctx, &material](VkCommandBuffer commandBuffer) {
        ctx.descriptors().bind(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE);
        material.bind(commandBuffer, 0);
        auto groups = material.getDispatchDimensions();
        vkCmdDispatch(commandBuffer,
                      static_cast<uint32_t>(groups.x),
                      static_cast<uint32_t>(groups.y),
                      static_cast<uint32_t>(groups.z));
    }).value();
}

float srgbToLinear(float value) {
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

// Decodes an IEEE 754 half precision float
float halfToFloat(uint16_t half) {
    const int sign = half >> 15;
    const int exponent = (half >> 10) & 0x1f;
    const int mantissa = half & 0x3ff;
    float value;
    if (exponent == 0) {
        value = std::ldexp(static_cast<float>(mantissa), -24); // Subnormal
    } else if (exponent == 31) {
        value = mantissa ? NAN : INFINITY;
    } else {
        value = std::ldexp(static_cast<float>(mantissa | 0x400), exponent - 25);
    }
    return sign ? -value : value;
}

// Left in the GENERAL layout, which storage images must be in when used by a shader
Handle<Image> createStorageImage(VulkanContext& ctx, VkFormat format, uint32_t width, uint32_t height) {
    auto image = Image::createStorage(ctx, format, width, height).value();
    return image;
}

std::vector<uint8_t> readback(VulkanContext& ctx, Image& image) {
    return image.readback(VK_IMAGE_LAYOUT_GENERAL).value();
}

} // namespace

TEST_CASE_FIXTURE(GpuTest, "ComputeMaterial dispatch writes every texel of a storage image") {
    auto& ctx = context();
    // Not a multiple of the 8x8 workgroup, to exercise the shader's bounds check
    constexpr uint32_t width = 45;
    constexpr uint32_t height = 30;

    auto output = createStorageImage(ctx, VK_FORMAT_R8G8B8A8_UNORM, width, height);
    {
        auto material = ctx.resources().create<TestComputeMaterial>(
            ctx, std::vector<MaterialBinding>{
                MaterialBinding::storageImage(ctx.descriptors().storageImage(output), Access::StorageWrite),
            },
            testShader(ctx, "pattern.comp"),
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

TEST_CASE_FIXTURE(GpuTest, "Example copy shader (test.comp) copies a sampled sRGB image into a float image") {
    auto& ctx = context();
    // test.comp uses 32x32 workgroups without a bounds check, so use multiples of 32
    constexpr uint32_t width = 64;
    constexpr uint32_t height = 32;

    std::vector<uint8_t> source(width * height * 4);
    for (size_t i = 0; i < source.size(); ++i) {
        source[i] = static_cast<uint8_t>((i * 7) % 251);
    }

    // The input is sampled (and so stays in SHADER_READ_ONLY_OPTIMAL); the output is a storage image
    auto input = Image::createFromUcharBuffer(ctx, source.data(), width, height, source.size(), ColorSpace::Srgb).value();
    auto output = createStorageImage(ctx, VK_FORMAT_R16G16B16A16_SFLOAT, width, height);
    auto sampler = VulkanSampler::createWithModeAndFilter(ctx, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                                   VK_FILTER_NEAREST).value();
    {
        auto& heap = ctx.descriptors();
        auto material = ctx.resources().create<TestComputeMaterial>(
            ctx, std::vector<MaterialBinding>{
                MaterialBinding::sampled(heap.sampledImage(input)),
                MaterialBinding::sampler(heap.sampler(sampler)),
                MaterialBinding::storageImage(heap.storageImage(output), Access::StorageWrite),
            },
            testShader(ctx, "test.comp"),
            glm::vec3{width / 32, height / 32, 1});

        dispatch(ctx, *material);
    }

    auto bytes = readback(ctx, *output);
    REQUIRE(bytes.size() == width * height * 4 * sizeof(uint16_t));
    std::vector<uint16_t> halves(width * height * 4);
    memcpy(halves.data(), bytes.data(), bytes.size());

    int mismatches = 0;
    for (size_t i = 0; i < source.size(); ++i) {
        const bool isAlpha = i % 4 == 3;
        const float encoded = source[i] / 255.0f;
        // Sampling an sRGB image decodes its color channels to linear; alpha is always linear
        const float expected = isAlpha ? encoded : srgbToLinear(encoded);
        const float actual = halfToFloat(halves[i]);
        if (std::abs(actual - expected) > 2e-3f && mismatches++ < 10) {
            FAIL_CHECK("Component " << i << " = " << actual << ", expected " << expected);
        }
    }
    CHECK(mismatches == 0);
}

TEST_CASE_FIXTURE(GpuTest, "BINDLESS_BUFFER_ARRAY declares buffers of runtime-sized arrays") {
    auto& ctx = context();
    constexpr uint32_t kCount = 100;
    auto hostBuffer = [&ctx](uint32_t count) {
        return Buffer<uint32_t>::create(ctx, count, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT).value();
    };
    auto source = hostBuffer(kCount);
    auto target = hostBuffer(kCount);
    source->mapAndExecute(0, sizeof(uint32_t) * kCount, [](void* data) {
        auto* values = static_cast<uint32_t*>(data);
        for (uint32_t i = 0; i < kCount; ++i) values[i] = i;
    }).value();
    auto& heap = ctx.descriptors();
    {
        TestComputeMaterial material(ctx, {
            MaterialBinding::storageBuffer(heap.storageBuffer(source), Access::StorageBufferRead),
            MaterialBinding::storageBuffer(heap.storageBuffer(target), Access::StorageBufferWrite),
        }, testShader(ctx, "buffer_array.comp"), glm::vec3{(kCount + 63) / 64, 1, 1}, {kCount});
        dispatch(ctx, material);
    }
    target->mapAndExecute(0, sizeof(uint32_t) * kCount, [](void* data) {
        auto* values = static_cast<uint32_t*>(data);
        for (uint32_t i = 0; i < kCount; ++i) {
            CAPTURE(i);
            CHECK(values[i] == 2 * i);
        }
    }).value();
}
