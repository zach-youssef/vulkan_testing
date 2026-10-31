#pragma once

#include "graph/GraphTypes.h"

#include <algorithm>
#include <optional>
#include <vector>

/*
 Synchronization for the render graph, derived from tracked resource state.

 Each resource (each frame's copy, for per-frame resources) carries a ResourceSyncState between
 uses and between frames. applyUse() updates it for a new use and returns what must happen first:
 a pipeline barrier on the use's queue and/or timeline waits on other queues. Pure functions, so
 they are tested on the CPU.

 Rules:
    - A write or a layout change waits for the last write and every read since. On the same queue
      that is a barrier; on other queues, a wait on their timeline values.
    - A read waits for the last write: on the same queue with a barrier, until the write has been
      made visible to the read's stages and access; on another queue with a timeline wait.
    - Reads in the same layout as earlier reads need nothing more.
    - A use that discards contents transitions from UNDEFINED.
 */

// A value on a queue's timeline. kPendingValue marks a submission being recorded, whose value is
// known once it is submitted (see ResourceSyncState::resolvePending).
struct TimelinePoint {
    static constexpr uint64_t kPendingValue = UINT64_MAX;
    uint32_t queue = 0;
    uint64_t value = 0;
    bool operator==(const TimelinePoint&) const = default;
};

struct SyncBarrier {
    VkPipelineStageFlags2 srcStages = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 srcAccess = VK_ACCESS_2_NONE;
    VkPipelineStageFlags2 dstStages = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 dstAccess = VK_ACCESS_2_NONE;
    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool operator==(const SyncBarrier&) const = default;
};

struct SyncWait {
    TimelinePoint point;
    VkPipelineStageFlags2 stages;   // Stages of the use that wait
    bool operator==(const SyncWait&) const = default;
};

struct SyncOps {
    std::optional<SyncBarrier> barrier;
    std::vector<SyncWait> waits;
};

// A use of a resource on a queue
struct SyncUse {
    uint32_t queue = 0;
    uint64_t value = TimelinePoint::kPendingValue;   // Timeline value of the use's submission
    VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;   // Ignored for buffers
    bool read = false;
    bool write = false;
    bool discard = false;   // The previous contents are not needed (images: transition from UNDEFINED)

    static SyncUse of(Access access, VkPipelineStageFlags2 shaderStages, uint32_t queue, bool discard = false) {
        auto info = accessInfo(access, shaderStages);
        SyncUse use;
        use.queue = queue;
        use.stages = info.stages;
        use.access = info.access;
        use.layout = info.layout;
        use.read = info.read;
        use.write = info.write;
        use.discard = discard;
        return use;
    }
};

struct ResourceSyncState {
    bool image = true;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;

    // The last write (or layout transition), if any
    std::optional<TimelinePoint> write;
    VkPipelineStageFlags2 writeStages = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 writeAccess = VK_ACCESS_2_NONE;
    // Stages and accesses (on the write's queue) the last write has been made visible to
    VkPipelineStageFlags2 visibleStages = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 visibleAccess = VK_ACCESS_2_NONE;

    // Reads since the last write, one entry per queue
    struct Read {
        TimelinePoint point;
        VkPipelineStageFlags2 stages;
    };
    std::vector<Read> reads;

    // Something outside the tracked uses may have accessed the resource (another graph, the host):
    // the next use synchronizes conservatively with everything on its queue
    bool unknown = false;
    // The next use must wait on a semaphore at its own stages (e.g. the acquired swapchain image),
    // so its barrier's source must include those stages
    bool externalSemaphore = false;
    // A graph-created buffer whose contents are still to be zeroed (the graph fills it once, in
    // the first submission of the next frame using its frame slot)
    bool zeroFill = false;

    static ResourceSyncState forImage(VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED) {
        ResourceSyncState state;
        state.image = true;
        state.layout = layout;
        return state;
    }

    static ResourceSyncState forBuffer() {
        ResourceSyncState state;
        state.image = false;
        return state;
    }

    // Replaces pending timeline values (from a submission being recorded) with its real value
    void resolvePending(uint32_t queue, uint64_t value) {
        if (write && write->queue == queue && write->value == TimelinePoint::kPendingValue) {
            write->value = value;
        }
        for (auto& read : reads) {
            if (read.point.queue == queue && read.point.value == TimelinePoint::kPendingValue) {
                read.point.value = value;
            }
        }
    }
};

