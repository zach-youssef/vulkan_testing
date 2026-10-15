#include "descriptors/DescriptorHeap.h"

#include <doctest/doctest.h>

TEST_CASE("SlotAllocator hands out slots up to its capacity and reuses freed ones") {
    SlotAllocator slots(3);
    CHECK(slots.capacity() == 3);
    CHECK(slots.allocate() == 0u);
    CHECK(slots.allocate() == 1u);
    CHECK(slots.allocate() == 2u);
    CHECK_FALSE(slots.allocate());
    CHECK(slots.used() == 3);

    slots.free(1);
    slots.free(0);
    CHECK(slots.used() == 1);
    // Most recently freed first
    CHECK(slots.allocate() == 0u);
    CHECK(slots.allocate() == 1u);
    CHECK_FALSE(slots.allocate());
}

TEST_CASE("SlotAllocator with no capacity never allocates") {
    SlotAllocator slots;
    CHECK_FALSE(slots.allocate());
    CHECK(slots.used() == 0);
}
