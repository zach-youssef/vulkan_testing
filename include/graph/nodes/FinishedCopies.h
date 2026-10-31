#pragma once

#include "core/Contract.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

/*
 Which of a readback node's copies to return: the newest that has finished since the last one
 taken, without waiting (what takeFinished() of readback nodes returns, for code polling between
 frames), and the most recently recorded one (what their waiting reads return).

 The node keeps a Copy (host memory and what it holds) per frame slot, recorded each time the slot
 runs the node. A finished copy is never dropped before it's taken unless a newer one has finished: when
 a slot is about to be recorded again while its copy is untaken (it has finished, since the graph
 waited for the slot), reuse() keeps that copy aside, swapping it with the storage of the copy kept
 aside before (which it supersedes). So polling finds it whether it runs before or after the graph
 waits for the slot, at the cost of a swap: no copying, no waiting, and at most one more Copy's
 storage, which the slot then reuses.

 takeNewest() returns only the newest finished copy: older ones, finished or not, are dropped, as
 are copies in frame slots removed when the number of frames in flight shrinks.

 Not synchronized: the node's user calls it on the thread executing the graph, between frames or
 from its nodes.
 */
template<typename Copy>
class FinishedCopies {
public:
    // Before frame slot `slot` is recorded again, once its previous use has finished (in
    // prepareFrame), with the node's copies per frame slot: if the slot's copy hasn't been taken,
    // it's kept aside and the slot's copy becomes storage to record into (a default Copy, or the
    // storage of a copy no longer needed).
    template<typename Copies>
    void reuse(Copies& copies, uint32_t slot) {
        dropRemovedSlots(copies.size());
        auto it = find(slot);
        if (it == pending_.end()) {
            return;
        }
        const uint64_t sequence = it->sequence;
        pending_.erase(it);
        // A newer copy kept aside (finished too) supersedes it; that copy's storage stays
        if (sequence < keptSequence_) {
            return;
        }
        std::swap(copies[slot], kept_);
        keptSequence_ = sequence;
        hasKept_ = true;
        if (latest_ == slot) {
            latest_ = kKept;
        }
    }

    // A copy recorded into frame slot `slot`, after reuse() (which keeps aside the slot's untaken copy)
    void recorded(uint32_t slot) {
        if (find(slot) != pending_.end()) {
            contractViolation("FinishedCopies::recorded called for a slot whose untaken copy wasn't reused first");
        }
        pending_.push_back({slot, ++sequence_, std::nullopt});
        latest_ = slot;
    }

    // The newest copy that has finished since the last one taken, or null if none has, without
    // waiting. copies: the node's copies per frame slot (as many as there are slots now).
    // timeline: what the copies were submitted to (a VulkanContext, or a fake in tests), with
    // submittedPoints() and isComplete(points). The copy stays valid until the next reuse().
    template<typename Copies, typename Timeline>
    Copy* takeNewest(Copies& copies, Timeline& timeline) {
        dropRemovedSlots(copies.size());
        std::optional<size_t> newest;
        for (size_t i = 0; i < pending_.size(); ++i) {
            auto& p = pending_[i];
            // Copies recorded before now have been submitted, so the work submitted so far includes them
            if (!p.points) {
                p.points = timeline.submittedPoints();
            }
            if (timeline.isComplete(*p.points)) {
                newest = i;
            }
        }
        // The copy kept aside has finished; a pending one is newer if recorded after it
        if (newest && (!hasKept_ || pending_[*newest].sequence > keptSequence_)) {
            const uint32_t slot = pending_[*newest].slot;
            // Pending copies are in recording order: the older ones are superseded
            pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(*newest) + 1);
            hasKept_ = false;
            return &copies[slot];
        }
        if (hasKept_) {
            const uint64_t kept = keptSequence_;
            std::erase_if(pending_, [kept](const Pending& p) { return p.sequence < kept; });
            hasKept_ = false;
            return &kept_;
        }
        return nullptr;
    }

    // The most recently recorded copy, or null if none has been (or its slot was removed)
    template<typename Copies>
    Copy* latest(Copies& copies) {
        if (!latest_) {
            return nullptr;
        }
        if (*latest_ == kKept) {
            return &kept_;
        }
        return *latest_ < copies.size() ? &copies[*latest_] : nullptr;
    }

private:
    // latest_ when the most recent copy is the one kept aside
    static constexpr uint32_t kKept = std::numeric_limits<uint32_t>::max();

    // A copy recorded and not yet taken: its frame slot, its place in recording order, and the work
    // it was submitted with (VulkanContext::TimelinePoints, captured once it has been submitted)
    struct Pending {
        uint32_t slot;
        uint64_t sequence;
        std::optional<std::vector<uint64_t>> points;
    };

    // Forgets copies in frame slots removed when the number of frames in flight shrank
    void dropRemovedSlots(size_t slots) {
        std::erase_if(pending_, [slots](const Pending& p) { return p.slot >= slots; });
        if (latest_ && *latest_ != kKept && *latest_ >= slots) {
            latest_.reset();
        }
    }

    typename std::vector<Pending>::iterator find(uint32_t slot) {
        return std::find_if(pending_.begin(), pending_.end(), [slot](const Pending& p) { return p.slot == slot; });
    }

    // Untaken copies in their frame slots, in recording order
    std::vector<Pending> pending_;
    // Copies are numbered from 1 in recording order
    uint64_t sequence_ = 0;
    // A finished copy whose slot was reused before it was taken (when hasKept_), else storage
    // holding a taken or superseded copy (number keptSequence_; 0 for none), which a reused slot
    // whose copy is older than it doesn't swap with, as it may be the most recent copy
    Copy kept_{};
    uint64_t keptSequence_ = 0;
    bool hasKept_ = false;
    std::optional<uint32_t> latest_;
};
