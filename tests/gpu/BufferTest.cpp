#include "VkTestContext.h"
#include "resources/Buffer.h"

#include <numeric>

namespace {

constexpr VkMemoryPropertyFlags kHostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

template<typename T>
std::vector<T> readBuffer(Buffer<T>& buffer, size_t count) {
    std::vector<T> values(count);
    buffer.mapAndExecute(0, count * sizeof(T), [&values, count](void* data) {
        memcpy(values.data(), data, count * sizeof(T));
    }).value();
    return values;
}

} // namespace

TEST_CASE_FIXTURE(GpuTest, "Host visible Buffer round trips data through mapAndExecute") {
    auto& ctx = context();
    std::vector<uint32_t> data(64);
    std::iota(data.begin(), data.end(), 100u);

    auto buffer = Buffer<uint32_t>::create(ctx, data.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, kHostVisible).value();
    CHECK(buffer->getStride() == sizeof(uint32_t));

    CHECK_OK(buffer->mapAndExecute(0, data.size() * sizeof(uint32_t), [&data](void* mapped) {
        memcpy(mapped, data.data(), data.size() * sizeof(uint32_t));
    }));

    CHECK(readBuffer(*buffer, data.size()) == data);
}

TEST_CASE_FIXTURE(GpuTest, "Buffer::createAndInitialize uploads data to device local memory") {
    auto& ctx = context();
    std::vector<float> data(256);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<float>(i) * 0.5f;
    }

    auto deviceBuffer = Buffer<float>::createAndInitialize(ctx, data, VK_BUFFER_USAGE_TRANSFER_SRC_BIT).value();
    REQUIRE(deviceBuffer);

    // Device local memory may not be mappable, so copy it back through a host visible buffer
    auto readbackBuffer = Buffer<float>::create(ctx, data.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, kHostVisible).value();
    CHECK_OK(Buffer<float>::copyBuffer(ctx, deviceBuffer->getBuffer(), readbackBuffer->getBuffer(),
                                       data.size() * sizeof(float)));

    CHECK(readBuffer(*readbackBuffer, data.size()) == data);
}

TEST_CASE_FIXTURE(GpuTest, "Buffer persistent mappings stay valid until released") {
    auto& ctx = context();
    struct Pair { int a; int b; };

    auto buffer = Buffer<Pair>::create(ctx, 1, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, kHostVisible).value();
    {
        auto mapping = buffer->getPersistentMapping(0, sizeof(Pair)).value();
        mapping->a = 7;
        mapping->b = -3;
    }

    auto values = readBuffer(*buffer, 1);
    CHECK(values[0].a == 7);
    CHECK(values[0].b == -3);
}
