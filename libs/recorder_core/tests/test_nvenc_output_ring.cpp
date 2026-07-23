// Pure unit tests for FindFreeOutputSlot — the async output-ring free-slot scan
// backing the S8 submit path's output-buffer allocation. Same round-robin
// pattern as the existing 8-slot input ring's AcquireFreeSlot, generalised into
// a pure function over an explicit in-flight array. No GPU/NVENC session.

#include "nvenc_encoder.h"

#include <vector>

#include <gtest/gtest.h>

using namespace recorder_core;

namespace {

TEST(FindFreeOutputSlot, AllFree_ReturnsCursorSlot) {
    const bool in_flight[4] = {false, false, false, false};
    const FreeOutputSlotResult r = FindFreeOutputSlot(in_flight, 4, /*cursor=*/0);
    EXPECT_EQ(r.slot_idx, 0);
    EXPECT_EQ(r.next_cursor, 1);
}

TEST(FindFreeOutputSlot, StartsScanFromCursor) {
    const bool in_flight[4] = {false, false, false, false};
    const FreeOutputSlotResult r = FindFreeOutputSlot(in_flight, 4, /*cursor=*/2);
    EXPECT_EQ(r.slot_idx, 2);
    EXPECT_EQ(r.next_cursor, 3);
}

TEST(FindFreeOutputSlot, SkipsInFlightSlots) {
    // Slots 0,1 in flight; scan from cursor 0 must land on the first free one (2).
    const bool in_flight[4] = {true, true, false, false};
    const FreeOutputSlotResult r = FindFreeOutputSlot(in_flight, 4, /*cursor=*/0);
    EXPECT_EQ(r.slot_idx, 2);
    EXPECT_EQ(r.next_cursor, 3);
}

TEST(FindFreeOutputSlot, WrapsAroundPastTheEnd) {
    // Cursor starts at 3 (last index); only slot 1 is free -> must wrap around.
    const bool in_flight[4] = {true, false, true, true};
    const FreeOutputSlotResult r = FindFreeOutputSlot(in_flight, 4, /*cursor=*/3);
    EXPECT_EQ(r.slot_idx, 1);
    EXPECT_EQ(r.next_cursor, 2);
}

TEST(FindFreeOutputSlot, AllInFlight_ReturnsNegativeOneAndUnchangedCursor) {
    const bool in_flight[4] = {true, true, true, true};
    const FreeOutputSlotResult r = FindFreeOutputSlot(in_flight, 4, /*cursor=*/2);
    EXPECT_EQ(r.slot_idx, -1);
    EXPECT_EQ(r.next_cursor, 2);
}

TEST(FindFreeOutputSlot, SingleSlotRing) {
    // The depth-1 default (M-1 Rev. 3/4): a one-element ring must still behave.
    const bool freeSlot[1] = {false};
    const FreeOutputSlotResult free = FindFreeOutputSlot(freeSlot, 1, 0);
    EXPECT_EQ(free.slot_idx, 0);
    EXPECT_EQ(free.next_cursor, 0);

    const bool busySlot[1] = {true};
    const FreeOutputSlotResult busy = FindFreeOutputSlot(busySlot, 1, 0);
    EXPECT_EQ(busy.slot_idx, -1);
}

TEST(FindFreeOutputSlot, ZeroCount_ReturnsNegativeOne) {
    // Defensive: an empty ring (never produced in practice — depth is clamped
    // to >= 1) must not read out of bounds or crash.
    const FreeOutputSlotResult r = FindFreeOutputSlot(nullptr, 0, 0);
    EXPECT_EQ(r.slot_idx, -1);
}

} // namespace
