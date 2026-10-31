#include "graph/nodes/FinishedCopies.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace {

// One queue's timeline: frame k's work is submitted as point k + 1, and the GPU finishes points
// when told to
struct FakeTimeline {
    uint64_t submitted = 0;
    uint64_t completed = 0;

    std::vector<uint64_t> submittedPoints() const { return {submitted}; }
    bool isComplete(const std::vector<uint64_t>& points) const { return completed >= points.at(0); }
};

// A copy remembers the frame that recorded it
struct Copy {
    int frame = -1;
};

// A readback node's frames, as a RenderGraph runs them: each waits for its slot's previous use,
// prepares (reuse), records and submits. A copy's points are the work submitted when it's first
// polled, so tests poll between frames to give each copy its own frame's.
struct Frames {
    explicit Frames(uint32_t slots) : copies(slots) {}

    // Runs frame `frame` (into slot frame % slots); with record false, it's prepared but skipped
    // (e.g. the surface was out of date)
    void run(int frame, bool record = true) {
        const uint32_t slot = static_cast<uint32_t>(frame) % static_cast<uint32_t>(copies.size());
        // The graph waits for the slot's previous use: frame - slots has finished
        const int previous = frame - static_cast<int>(copies.size());
        if (previous >= 0) {
            timeline.completed = std::max(timeline.completed, static_cast<uint64_t>(previous) + 1);
        }
        finished.reuse(copies, slot);
        if (record) {
            copies[slot].frame = frame;
            finished.recorded(slot);
        }
        timeline.submitted = static_cast<uint64_t>(frame) + 1;
    }

    // The frame of the newest finished copy not taken before, if any
    std::optional<int> take() {
        Copy* copy = finished.takeNewest(copies, timeline);
        return copy ? std::optional<int>(copy->frame) : std::nullopt;
    }

    std::optional<int> latest() {
        Copy* copy = finished.latest(copies);
        return copy ? std::optional<int>(copy->frame) : std::nullopt;
    }

    std::vector<Copy> copies;
    FakeTimeline timeline;
    FinishedCopies<Copy> finished;
};

} // namespace

TEST_CASE("FinishedCopies delivers copies whose slot is reused before they're polled") {
    // The GPU only finishes a frame when the graph waits for its slot, so polling before each
    // frame never sees a pending copy finished: the copy waited for must be kept, not dropped
    for (uint32_t slots : {1u, 2u, 3u}) {
        CAPTURE(slots);
        Frames frames(slots);
        const int count = 20;
        for (int frame = 0; frame < count; ++frame) {
            const std::optional<int> taken = frames.take();
            const int waited = frame - 1 - static_cast<int>(slots);
            if (waited >= 0) {
                REQUIRE(taken);
                CHECK(*taken == waited);
            } else {
                CHECK_FALSE(taken);
            }
            frames.run(frame);
        }
    }
}

TEST_CASE("FinishedCopies returns the newest finished copy once, dropping older ones") {
    Frames frames(2);
    CHECK_FALSE(frames.take());
    // Polled after each frame, so each copy's points are its own frame's: submitted, not finished
    frames.run(0);
    CHECK_FALSE(frames.take());
    frames.run(1);
    CHECK_FALSE(frames.take());
    frames.timeline.completed = 1;
    CHECK(frames.take() == 0);
    CHECK_FALSE(frames.take());
    frames.timeline.completed = 2;
    CHECK(frames.take() == 1);
    CHECK_FALSE(frames.take());

    // Both finish: the newest supersedes the older
    frames.run(2);
    frames.run(3);
    frames.timeline.completed = 4;
    CHECK(frames.take() == 3);
    CHECK_FALSE(frames.take());
}

TEST_CASE("FinishedCopies drops a kept copy once a newer one has finished, and keeps only the newest") {
    Frames frames(2);
    frames.run(0);
    CHECK_FALSE(frames.take());
    frames.run(1);
    CHECK_FALSE(frames.take());
    // Frame 2 reuses slot 0: frame 0's copy is kept
    frames.run(2);
    // Frame 1 finished too: it supersedes the kept one
    frames.timeline.completed = 2;
    CHECK(frames.take() == 1);
    CHECK_FALSE(frames.take());

    // Frame 4 reuses slot 0 with frame 2's copy finished and untaken: it's kept, and returned while
    // frame 3 hasn't finished (unpolled, its points include frame 4's work)
    frames.run(3);
    frames.run(4);
    CHECK(frames.take() == 2);
    CHECK_FALSE(frames.take());
    frames.timeline.completed = 5;
    CHECK(frames.take() == 4);
    CHECK_FALSE(frames.take());
}

TEST_CASE("FinishedCopies' latest copy is the most recently recorded, even kept aside") {
    Frames frames(1);
    CHECK_FALSE(frames.latest());
    frames.run(0);
    CHECK(frames.latest() == 0);
    // Prepared but skipped: frame 0's copy is kept aside, and still the latest
    frames.run(1, false);
    CHECK(frames.latest() == 0);
    CHECK(frames.take() == 0);
    CHECK(frames.latest() == 0);
    // Taken, so the next frame records over its slot's storage, not the kept one
    frames.run(2, false);
    CHECK(frames.latest() == 0);
    frames.run(3);
    CHECK(frames.latest() == 3);
}

TEST_CASE("FinishedCopies doesn't let an older copy displace a newer kept one") {
    // Slots reused out of order, as after the number of frames in flight changes
    std::vector<Copy> copies(2);
    FakeTimeline timeline;
    FinishedCopies<Copy> finished;
    finished.reuse(copies, 0);
    copies[0].frame = 0;
    finished.recorded(0);
    finished.reuse(copies, 1);
    copies[1].frame = 1;
    finished.recorded(1);
    timeline.submitted = 2;
    timeline.completed = 2;
    // Slot 1 (newer) reused first: its copy is kept, and the latest
    finished.reuse(copies, 1);
    CHECK(finished.latest(copies)->frame == 1);
    // Slot 0's older copy is superseded, so the kept copy's storage stays put
    finished.reuse(copies, 0);
    CHECK(finished.latest(copies)->frame == 1);
    Copy* taken = finished.takeNewest(copies, timeline);
    REQUIRE(taken);
    CHECK(taken->frame == 1);
    CHECK_FALSE(finished.takeNewest(copies, timeline));
}

TEST_CASE("FinishedCopies drops copies of removed slots, and rejects recording over an untaken copy") {
    Frames frames(3);
    frames.run(0);
    frames.run(1);
    frames.run(2);
    frames.timeline.completed = 3;
    // Two slots remain: frame 2's copy is gone, frame 1's is the newest left
    frames.copies.resize(2);
    CHECK(frames.take() == 1);
    CHECK_FALSE(frames.latest());

    frames.run(3);
    CHECK_THROWS_AS(frames.finished.recorded(1), ContractViolation);
}
