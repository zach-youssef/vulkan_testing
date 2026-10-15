#include "VkTestContext.h"
#include "TestShaders.h"
#include "descriptors/DescriptorHeap.h"

#include <set>

namespace {

Handle<Buffer<uint32_t>> valuesBuffer(VulkanContext& ctx, uint32_t value) {
    Handle<Buffer<uint32_t>> buffer;
    VK_SUCCESS_OR_THROW(Buffer<uint32_t>::create(buffer, ctx, 1, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
                        "Failed to create buffer");
    buffer->mapAndExecute(0, sizeof(uint32_t), [value](void* data) { memcpy(data, &value, sizeof(value)); });
    return buffer;
}

} // namespace

TEST_CASE_FIXTURE(GpuTest, "The universal descriptor set has a slot array per descriptor type") {
    auto& heap = context().descriptors();
    CHECK(heap.set() != VK_NULL_HANDLE);
    CHECK(heap.setLayout() != VK_NULL_HANDLE);
    CHECK(heap.pipelineLayout() != VK_NULL_HANDLE);
    for (auto type : {DescriptorType::SampledImage, DescriptorType::Sampler, DescriptorType::StorageImage,
                      DescriptorType::StorageBuffer}) {
        CAPTURE(descriptorTypeName(type));
        CHECK(heap.capacity(type) > 0);
    }
    // Created once
    CHECK(&context().descriptors() == &heap);
}

TEST_CASE_FIXTURE(GpuTest, "Descriptor entries keep their resources alive and free their slots after in-flight work") {
    auto& ctx = context();
    auto& heap = ctx.descriptors();
    auto& resources = ctx.resources();
    const size_t baseline = resources.size();
    // Slots freed by earlier tests may still be waiting to be collected
    ctx.waitIdleAll();
    ctx.collectRetired();
    const uint32_t usedBefore = heap.used(DescriptorType::StorageImage);

    Handle<Image> image;
    Image::createStorage(image, ctx, VK_FORMAT_R8G8B8A8_UNORM, 4, 4);
    auto storage = heap.storageImage(image);
    auto sampled = heap.sampledImage(image);
    CHECK(storage->type() == DescriptorType::StorageImage);
    CHECK(storage->imageLayout() == VK_IMAGE_LAYOUT_GENERAL);
    CHECK(sampled->imageLayout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(heap.used(DescriptorType::StorageImage) == usedBefore + 1);

    // Distinct entries of a type get distinct slots
    auto another = heap.storageImage(image);
    CHECK(another->index() != storage->index());

    image.reset();
    CHECK(resources.size() == baseline + 4);   // The image and three entries
    const std::set<uint32_t> freedIndices{storage->index(), another->index()};
    storage.reset();
    another.reset();
    sampled.reset();
    CHECK(resources.size() == baseline);

    // The slots are freed once the work submitted before has finished
    ctx.waitIdleAll();
    ctx.collectRetired();
    CHECK(heap.used(DescriptorType::StorageImage) == usedBefore);
    Handle<Image> reused;
    Image::createStorage(reused, ctx, VK_FORMAT_R8G8B8A8_UNORM, 4, 4);
    auto entry = heap.storageImage(reused);
    CHECK(freedIndices.count(entry->index()) == 1);
}

TEST_CASE_FIXTURE(GpuTest, "Image descriptor entries require the matching image usage") {
    auto& ctx = context();
    Handle<Image> sampledOnly;
    Image::createEmptyRGBA(sampledOnly, ctx, 4, 4);
    CHECK_THROWS_AS(ctx.descriptors().storageImage(sampledOnly), std::invalid_argument);
    CHECK_NOTHROW(ctx.descriptors().sampledImage(sampledOnly));
}

TEST_CASE_FIXTURE(GpuTest, "Shaders find resources in the universal set by pushed index") {
    auto& ctx = context();
    auto& heap = ctx.descriptors();
    constexpr uint32_t width = 20, height = 12;

    Handle<Image> image;
    Image::createStorage(image, ctx, VK_FORMAT_R8G8B8A8_UNORM, width, height);
    // A few unrelated entries first, so the indices aren't 0
    std::vector<Handle<DescriptorEntry>> padding;
    for (int i = 0; i < 3; ++i) {
        padding.push_back(heap.storageImage(image));
        padding.push_back(heap.storageBuffer(valuesBuffer(ctx, 0)));
    }
    auto output = heap.storageImage(image);
    auto values = heap.storageBuffer(valuesBuffer(ctx, 77));

    // A compute pipeline with the universal layout
    auto shader = testShader(ctx, "bindless_pattern.comp");
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shader->module();
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = heap.pipelineLayout();
    std::unique_ptr<VulkanComputePipeline> pipeline;
    REQUIRE(VulkanComputePipeline::create(pipeline, ctx.device(), pipelineInfo) == VK_SUCCESS);

    ctx.submitImmediate([&](VkCommandBuffer commandBuffer) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, **pipeline);
        heap.bind(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE);
        uint32_t indices[2] = {output->index(), values->index()};
        vkCmdPushConstants(commandBuffer, heap.pipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(indices), indices);
        vkCmdDispatch(commandBuffer, (width + 7) / 8, (height + 7) / 8, 1);
    });

    auto pixels = image->readback(VK_IMAGE_LAYOUT_GENERAL);
    int mismatches = 0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* pixel = &pixels[(y * width + x) * 4];
            if ((pixel[0] != x || pixel[1] != y || pixel[2] != 77 || pixel[3] != 255) && mismatches++ < 5) {
                FAIL_CHECK("Texel (" << x << ", " << y << ") = " << int(pixel[0]) << ", " << int(pixel[1]) << ", "
                           << int(pixel[2]) << ", " << int(pixel[3]));
            }
        }
    }
    CHECK(mismatches == 0);
}

TEST_CASE("Running out of descriptor slots throws, until entries are freed") {
    VulkanContextConfig config;
    config.descriptorCapacities.samplers = 2;
    VulkanContext ctx(std::move(config));
    auto& heap = ctx.descriptors();
    REQUIRE(heap.capacity(DescriptorType::Sampler) == 2);

    Handle<VulkanSampler> sampler;
    REQUIRE(VulkanSampler::createWithAddressMode(sampler, ctx, VK_SAMPLER_ADDRESS_MODE_REPEAT) == VK_SUCCESS);
    auto first = heap.sampler(sampler);
    auto second = heap.sampler(sampler);
    CHECK_THROWS_WITH_AS(heap.sampler(sampler), doctest::Contains("no free sampler slots"), std::runtime_error);

    // Nothing is in flight, so the freed slot is available straight away
    first.reset();
    CHECK_NOTHROW(heap.sampler(sampler));
}
