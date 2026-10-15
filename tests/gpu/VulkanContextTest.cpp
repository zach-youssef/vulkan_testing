#include "core/VulkanContext.h"
#include "resources/Buffer.h"

#include <doctest/doctest.h>

#include <atomic>
#include <set>
#include <thread>

namespace {

constexpr VkMemoryPropertyFlags kHostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

// A headless, validated context that records validation errors (which may arrive on any thread)
struct ValidatedContext {
    std::mutex mutex;
    std::vector<std::string> errors;
    std::unique_ptr<VulkanContext> context;

    ValidatedContext(bool syncValidation = true) {
        VulkanContextConfig config;
        config.applicationName = "VulkanContext tests";
        config.enableValidation = true;
        config.enableSynchronizationValidation = syncValidation;
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

TEST_CASE("Synchronization validation reports hazards when enabled") {
    std::mutex mutex;
    std::vector<std::string> errors;
    VulkanContextConfig config;
    config.enableValidation = true;
    config.enableSynchronizationValidation = true;
    config.onValidationMessage = [&](VkDebugUtilsMessageSeverityFlagBitsEXT severity, const char* message) {
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
            std::lock_guard<std::mutex> lock(mutex);
            errors.emplace_back(message);
        }
    };
    {
        VulkanContext ctx(std::move(config));
        if (!ctx.validationEnabled()) {
            MESSAGE("Validation layer unavailable; skipping");
            return;
        }
        Handle<Buffer<uint32_t>> buffer;
        REQUIRE(Buffer<uint32_t>::create(buffer, ctx, 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == VK_SUCCESS);
        // Two writes to the same buffer without a barrier between them: a write-after-write hazard
        ctx.submitImmediate([&buffer](VkCommandBuffer commandBuffer) {
            vkCmdFillBuffer(commandBuffer, buffer->getBuffer(), 0, VK_WHOLE_SIZE, 1);
            vkCmdFillBuffer(commandBuffer, buffer->getBuffer(), 0, VK_WHOLE_SIZE, 2);
        });
    }
    bool hazard = false;
    for (const auto& error : errors) {
        hazard |= error.find("WRITE_AFTER_WRITE hazard") != std::string::npos;
    }
    CHECK(hazard);
}

namespace {

// Records a one-off command buffer from pool
VkCommandBuffer recordOnce(CommandPool& pool, const std::function<void(VkCommandBuffer)>& op) {
    VkCommandBuffer commandBuffer = pool.allocateOne();
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    op(commandBuffer);
    vkEndCommandBuffer(commandBuffer);
    return commandBuffer;
}

VulkanContext::SubmitBatch batchOf(VkCommandBuffer commandBuffer) {
    VulkanContext::SubmitBatch batch;
    VkCommandBufferSubmitInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    info.commandBuffer = commandBuffer;
    batch.commandBuffers.push_back(info);
    return batch;
}

} // namespace

TEST_CASE("VulkanContext creates the compute-capable queues the device offers") {
    ValidatedContext validated;
    auto& ctx = *validated.context;

    REQUIRE(ctx.queueCount() >= 1);
    CHECK(ctx.queueCount() <= VulkanContext::kMaxQueues);
    const auto& primary = ctx.queueDescription(VulkanContext::kPrimaryQueue);
    CHECK((primary.flags & VK_QUEUE_GRAPHICS_BIT) != 0);
    CHECK((primary.flags & VK_QUEUE_COMPUTE_BIT) != 0);
    CHECK(ctx.queueIndex(QueueType::Graphics) == VulkanContext::kPrimaryQueue);
    CHECK(ctx.queueIndex(QueueType::Compute) == VulkanContext::kPrimaryQueue);

    std::set<VkQueue> distinct;
    std::set<uint32_t> families;
    for (uint32_t queue = 0; queue < ctx.queueCount(); ++queue) {
        CAPTURE(queue);
        CHECK((ctx.queueDescription(queue).flags & VK_QUEUE_COMPUTE_BIT) != 0);
        CHECK(ctx.timelineSemaphore(queue) != VK_NULL_HANDLE);
        distinct.insert(ctx.queue(queue));
        families.insert(ctx.queueDescription(queue).family);
    }
    CHECK(distinct.size() == ctx.queueCount());
    CHECK(std::set<uint32_t>(ctx.queueFamilies().begin(), ctx.queueFamilies().end()) == families);
    MESSAGE("Queues: " << ctx.queueCount() << " in " << families.size() << " families");
}

TEST_CASE("Submissions signal increasing timeline values") {
    ValidatedContext validated;
    auto& ctx = *validated.context;

    for (uint32_t queue = 0; queue < ctx.queueCount(); ++queue) {
        CAPTURE(queue);
        const uint64_t first = ctx.submit(queue, {});
        const uint64_t second = ctx.submit(queue, {});
        CHECK(second == first + 1);
        CHECK(ctx.submittedValue(queue) == second);
        ctx.wait(queue, second);
        CHECK(ctx.completedValue(queue) >= second);
    }
    auto points = ctx.submittedPoints();
    CHECK(points.size() == ctx.queueCount());
    ctx.wait(points);
    CHECK(ctx.isComplete(points));
}

TEST_CASE("Work on different queues is ordered by timeline waits") {
    ValidatedContext validated;
    auto& ctx = *validated.context;
    if (ctx.queueCount() < 2) {
        MESSAGE("Only one queue; skipping");
        return;
    }

    Handle<Buffer<uint32_t>> source;
    REQUIRE(Buffer<uint32_t>::create(source, ctx, 1024, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == VK_SUCCESS);
    auto destination = createHostBuffer(ctx, 1024);

    // Queue 1 fills the buffer; the primary queue copies it once queue 1's timeline is reached
    auto fillPool = ctx.createCommandPool(1u);
    auto copyPool = ctx.createCommandPool(VulkanContext::kPrimaryQueue);
    VkCommandBuffer fill = recordOnce(*fillPool, [&](VkCommandBuffer commandBuffer) {
        vkCmdFillBuffer(commandBuffer, source->getBuffer(), 0, VK_WHOLE_SIZE, 0xABCD1234);
    });
    VkCommandBuffer copy = recordOnce(*copyPool, [&](VkCommandBuffer commandBuffer) {
        VkBufferCopy region{0, 0, 1024 * sizeof(uint32_t)};
        vkCmdCopyBuffer(commandBuffer, source->getBuffer(), destination->getBuffer(), 1, &region);
    });

    const uint64_t filled = ctx.submit(1, batchOf(fill));
    auto copyBatch = batchOf(copy);
    VkSemaphoreSubmitInfo wait{};
    wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait.semaphore = ctx.timelineSemaphore(1);
    wait.value = filled;
    wait.stageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    copyBatch.waits.push_back(wait);
    const uint64_t copied = ctx.submit(VulkanContext::kPrimaryQueue, copyBatch);

    ctx.wait(VulkanContext::kPrimaryQueue, copied);
    CHECK(readFirst(*destination) == 0xABCD1234);
}

TEST_CASE("Retired functions run once the work submitted before them completes") {
    ValidatedContext validated;
    auto& ctx = *validated.context;

    int ran = 0;
    ctx.submit(VulkanContext::kPrimaryQueue, {});
    ctx.retire([&ran] { ++ran; });
    ctx.waitIdleAll();
    ctx.collectRetired();
    CHECK(ran == 1);
    ctx.collectRetired();
    CHECK(ran == 1);

    // Retiring with nothing outstanding runs at the next collection
    ctx.retire([&ran] { ++ran; });
    ctx.collectRetired();
    CHECK(ran == 2);

    // Anything still pending runs when the context is destroyed
    ctx.retire([&ran] { ++ran; });
    validated.context.reset();
    CHECK(ran == 3);
}

TEST_CASE("Frame slots only wait for their own previous work") {
    ValidatedContext validated;
    auto& ctx = *validated.context;
    REQUIRE(ctx.framesInFlight() == 2);

    // Frame 0 submits work, frame 1 submits work; beginning frame 0 again waits for frame 0's
    ctx.beginFrame();
    const uint64_t frame0 = ctx.submit(VulkanContext::kPrimaryQueue, {});
    ctx.advanceFrame();
    ctx.beginFrame();
    ctx.submit(VulkanContext::kPrimaryQueue, {});
    ctx.advanceFrame();
    CHECK(ctx.frameNumber() == 2);
    ctx.beginFrame();
    CHECK(ctx.completedValue(VulkanContext::kPrimaryQueue) >= frame0);
}

TEST_CASE("Shrinking frames in flight destroys removed slots only after their work completes") {
    ValidatedContext validated;
    auto& ctx = *validated.context;
    ctx.setFramesInFlight(3);
    ctx.advanceFrame();
    REQUIRE(ctx.framesInFlight() == 3);

    // Each slot's element records the timeline value of its frame's work
    std::vector<uint64_t> frameWork(3, 0);
    bool destroyedEarly = false;
    PerFrame<uint32_t> slots(ctx, [](uint32_t frame) { return frame; }, [&](uint32_t& frame) {
        destroyedEarly |= ctx.completedValue(VulkanContext::kPrimaryQueue) < frameWork[frame];
    });

    for (uint32_t frame = 0; frame < 3; ++frame) {
        ctx.beginFrame();
        frameWork[ctx.currentFrame()] = ctx.submit(VulkanContext::kPrimaryQueue, {});
        if (frame == 2) {
            ctx.setFramesInFlight(1);
        }
        ctx.advanceFrame();
    }
    CHECK(slots.size() == 1);
    CHECK_FALSE(destroyedEarly);
}