// Updates state for a use, returning the barrier (on the use's queue) and timeline waits (on other
// queues) that must come before it
inline SyncOps applyUse(ResourceSyncState& state, const SyncUse& use) {
    SyncOps ops;
    const bool layoutChange = state.image && (use.discard || use.layout != state.layout);
    const VkImageLayout oldLayout = use.discard ? VK_IMAGE_LAYOUT_UNDEFINED : state.layout;

    auto addWait = [&](const TimelinePoint& point) {
        if (point.value == 0) {
            return;
        }
        for (auto& wait : ops.waits) {
            if (wait.point.queue == point.queue) {
                wait.point.value = std::max(wait.point.value, point.value);
                return;
            }
        }
        ops.waits.push_back({point, use.stages});
    };

    VkPipelineStageFlags2 srcStages = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 srcAccess = VK_ACCESS_2_NONE;
    bool needBarrier = false;

    if (state.unknown) {
        srcStages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        srcAccess = VK_ACCESS_2_MEMORY_WRITE_BIT;
        needBarrier = true;
    }

    if (use.write || layoutChange) {
        // Wait for the last write and every read since
        if (state.write) {
            if (state.write->queue == use.queue) {
                srcStages |= state.writeStages;
                srcAccess |= state.writeAccess;
            } else {
                addWait(*state.write);
            }
        }
        for (const auto& read : state.reads) {
            if (read.point.queue == use.queue) {
                srcStages |= read.stages;
            } else {
                addWait(read.point);
            }
        }
        needBarrier |= layoutChange || srcStages != VK_PIPELINE_STAGE_2_NONE;
    } else if (state.write) {
        // A read waits for the last write until it is visible to the read
        if (state.write->queue != use.queue) {
            // A semaphore wait makes the write available and visible to the waiting queue
            addWait(*state.write);
        } else if ((use.stages & ~state.visibleStages) || (use.access & ~state.visibleAccess)) {
            srcStages |= state.writeStages;
            srcAccess |= state.writeAccess;
            needBarrier = true;
        }
    }

    // After semaphore waits (other queues, or an acquired image), a barrier must start from the
    // stages the semaphores were waited at to be ordered after them
    if (needBarrier && (!ops.waits.empty() || state.externalSemaphore)) {
        srcStages |= use.stages;
    }

    if (needBarrier) {
        SyncBarrier barrier;
        barrier.srcStages = srcStages;
        barrier.srcAccess = srcAccess;
        barrier.dstStages = use.stages;
        barrier.dstAccess = use.access;
        barrier.oldLayout = state.image ? oldLayout : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = state.image ? use.layout : VK_IMAGE_LAYOUT_UNDEFINED;
        ops.barrier = barrier;
    }

    // Update the state
    const TimelinePoint point{use.queue, use.value};
    if (use.write || layoutChange) {
        state.write = point;
        state.writeStages = use.stages;
        // A layout transition with no write leaves nothing to make available beyond the transition
        state.writeAccess = use.write ? use.access : VK_ACCESS_2_NONE;
        if (use.write) {
            // A new write is visible to nothing yet
            state.visibleStages = VK_PIPELINE_STAGE_2_NONE;
            state.visibleAccess = VK_ACCESS_2_NONE;
        } else {
            // The transition is visible to the stages it was made for
            state.visibleStages = use.stages;
            state.visibleAccess = use.access;
        }
        state.reads.clear();
        if (use.read && !use.write) {
            state.reads.push_back({point, use.stages});
        }
        state.layout = state.image ? use.layout : state.layout;
    } else {
        if (state.write && state.write->queue == use.queue && ops.barrier) {
            state.visibleStages |= use.stages;
            state.visibleAccess |= use.access;
        }
        bool merged = false;
        for (auto& read : state.reads) {
            if (read.point.queue == use.queue) {
                // A queue's values complete in order, so the latest read covers the earlier ones
                // (a pending value is the latest of all)
                read.stages |= use.stages;
                read.point.value = std::max(read.point.value, use.value);
                merged = true;
            }
        }
        if (!merged) {
            state.reads.push_back({point, use.stages});
        }
    }
    state.unknown = false;
    state.externalSemaphore = false;
    return ops;
}
