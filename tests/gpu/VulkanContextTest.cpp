#include "core/VulkanContext.h"
#include "resources/Buffer.h"

#include <doctest/doctest.h>

#include <atomic>
#include <thread>

namespace {

constexpr VkMemoryPropertyFlags kHostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

// A headless, validated context that records validation errors (which may arrive on any thread)
struct ValidatedContext {
    std::mutex mutex;
    std::vector<std::string> errors;
    std::unique_ptr<VulkanContext> context;

    ValidatedContext() {
        VulkanContextConfig config;
        config.applicationName = "VulkanContext tests";
        config.enableValidation = true;
        config.onValidationMessage = [this](VkDebugUtilsMessageSeverityFlagBitsEXT severity, const char* message) {
            if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
                std::lock_guard<std::mutex> lock(mutex);
                errors.emplace_back(message);
            }
        };
        context = std::make_unique<VulkanContext>(std::move(config));
    }

    ~ValidatedContext() {
        context.reset(); // Destruction may report errors too
        for (const auto& error : errors) {
            FAIL_CHECK("Vulkan validation error: " << error);
        }
    }
};

Handle<Buffer<uint32_t>> createHostBuffer(VulkanContext& ctx, size_t count) {
    Handle<Buffer<uint32_t>> buffer;
    VK_SUCCESS_OR_THROW(Buffer<uint32_t>::create(buffer, ctx, count, VK_BUFFER_USAGE_TRANSFER_DST_BIT, kHostVisible),
                        "Failed to create buffer");
    return buffer;
}

uint32_t readFirst(Buffer<uint32_t>& buffer) {
    uint32_t value = 0;
    buffer.mapAndExecute(0, sizeof(uint32_t), [&value](void* data) {
        memcpy(&value, data, sizeof(uint32_t));
    });
    return value;
}

} // namespace

TEST_CASE("VulkanContext creates a headless device") {
    ValidatedContext validated;
    auto& ctx = *validated.context;

    CHECK(ctx.instance() != VK_NULL_HANDLE);
    CHECK(ctx.physicalDevice() != VK_NULL_HANDLE);
    CHECK(ctx.device() != VK_NULL_HANDLE);
    CHECK_FALSE(ctx.hasSurface());
    WARN_MESSAGE(ctx.validationEnabled(), "Validation layers unavailable; validation errors will not be caught");
    CHECK_FALSE(ctx.deviceName().empty());
    CHECK(ctx.resources().size() == 0);

    // Graphics & compute share a family (and therefore a queue)
    CHECK(ctx.queue(QueueType::Graphics) != VK_NULL_HANDLE);
    CHECK(ctx.queueFamily(QueueType::Compute) == ctx.queueFamily(QueueType::Graphics));
    CHECK(ctx.queue(QueueType::Compute) == ctx.queue(QueueType::Graphics));

    // Without a surface there is nothing to present to
    CHECK_THROWS_AS(ctx.queue(QueueType::Present), std::logic_error);

    CHECK(ctx.waitIdle(QueueType::Graphics) == VK_SUCCESS);
    CHECK(ctx.waitIdleAll() == VK_SUCCESS);
}

TEST_CASE("VulkanContext::createCommandPool allocates command buffers") {
    ValidatedContext validated;
    auto& ctx = *validated.context;

    auto pool = ctx.createCommandPool(QueueType::Compute);
    REQUIRE(pool);
    CHECK(pool->get() != VK_NULL_HANDLE);
    CHECK(pool->getQueueFamilyIndex() == ctx.queueFamily(QueueType::Compute));

    auto several = pool->allocate(3);
    REQUIRE(several.size() == 3);
    CHECK(several[0] != several[1]);
    CHECK(several[1] != several[2]);

    auto array = pool->allocateArray<2>();
    CHECK(array[0] != VK_NULL_HANDLE);
    CHECK(array[1] != VK_NULL_HANDLE);

    auto single = pool->allocateOne(VK_COMMAND_BUFFER_LEVEL_SECONDARY);
    CHECK(single != VK_NULL_HANDLE);
    pool->free(single);
}

TEST_CASE("VulkanContext::submitImmediate runs commands to completion") {
    ValidatedContext validated;
    auto& ctx = *validated.context;

    auto buffer = createHostBuffer(ctx, 4);
    ctx.submitImmediate([&buffer](VkCommandBuffer commandBuffer) {
        vkCmdFillBuffer(commandBuffer, buffer->getBuffer(), 0, VK_WHOLE_SIZE, 0xC0FFEE);
    });

    CHECK(readFirst(*buffer) == 0xC0FFEE);
}

