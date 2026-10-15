#include "graph/GraphSync.h"

#include <doctest/doctest.h>

namespace {

constexpr VkPipelineStageFlags2 kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags2 kFragment = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
constexpr VkPipelineStageFlags2 kVertex = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;

SyncUse use(Access access, VkPipelineStageFlags2 stages, uint32_t queue, uint64_t value, bool discard = false) {
    auto result = SyncUse::of(access, stages, queue, discard);
    result.value = value;
    return result;
}

} // namespace

TEST_CASE("The first use of a new image transitions it from UNDEFINED without waiting on anything") {
    auto state = ResourceSyncState::forImage();
    auto ops = applyUse(state, use(Access::StorageWrite, kCompute, 0, 1));
    REQUIRE(ops.barrier);
    CHECK(ops.barrier->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
    CHECK(ops.barrier->newLayout == VK_IMAGE_LAYOUT_GENERAL);
    CHECK(ops.barrier->srcStages == VK_PIPELINE_STAGE_2_NONE);
    CHECK(ops.barrier->dstStages == kCompute);
    CHECK(ops.waits.empty());
    CHECK(state.layout == VK_IMAGE_LAYOUT_GENERAL);
}

TEST_CASE("A read after a write on the same queue makes the write visible and changes layout") {
    auto state = ResourceSyncState::forImage();
    applyUse(state, use(Access::StorageWrite, kCompute, 0, 1));
    auto ops = applyUse(state, use(Access::SampledRead, kFragment, 0, 1));
    REQUIRE(ops.barrier);
    CHECK(ops.barrier->srcStages == kCompute);
    CHECK(ops.barrier->srcAccess == VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    CHECK(ops.barrier->dstStages == kFragment);
    CHECK(ops.barrier->dstAccess == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    CHECK(ops.barrier->oldLayout == VK_IMAGE_LAYOUT_GENERAL);
    CHECK(ops.barrier->newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(ops.waits.empty());
}

TEST_CASE("Reads in the same layout after a barrier need nothing more, until a new stage reads") {
    auto state = ResourceSyncState::forBuffer();
    applyUse(state, use(Access::StorageBufferWrite, kCompute, 0, 1));
    auto first = applyUse(state, use(Access::StorageBufferRead, kFragment, 0, 1));
    CHECK(first.barrier);
    auto again = applyUse(state, use(Access::StorageBufferRead, kFragment, 0, 1));
    CHECK_FALSE(again.barrier);
    // The write hasn't been made visible to the vertex shader yet
    auto vertex = applyUse(state, use(Access::StorageBufferRead, kVertex, 0, 1));
    REQUIRE(vertex.barrier);
    CHECK(vertex.barrier->srcStages == kCompute);
    CHECK(vertex.barrier->dstStages == kVertex);
}

TEST_CASE("A write after reads waits for the reads without making anything available") {
    auto state = ResourceSyncState::forBuffer();
    applyUse(state, use(Access::StorageBufferRead, kFragment, 0, 1));
    applyUse(state, use(Access::StorageBufferRead, kVertex, 0, 1));
    auto ops = applyUse(state, use(Access::StorageBufferWrite, kCompute, 0, 1));
    REQUIRE(ops.barrier);
    CHECK(ops.barrier->srcStages == (kFragment | kVertex));
    CHECK(ops.barrier->srcAccess == VK_ACCESS_2_NONE);
}

TEST_CASE("A write after a write waits for and makes available the earlier write") {
    auto state = ResourceSyncState::forImage();
    applyUse(state, use(Access::ColorAttachment, 0, 0, 1));
    auto ops = applyUse(state, use(Access::ColorAttachment, 0, 0, 1));
    REQUIRE(ops.barrier);
    CHECK(ops.barrier->srcStages == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    CHECK((ops.barrier->srcAccess & VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT) != 0);
    CHECK(ops.barrier->oldLayout == ops.barrier->newLayout);
}

TEST_CASE("A use that discards contents transitions from UNDEFINED but still waits for earlier uses") {
    auto state = ResourceSyncState::forImage();
    applyUse(state, use(Access::SampledRead, kFragment, 0, 1));
    auto ops = applyUse(state, use(Access::ColorAttachment, 0, 0, 2, true));
    REQUIRE(ops.barrier);
    CHECK(ops.barrier->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
    CHECK(ops.barrier->srcStages == kFragment);
}

TEST_CASE("Uses on another queue wait on that queue's timeline") {
    auto state = ResourceSyncState::forImage();
    applyUse(state, use(Access::StorageWrite, kCompute, 1, 7));

    // A read in another layout: wait for the write, then transition starting from the waited stages
    auto read = applyUse(state, use(Access::SampledRead, kFragment, 0, 3));
    REQUIRE(read.waits.size() == 1);
    CHECK(read.waits[0].point == TimelinePoint{1, 7});
    CHECK(read.waits[0].stages == kFragment);
    REQUIRE(read.barrier);
    CHECK(read.barrier->srcStages == kFragment);
    CHECK(read.barrier->srcAccess == VK_ACCESS_2_NONE);
    CHECK(read.barrier->oldLayout == VK_IMAGE_LAYOUT_GENERAL);

    // A read on a third queue in the same layout: the transition on queue 0 counts as a write
    auto other = applyUse(state, use(Access::SampledRead, kCompute, 2, 4));
    REQUIRE(other.waits.size() == 1);
    CHECK(other.waits[0].point == TimelinePoint{0, 3});
    CHECK_FALSE(other.barrier);

    // A write back on queue 1 waits for the reads on queues 0 and 2
    auto write = applyUse(state, use(Access::StorageWrite, kCompute, 1, 8));
    CHECK(write.waits.size() == 2);
    REQUIRE(write.barrier);
    CHECK(write.barrier->newLayout == VK_IMAGE_LAYOUT_GENERAL);
}

TEST_CASE("A read on another queue in the same layout only needs a timeline wait") {
    auto state = ResourceSyncState::forBuffer();
    applyUse(state, use(Access::StorageBufferWrite, kCompute, 1, 5));
    auto ops = applyUse(state, use(Access::StorageBufferRead, kFragment, 0, 2));
    CHECK_FALSE(ops.barrier);
    REQUIRE(ops.waits.size() == 1);
    CHECK(ops.waits[0].point == TimelinePoint{1, 5});
}

TEST_CASE("Pending timeline values are resolved once submitted") {
    auto state = ResourceSyncState::forBuffer();
    applyUse(state, use(Access::StorageBufferWrite, kCompute, 1, TimelinePoint::kPendingValue));
    state.resolvePending(1, 42);
    auto ops = applyUse(state, use(Access::StorageBufferRead, kCompute, 0, 1));
    REQUIRE(ops.waits.size() == 1);
    CHECK(ops.waits[0].point == TimelinePoint{1, 42});
}

TEST_CASE("Waits on the same queue are merged to the latest value") {
    auto state = ResourceSyncState::forBuffer();
    applyUse(state, use(Access::StorageBufferRead, kCompute, 1, 3));
    applyUse(state, use(Access::StorageBufferRead, kCompute, 1, 9));
    auto ops = applyUse(state, use(Access::StorageBufferWrite, kCompute, 0, 1));
    REQUIRE(ops.waits.size() == 1);
    CHECK(ops.waits[0].point == TimelinePoint{1, 9});
}

TEST_CASE("Unknown state synchronizes conservatively") {
    auto state = ResourceSyncState::forImage(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    state.unknown = true;
    auto ops = applyUse(state, use(Access::SampledRead, kFragment, 0, 1));
    REQUIRE(ops.barrier);
    CHECK(ops.barrier->srcStages == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    CHECK(ops.barrier->srcAccess == VK_ACCESS_2_MEMORY_WRITE_BIT);
    CHECK(ops.barrier->oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK_FALSE(state.unknown);
}

TEST_CASE("An image acquired with a semaphore transitions after the semaphore's stages") {
    auto state = ResourceSyncState::forImage();
    state.externalSemaphore = true;
    auto ops = applyUse(state, use(Access::ColorAttachment, 0, 0, 1, true));
    REQUIRE(ops.barrier);
    CHECK(ops.barrier->srcStages == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    CHECK(ops.barrier->srcAccess == VK_ACCESS_2_NONE);
    CHECK(ops.barrier->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
}

TEST_CASE("State carries across frames: the next frame's first use synchronizes with the last") {
    auto state = ResourceSyncState::forImage();
    // Frame 1: compute writes on queue 1, graphics samples on queue 0
    applyUse(state, use(Access::StorageWrite, kCompute, 1, 1));
    applyUse(state, use(Access::SampledRead, kFragment, 0, 1));
    // Frame 2: compute writes again; it must wait for frame 1's sampling on queue 0
    auto ops = applyUse(state, use(Access::StorageWrite, kCompute, 1, 2));
    REQUIRE(ops.waits.size() == 1);
    CHECK(ops.waits[0].point == TimelinePoint{0, 1});
    REQUIRE(ops.barrier);
    CHECK(ops.barrier->oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(ops.barrier->newLayout == VK_IMAGE_LAYOUT_GENERAL);
}
