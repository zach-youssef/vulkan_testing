#include "VkTestContext.h"
#include "Buffer.h"

#include <numeric>

namespace {

constexpr VkMemoryPropertyFlags kHostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

template<typename T>
std::vector<T> readBuffer(Buffer<T>& buffer, size_t count) {
    std::vector<T> values(count);
    buffer.mapAndExecute(0, count * sizeof(T), [&values, count](void* data) {
        memcpy(values.data(), data, count * sizeof(T));
    });
    return values;
}

} // namespace

TEST_CASE_FIXTURE(GpuTest, "Host visible Buffer round trips data through mapAndExecute") {
    auto& ctx = context();
    std::vector<uint32_t> data(64);
    std::iota(data.begin(), data.end(), 100u);

    Handle<Buffer<uint32_t>> buffer;
    REQUIRE(Buffer<uint32_t>::create(buffer, ctx.vulkan(), data.size(),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, kHostVisible) == VK_SUCCESS);
    CHECK(buffer->getStride() == sizeof(uint32_t));

    buffer->mapAndExecute(0, data.size() * sizeof(uint32_t), [&data](void* mapped) {
        memcpy(mapped, data.data(), data.size() * sizeof(uint32_t));
    });

    CHECK(readBuffer(*buffer, data.size()) == data);
}

TEST_CASE_FIXTURE(GpuTest, "Buffer::createAndInitialize uploads data to device local memory") {
    auto& ctx = context();
    std::vector<float> data(256);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<float>(i) * 0.5f;
    }

    Handle<Buffer<float>> deviceBuffer;
    Buffer<float>::createAndInitialize(deviceBuffer, ctx.vulkan(), data,
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(deviceBuffer);

    // Device local memory may not be mappable, so copy it back through a host visible buffer
    Handle<Buffer<float>> readbackBuffer;
    REQUIRE(Buffer<float>::create(readbackBuffer, ctx.vulkan(), data.size(),
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT, kHostVisible) == VK_SUCCESS);
    Buffer<float>::copyBuffer(ctx.vulkan(), deviceBuffer->getBuffer(), readbackBuffer->getBuffer(),
                              data.size() * sizeof(float));

    CHECK(readBuffer(*readbackBuffer, data.size()) == data);
}

TEST_CASE_FIXTURE(GpuTest, "Buffer persistent mappings stay valid until released") {
    auto& ctx = context();
    struct Pair { int a; int b; };

    Handle<Buffer<Pair>> buffer;
    REQUIRE(Buffer<Pair>::create(buffer, ctx.vulkan(), 1,
                                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, kHostVisible) == VK_SUCCESS);
    {
        auto mapping = buffer->getPersistentMapping(0, sizeof(Pair));
        mapping->a = 7;
        mapping->b = -3;
    }

    auto values = readBuffer(*buffer, 1);
    CHECK(values[0].a == 7);
    CHECK(values[0].b == -3);
}