TEST_CASE("VulkanContext supports recording on per-thread pools and submitting concurrently") {
    ValidatedContext validated;
    auto& ctx = *validated.context;

    constexpr uint32_t recordingThreads = 4;
    constexpr uint32_t immediateThreads = 2;
    constexpr uint32_t iterations = 25;

    std::vector<Handle<Buffer<uint32_t>>> buffers;
    for (uint32_t i = 0; i < recordingThreads + immediateThreads; ++i) {
        buffers.push_back(createHostBuffer(ctx, 4));
    }

    std::atomic<bool> done{false};
    std::vector<std::thread> threads;

    // Each recording thread owns a command pool and records into it without any locking,
    // submitting through the context's locked queue
    for (uint32_t t = 0; t < recordingThreads; ++t) {
        threads.emplace_back([&ctx, &buffers, t]() {
            auto pool = ctx.createCommandPool(QueueType::Graphics);
            VkCommandBuffer commandBuffer = pool->allocateOne();

            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            std::unique_ptr<VulkanFence> fence;
            VK_SUCCESS_OR_THROW(VulkanFence::create(fence, ctx.device(), fenceInfo), "Failed to create fence");

            for (uint32_t i = 0; i < iterations; ++i) {
                VkCommandBufferBeginInfo beginInfo{};
                beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                vkBeginCommandBuffer(commandBuffer, &beginInfo);
                vkCmdFillBuffer(commandBuffer, buffers[t]->getBuffer(), 0, VK_WHOLE_SIZE, t * 1000 + i);
                vkEndCommandBuffer(commandBuffer);

                VkSubmitInfo submitInfo{};
                submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submitInfo.commandBufferCount = 1;
                submitInfo.pCommandBuffers = &commandBuffer;
                VK_SUCCESS_OR_THROW(ctx.submit(QueueType::Graphics, submitInfo, **fence), "Failed to submit");

                vkWaitForFences(ctx.device(), 1, fence->get(), VK_TRUE, UINT64_MAX);
                vkResetFences(ctx.device(), 1, fence->get());
            }
        });
    }

    // submitImmediate shares one context-owned pool, so it must be safe to call concurrently
    for (uint32_t t = 0; t < immediateThreads; ++t) {
        threads.emplace_back([&ctx, &buffers, t]() {
            auto& buffer = *buffers[recordingThreads + t];
            for (uint32_t i = 0; i < iterations; ++i) {
                ctx.submitImmediate([&buffer, t, i](VkCommandBuffer commandBuffer) {
                    vkCmdFillBuffer(commandBuffer, buffer.getBuffer(), 0, VK_WHOLE_SIZE, 50000 + t * 1000 + i);
                });
            }
        });
    }

    // Waiting for idle takes every queue lock while the other threads submit
    std::thread waiter([&ctx, &done]() {
        while (!done) {
            ctx.waitIdleAll();
            ctx.waitIdle(QueueType::Compute);
        }
    });

    for (auto& thread : threads) {
        thread.join();
    }
    done = true;
    waiter.join();

    for (uint32_t t = 0; t < recordingThreads; ++t) {
        CAPTURE(t);
        CHECK(readFirst(*buffers[t]) == t * 1000 + iterations - 1);
    }
    for (uint32_t t = 0; t < immediateThreads; ++t) {
        CAPTURE(t);
        CHECK(readFirst(*buffers[recordingThreads + t]) == 50000 + t * 1000 + iterations - 1);
    }
}

TEST_CASE("Frames in flight can change between frames, resizing per-frame resources") {
    ValidatedContext validated;
    auto& ctx = *validated.context;
    CHECK(ctx.framesInFlight() == 2);
    CHECK(ctx.currentFrame() == 0);

    {
        PerFrameHandles<Buffer<uint32_t>> buffers(ctx, [&ctx](uint32_t) { return createHostBuffer(ctx, 4); });
        CHECK(buffers.size() == 2);
        const size_t baseline = ctx.resources().size();

        ctx.advanceFrame();
        CHECK(ctx.currentFrame() == 1);

        // Takes effect at the next frame boundary, not immediately
        ctx.setFramesInFlight(3);
        CHECK(ctx.framesInFlight() == 2);
        CHECK(buffers.size() == 2);

        ctx.advanceFrame();
        CHECK(ctx.framesInFlight() == 3);
        CHECK(ctx.currentFrame() == 0);
        CHECK(buffers.size() == 3);
        CHECK(ctx.resources().size() == baseline + 1);
        CHECK(buffers[2]->getBuffer() != VK_NULL_HANDLE);

        ctx.advanceFrame();
        ctx.advanceFrame();
        CHECK(ctx.currentFrame() == 2);
        ctx.advanceFrame();
        CHECK(ctx.currentFrame() == 0);

        ctx.setFramesInFlight(1);
        ctx.advanceFrame();
        CHECK(buffers.size() == 1);
        CHECK(ctx.resources().size() == baseline - 1);

        CHECK_THROWS_AS(ctx.setFramesInFlight(0), std::invalid_argument);
    }
    CHECK(ctx.frames().size() == 0);
}

TEST_CASE("The initial number of frames in flight is configurable") {
    VulkanContextConfig config;
    config.framesInFlight = 3;
    VulkanContext ctx(std::move(config));
    CHECK(ctx.framesInFlight() == 3);
    PerFrame<int> values(ctx, [](uint32_t frame) { return static_cast<int>(frame); });
    CHECK(values.size() == 3);
}
