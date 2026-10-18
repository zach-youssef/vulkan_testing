#pragma once

#include "core/VulkanContext.h"

#include <cstdint>
#include <optional>
#include <vector>

/*
 The copies a readback node recorded (one per frame slot at a time) that haven't been taken yet, to
 find the newest that has finished without waiting: what takeFinished() of readback nodes returns,
 for code polling between frames.
 */
class FinishedCopies {
public:
    // A copy recorded into frame slot frameIndex, replacing the slot's previous one
    void recorded(uint32_t frameIndex) {
        std::erase_if(pending_, [frameIndex](const Pending& p) { return p.frameIndex == frameIndex; });
        pending_.push_back({frameIndex, std::nullopt});
    }

    // The frame slot of the newest copy that has finished since the last one taken, if any (older
    // ones are dropped, superseded). slots: how many frame slots there are now (copies in removed
    // slots, when the number of frames in flight shrank, are dropped).
    std::optional<uint32_t> takeNewest(VulkanContext& context, size_t slots) {
        std::erase_if(pending_, [slots](const Pending& p) { return p.frameIndex >= slots; });
        std::optional<uint32_t> newest;
        for (auto& p : pending_) {
            // Copies recorded before now have been submitted, so the work submitted so far includes them
            if (!p.points) {
                p.points = context.submittedPoints();
            }
            if (context.isComplete(*p.points)) {
                newest = p.frameIndex;
            }
        }
        if (!newest) {
            return std::nullopt;
        }
        // Pending copies are in recording order
        while (!pending_.empty()) {
            const bool found = pending_.front().frameIndex == *newest;
            pending_.erase(pending_.begin());
            if (found) {
                break;
            }
        }
        return newest;
    }

private:
    // A copy recorded and not yet taken: its frame slot, and the work it was submitted with
    // (captured once it has been submitted)
    struct Pending {
        uint32_t frameIndex;
        std::optional<VulkanContext::TimelinePoints> points;
    };
    std::vector<Pending> pending_;
};
