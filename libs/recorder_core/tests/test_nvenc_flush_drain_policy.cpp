// Pure policy for the bounded NVENC flush drain. The shutdown flush sends EOS then
// drains the encoder's buffered frames. Previously that drain used a blocking
// nvEncLockBitstream (doNotWait=0), so on a lost/hung device the lock never became
// ready and the video thread wedged — no video-EOS reached the mux, and the whole
// session died to the fixed 120 s join budget (the observed v=TIMEOUT hang).
//
// The fix drains with a non-blocking lock (doNotWait=1) and consults this policy
// after each attempt. The guarantee pinned here: the drain always terminates —
// once LOCK_BUSY persists past the time budget it aborts, and the caller pushes
// EOS anyway. Pure and D3D/GPU-free.

#include "nvenc_encoder.h"

#include <gtest/gtest.h>

using namespace recorder_core;

namespace {

constexpr double kBudgetMs = 2000.0;

TEST(NvencFlushDrain, ReadyPacketIsConsumed) {
    // A ready packet is always taken, regardless of elapsed time.
    EXPECT_EQ(NextFlushDrainStep(NV_ENC_SUCCESS, 0.0, kBudgetMs), FlushDrainStep::Consume);
    EXPECT_EQ(NextFlushDrainStep(NV_ENC_SUCCESS, kBudgetMs + 500.0, kBudgetMs), FlushDrainStep::Consume);
}

TEST(NvencFlushDrain, BusyWithinBudgetRetries) {
    // Output not ready yet but still inside the budget: keep polling.
    EXPECT_EQ(NextFlushDrainStep(NV_ENC_ERR_LOCK_BUSY, 0.0, kBudgetMs), FlushDrainStep::Retry);
    EXPECT_EQ(NextFlushDrainStep(NV_ENC_ERR_LOCK_BUSY, kBudgetMs - 1.0, kBudgetMs), FlushDrainStep::Retry);
}

TEST(NvencFlushDrain, BusyPastBudgetAbortsInsteadOfWedging) {
    // The anti-wedge guarantee: a device that never delivers must not hang the
    // drain — past the budget we abort and let the caller finalise.
    EXPECT_EQ(NextFlushDrainStep(NV_ENC_ERR_LOCK_BUSY, kBudgetMs, kBudgetMs), FlushDrainStep::AbortTimeout);
    EXPECT_EQ(NextFlushDrainStep(NV_ENC_ERR_LOCK_BUSY, kBudgetMs + 1000.0, kBudgetMs), FlushDrainStep::AbortTimeout);
}

TEST(NvencFlushDrain, HardErrorAbortsImmediately) {
    // A device-lost / generic failure stops the drain at once (no point polling).
    EXPECT_EQ(NextFlushDrainStep(NV_ENC_ERR_DEVICE_NOT_EXIST, 0.0, kBudgetMs), FlushDrainStep::AbortError);
    EXPECT_EQ(NextFlushDrainStep(NV_ENC_ERR_GENERIC, 0.0, kBudgetMs), FlushDrainStep::AbortError);
}

} // namespace
