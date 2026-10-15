#include "VkTestContext.h"
#include "descriptors/Descriptor.h"

#include <glm/glm.hpp>

namespace {

constexpr uint kFrames = 2;

Handle<Image> makeImage(VulkanContext& ctx) {
    Handle<Image> image;
    Image::createEmptyRGBA(image, ctx, 8, 8);
    return image;
}

} // namespace

TEST_CASE_FIXTURE(GpuTest, "CombinedImageSamplerDescriptor keeps its images and sampler alive") {
    auto& ctx = context();
    auto& resources = ctx.resources();
    const size_t baseline = resources.size();

    auto imageA = makeImage(ctx);
    auto imageB = makeImage(ctx);
    Handle<VulkanSampler> sampler;
    REQUIRE(VulkanSampler::createWithAddressMode(sampler, ctx, VK_SAMPLER_ADDRESS_MODE_REPEAT) == VK_SUCCESS);

    const VkImageView viewA = imageA->getImageView();
    const VkImageView viewB = imageB->getImageView();
    const VkSampler rawSampler = **sampler;

    auto descriptor = resources.create<CombinedImageSamplerDescriptor<kFrames>>(
        VK_SHADER_STAGE_FRAGMENT_BIT, std::array<Handle<Image>, kFrames>{imageA, imageB}, sampler);

    CHECK(descriptor->getType() == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    CHECK(descriptor->getBufferInfo(0) == nullptr);
    CHECK(descriptor->getImageInfo(0)->imageView == viewA);
    CHECK(descriptor->getImageInfo(1)->imageView == viewB);
    for (uint frame = 0; frame < kFrames; ++frame) {
        CAPTURE(frame);
        CHECK(descriptor->getImageInfo(frame)->sampler == rawSampler);
        CHECK(descriptor->getImageInfo(frame)->imageLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // Dropping our handles must not free resources the descriptor still references
    imageA.reset();
    imageB.reset();
    sampler.reset();
    CHECK(resources.size() == baseline + 4); // 2 images, 1 sampler, 1 descriptor

    descriptor.reset();
    CHECK(resources.size() == baseline);
}

TEST_CASE_FIXTURE(GpuTest, "StorageImageDescriptor keeps its images alive") {
    auto& ctx = context();
    auto& resources = ctx.resources();
    const size_t baseline = resources.size();

    Handle<Image> image;
    Image::createStorage(image, ctx, VK_FORMAT_R8G8B8A8_UNORM, 8, 8);
    auto descriptor = resources.create<StorageImageDescriptor<kFrames>>(
        VK_SHADER_STAGE_COMPUTE_BIT, std::array<Handle<Image>, kFrames>{image, image});
    CHECK(descriptor->getType() == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    CHECK(descriptor->getImageInfo(0)->imageLayout == VK_IMAGE_LAYOUT_GENERAL);
    CHECK(image.useCount() == 3);

    image.reset();
    CHECK(resources.size() == baseline + 2);
    descriptor.reset();
    CHECK(resources.size() == baseline);
}

TEST_CASE_FIXTURE(GpuTest, "UniformBufferDescriptor::bindBuffer binds and retains the buffer") {
    auto& ctx = context();
    auto& resources = ctx.resources();
    const size_t baseline = resources.size();

    UniformBufferDescriptor<glm::mat4, kFrames> descriptor({}, VK_SHADER_STAGE_VERTEX_BIT);

    Handle<Buffer<glm::mat4>> buffer;
    REQUIRE(Buffer<glm::mat4>::create(buffer, ctx, 1, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == VK_SUCCESS);
    const VkBuffer rawBuffer = buffer->getBuffer();

    descriptor.bindBuffer(1, buffer);
    buffer.reset();

    CHECK(descriptor.getBufferInfo(0)->buffer == VK_NULL_HANDLE);
    CHECK(descriptor.getBufferInfo(1)->buffer == rawBuffer);
    CHECK(resources.size() == baseline + 1);
}
