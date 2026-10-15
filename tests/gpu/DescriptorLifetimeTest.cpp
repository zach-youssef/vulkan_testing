#include "VkTestContext.h"
#include "descriptors/Descriptor.h"

#include <glm/glm.hpp>

namespace {

Handle<Image> makeImage(VulkanContext& ctx) {
    Handle<Image> image;
    Image::createEmptyRGBA(image, ctx, 8, 8);
    return image;
}

Handle<Buffer<glm::mat4>> makeUniformBuffer(VulkanContext& ctx) {
    Handle<Buffer<glm::mat4>> buffer;
    VK_SUCCESS_OR_THROW(Buffer<glm::mat4>::create(buffer, ctx, 1, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
                        "Failed to create uniform buffer");
    return buffer;
}

} // namespace

TEST_CASE_FIXTURE(GpuTest, "CombinedImageSamplerDescriptor keeps its images and sampler alive") {
    auto& ctx = context();
    auto& resources = ctx.resources();
    const size_t baseline = resources.size();
    REQUIRE(ctx.framesInFlight() == 2);

    auto images = resources.create<PerFrameHandles<Image>>(ctx, [&ctx](uint32_t) { return makeImage(ctx); });
    Handle<VulkanSampler> sampler;
    REQUIRE(VulkanSampler::createWithAddressMode(sampler, ctx, VK_SAMPLER_ADDRESS_MODE_REPEAT) == VK_SUCCESS);

    const VkImageView viewA = (*images)[0]->getImageView();
    const VkImageView viewB = (*images)[1]->getImageView();
    const VkSampler rawSampler = **sampler;

    auto descriptor = resources.create<CombinedImageSamplerDescriptor>(VK_SHADER_STAGE_FRAGMENT_BIT, images, sampler);

    CHECK(descriptor->getType() == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    CHECK_FALSE(descriptor->getBufferInfo(0).has_value());
    CHECK(descriptor->getImageInfo(0)->imageView == viewA);
    CHECK(descriptor->getImageInfo(1)->imageView == viewB);
    for (uint32_t frame = 0; frame < 2; ++frame) {
        CAPTURE(frame);
        CHECK(descriptor->getImageInfo(frame)->sampler == rawSampler);
        CHECK(descriptor->getImageInfo(frame)->imageLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // Dropping our handles must not free resources the descriptor still references
    images.reset();
    sampler.reset();
    CHECK(resources.size() == baseline + 5); // 2 images, their collection, 1 sampler, 1 descriptor

    descriptor.reset();
    CHECK(resources.size() == baseline);
}

TEST_CASE_FIXTURE(GpuTest, "StorageImageDescriptor shares a static image across frames and keeps it alive") {
    auto& ctx = context();
    auto& resources = ctx.resources();
    const size_t baseline = resources.size();

    Handle<Image> image;
    Image::createStorage(image, ctx, VK_FORMAT_R8G8B8A8_UNORM, 8, 8);
    auto descriptor = resources.create<StorageImageDescriptor>(VK_SHADER_STAGE_COMPUTE_BIT, image);
    CHECK(descriptor->getType() == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    CHECK(descriptor->getImageInfo(0)->imageLayout == VK_IMAGE_LAYOUT_GENERAL);
    CHECK(descriptor->getImageInfo(0)->imageView == image->getImageView());
    CHECK(descriptor->getImageInfo(1)->imageView == image->getImageView());
    CHECK(descriptor->getImageInfo(0)->sampler == VK_NULL_HANDLE);
    CHECK(image.useCount() == 2);

    image.reset();
    CHECK(resources.size() == baseline + 2);
    descriptor.reset();
    CHECK(resources.size() == baseline);
}

TEST_CASE_FIXTURE(GpuTest, "UniformBufferDescriptor::bind binds and retains buffers") {
    auto& ctx = context();
    auto& resources = ctx.resources();
    const size_t baseline = resources.size();

    UniformBufferDescriptor<glm::mat4> descriptor(nullptr, VK_SHADER_STAGE_VERTEX_BIT);
    CHECK(descriptor.getBufferInfo(0)->buffer == VK_NULL_HANDLE);

    auto buffer = makeUniformBuffer(ctx);
    const VkBuffer rawBuffer = buffer->getBuffer();
    descriptor.bind(buffer);
    buffer.reset();

    CHECK(descriptor.getBufferInfo(0)->buffer == rawBuffer);
    CHECK(descriptor.getBufferInfo(1)->buffer == rawBuffer);
    CHECK(resources.size() == baseline + 1);

    descriptor.bind(nullptr);
    CHECK(descriptor.getBufferInfo(0)->buffer == VK_NULL_HANDLE);
    CHECK(resources.size() == baseline);
}

TEST_CASE_FIXTURE(GpuTest, "Per-frame descriptors follow their collection when frames in flight change") {
    auto& ctx = context();
    auto& resources = ctx.resources();
    const size_t baseline = resources.size();
    const uint32_t originalFrames = ctx.framesInFlight();

    auto buffers = resources.create<PerFrameHandles<Buffer<glm::mat4>>>(ctx, [&ctx](uint32_t) {
        return makeUniformBuffer(ctx);
    });
    UniformBufferDescriptor<glm::mat4> descriptor(buffers, VK_SHADER_STAGE_VERTEX_BIT);
    CHECK(descriptor.getBufferInfo(1)->buffer == (*buffers)[1]->getBuffer());
    CHECK(descriptor.getBufferInfo(2)->buffer == VK_NULL_HANDLE);

    ctx.setFramesInFlight(3);
    ctx.advanceFrame();
    CHECK(descriptor.getBufferInfo(2)->buffer == (*buffers)[2]->getBuffer());
    CHECK(resources.size() == baseline + 4); // The collection and 3 buffers

    ctx.setFramesInFlight(originalFrames);
    ctx.advanceFrame();
    CHECK(descriptor.getBufferInfo(2)->buffer == VK_NULL_HANDLE);
    CHECK(resources.size() == baseline + 1 + originalFrames);

    buffers.reset();
    descriptor.bind(nullptr);
    CHECK(resources.size() == baseline);
}
