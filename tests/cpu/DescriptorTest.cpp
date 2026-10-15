#include "descriptors/Descriptor.h"

#include <doctest/doctest.h>

// Image descriptors dereference their image handles on construction, so they
// need real Images and are covered by the GPU tests instead.

namespace {

constexpr uint kFrames = 3;

struct alignas(16) TestUbo {
    float values[12];
};

} // namespace

TEST_CASE("UniformBufferDescriptor fills buffer infos for every frame") {
    UniformBufferDescriptor<TestUbo, kFrames> descriptor({}, VK_SHADER_STAGE_VERTEX_BIT);

    CHECK(descriptor.getType() == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    CHECK(descriptor.getStageFlags() == VK_SHADER_STAGE_VERTEX_BIT);

    for (uint frame = 0; frame < kFrames; ++frame) {
        CAPTURE(frame);
        auto* info = descriptor.getBufferInfo(frame);
        REQUIRE(info != nullptr);
        // Unbound buffers are left as VK_NULL_HANDLE until bindBuffer is called
        CHECK(info->buffer == VK_NULL_HANDLE);
        CHECK(info->offset == 0);
        CHECK(info->range == sizeof(TestUbo));
        CHECK(descriptor.getImageInfo(frame) == nullptr);
    }
}

TEST_CASE("UniformBufferDescriptor can offset each frame into a shared buffer") {
    UniformBufferDescriptor<TestUbo, kFrames> descriptor({}, VK_SHADER_STAGE_COMPUTE_BIT, true /* offsetByFrame */);

    for (uint frame = 0; frame < kFrames; ++frame) {
        CAPTURE(frame);
        CHECK(descriptor.getBufferInfo(frame)->offset == frame * sizeof(TestUbo));
        CHECK(descriptor.getBufferInfo(frame)->range == sizeof(TestUbo));
    }
}

TEST_CASE("BufferDescriptor sizes its range by element count and applies its offset") {
    BufferDescriptor<float, kFrames> descriptor(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                VK_SHADER_STAGE_FRAGMENT_BIT,
                                                {}, 16 /* size */, 8 /* offset */);

    CHECK(descriptor.getType() == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    CHECK(descriptor.getStageFlags() == VK_SHADER_STAGE_FRAGMENT_BIT);

    for (uint frame = 0; frame < kFrames; ++frame) {
        CAPTURE(frame);
        auto* info = descriptor.getBufferInfo(frame);
        CHECK(info->buffer == VK_NULL_HANDLE);
        CHECK(info->offset == 8);
        CHECK(info->range == 16 * sizeof(float));
        CHECK(descriptor.getImageInfo(frame) == nullptr);
    }
}

TEST_CASE("BufferDescriptor defaults to a zero offset") {
    BufferDescriptor<uint32_t, kFrames> descriptor(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_SHADER_STAGE_COMPUTE_BIT,
                                                   {}, 4 /* size */);

    CHECK(descriptor.getBufferInfo(0)->offset == 0);
    CHECK(descriptor.getBufferInfo(0)->range == 4 * sizeof(uint32_t));
}
