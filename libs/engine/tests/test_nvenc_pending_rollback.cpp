// Which pending entry a rejected NVENC submission takes back.
//
// The pending FIFO is written before nvEncEncodePicture; when the driver rejects
// the picture, the entry has to come out again. It is the newest entry, at the
// back. The encoder used to pop the FRONT there -- the oldest frame still in
// flight, whose slot and output buffer the driver still owned -- and leave the
// rejected frame's entry in place. These cases drive the removal over a FIFO of
// the real entry type with no encoder session behind it.

#include "nvenc_encoder.h"

#include <deque>

#include <gtest/gtest.h>

using namespace exosnap::engine;

namespace {

// The entry type is private to the encoder; the removal is a template over any
// range of entries carrying `input_ts`, so the test's own mirror of it is
// enough and does not couple to the encoder's other fields.
struct Entry {
    uint64_t input_ts;
    int32_t slot_idx;
};

std::deque<Entry> Pending(std::initializer_list<Entry> entries) {
    return std::deque<Entry>(entries);
}

TEST(DiscardRejectedSubmission, TakesBackTheRejectedEntryNotTheOldest) {
    // A and B buffered, C just pushed and rejected.
    auto pending = Pending({{10, 0}, {11, 1}, {12, 2}});
    EXPECT_EQ(DiscardRejectedSubmission(pending, /*rejected_input_ts=*/12), 1u);
    ASSERT_EQ(pending.size(), 2u);
    EXPECT_EQ(pending.front().input_ts, 10u) << "A must still be the next frame to consume";
    EXPECT_EQ(pending.back().input_ts, 11u) << "B must still be in flight";
}

TEST(DiscardRejectedSubmission, TheOldFrontPopWouldHaveRemovedAFrameTheDriverStillOwned) {
    // The defect, stated as the property it violated: after the rollback, every
    // entry that was in flight before the rejected submission is still there.
    auto pending = Pending({{10, 0}, {11, 1}, {12, 2}});
    DiscardRejectedSubmission(pending, 12);
    for (const auto& entry : pending) {
        EXPECT_NE(entry.input_ts, 12u);
        EXPECT_TRUE(entry.input_ts == 10u || entry.input_ts == 11u)
            << "an in-flight entry was removed instead of the rejected one";
    }
}

TEST(DiscardRejectedSubmission, AnEmptyPipelineLeavesNothingBehind) {
    // C is the only frame: removing it empties the FIFO, which is what the old
    // front pop also did -- the one case where the two agreed.
    auto pending = Pending({{12, 2}});
    EXPECT_EQ(DiscardRejectedSubmission(pending, 12), 1u);
    EXPECT_TRUE(pending.empty());
}

TEST(DiscardRejectedSubmission, AlreadyConsumedMeansNothingToUndo) {
    // Sync mode: the lock pops the front before it validates, so when the lock
    // then fails, this submission's entry may already be gone. The old code
    // popped "one entry if present" -- the next frame's. Nothing must be removed.
    auto pending = Pending({{11, 1}});
    EXPECT_EQ(DiscardRejectedSubmission(pending, /*rejected_input_ts=*/10), 0u);
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending.front().input_ts, 11u);
}

TEST(DiscardRejectedSubmission, RemovesAtMostTheOneEntry) {
    // inputTimeStamp is unique per submission, so at most one entry can match.
    // The removal is written not to assume that, and the count says what it did.
    auto pending = Pending({{10, 0}, {11, 1}});
    EXPECT_EQ(DiscardRejectedSubmission(pending, 11), 1u);
    EXPECT_EQ(DiscardRejectedSubmission(pending, 11), 0u) << "a second removal of the same id must be a no-op";
    ASSERT_EQ(pending.size(), 1u);
}

TEST(DiscardRejectedSubmission, DoesNotTouchAnEmptyQueue) {
    std::deque<Entry> pending;
    EXPECT_EQ(DiscardRejectedSubmission(pending, 12), 0u);
    EXPECT_TRUE(pending.empty());
}

} // namespace
