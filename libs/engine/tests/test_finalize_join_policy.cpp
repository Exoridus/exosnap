// Pure decoupling of recording shutdown into two phases, with a progress-based
// finalize wait. This encodes the fix for a false-timeout defect: on stop, ALL
// workers (video, audio, AND the mux thread's finalize — Cues -> Render, which is
// O(keyframes/duration) and disk-bound) shared ONE fixed 120 s join budget. On a
// long recording finalising to a NAS, finalize can legitimately run past 120 s
// while writing bytes the whole time, yet was reported as a hung worker
// (m=TIMEOUT) and the file left effectively unfinalised.
//
// The policy pinned here is D3D-free / wall-clock-free (time and bytes are
// parameters):
//
//   FinalizeProgressTracker — decides KeepWaiting vs StalledAbort from the
//     finalize byte count. As long as bytes keep growing it keeps waiting, no
//     matter how long finalize takes; it aborts ONLY when no byte progress is
//     observed for a whole stall window (a real stall, not slow-but-working), or
//     when an optional hard cap is exceeded.
//
//   ClassifyShutdownFault — separates the two failure kinds so they are reported
//     distinctly: a producer worker that failed to flush/join in the short
//     Phase-1 budget (WorkerHang) vs a finalize that genuinely stalled in Phase 2
//     (FinalizeStalled).

#include "finalize_join_policy.h"

#include <gtest/gtest.h>

using namespace exosnap::engine;

namespace {

// ---------------------------------------------------------------------------
// FinalizeProgressTracker
// ---------------------------------------------------------------------------

TEST(FinalizeProgress, FirstObservationKeepsWaiting) {
    FinalizeProgressTracker t(/*stall_window_ms=*/1000);
    EXPECT_EQ(t.Observe(/*bytes=*/0, /*now_ms=*/0), FinalizeWaitDecision::KeepWaiting);
}

// THE CORE CASE: a slow but continuously PROGRESSING finalize must never be cut
// off, even when each poll gap exceeds the stall window. Every observation shows
// the byte count growing, so the stall timer keeps resetting.
TEST(FinalizeProgress, SlowButProgressingIsNeverTruncated) {
    FinalizeProgressTracker t(/*stall_window_ms=*/500);
    uint64_t bytes = 0;
    // Poll every 1000 ms (twice the stall window) for a long "12h-to-NAS" style
    // finalize; bytes grow a little each poll.
    for (uint64_t tick = 0; tick <= 60; ++tick) {
        const uint64_t now = tick * 1000;
        bytes += 4096; // steady disk progress
        EXPECT_EQ(t.Observe(bytes, now), FinalizeWaitDecision::KeepWaiting)
            << "finalize was making byte progress at tick " << tick << " and must not abort";
    }
}

// A finalize that genuinely stalls — no bytes written for a whole stall window —
// must abort so a truly wedged mux cannot block shutdown forever.
TEST(FinalizeProgress, TrueStallAborts) {
    FinalizeProgressTracker t(/*stall_window_ms=*/500);
    ASSERT_EQ(t.Observe(/*bytes=*/1000, /*now_ms=*/0), FinalizeWaitDecision::KeepWaiting);
    EXPECT_EQ(t.Observe(1000, 100), FinalizeWaitDecision::KeepWaiting);  // 100 ms flat
    EXPECT_EQ(t.Observe(1000, 400), FinalizeWaitDecision::KeepWaiting);  // 400 ms flat
    EXPECT_EQ(t.Observe(1000, 499), FinalizeWaitDecision::KeepWaiting);  // just under window
    EXPECT_EQ(t.Observe(1000, 500), FinalizeWaitDecision::StalledAbort); // window elapsed, no progress
}

// Progress resets the stall timer: a burst of writes after a near-stall must
// keep the finalize alive, and the window is then measured from the LAST write.
TEST(FinalizeProgress, ProgressResetsTheStallTimer) {
    FinalizeProgressTracker t(/*stall_window_ms=*/500);
    ASSERT_EQ(t.Observe(0, 0), FinalizeWaitDecision::KeepWaiting);
    EXPECT_EQ(t.Observe(0, 400), FinalizeWaitDecision::KeepWaiting);   // 400 ms flat
    EXPECT_EQ(t.Observe(10, 450), FinalizeWaitDecision::KeepWaiting);  // progress! reset
    EXPECT_EQ(t.Observe(10, 900), FinalizeWaitDecision::KeepWaiting);  // 450 ms since reset
    EXPECT_EQ(t.Observe(10, 950), FinalizeWaitDecision::StalledAbort); // 500 ms since reset
}

// A byte count that drops (e.g. a fresh per-segment writer resets bytes_written)
// is treated as "no progress this tick", never as progress, and does not crash
// the unsigned arithmetic. It must still eventually stall if nothing grows.
TEST(FinalizeProgress, ByteCountResetIsNotProgress) {
    FinalizeProgressTracker t(/*stall_window_ms=*/500);
    ASSERT_EQ(t.Observe(9000, 0), FinalizeWaitDecision::KeepWaiting);
    EXPECT_EQ(t.Observe(50, 100), FinalizeWaitDecision::KeepWaiting);  // dropped, not progress
    EXPECT_EQ(t.Observe(50, 600), FinalizeWaitDecision::StalledAbort); // flat for a full window
}

// After a reset-drop, genuine growth above the new low resumes progress.
TEST(FinalizeProgress, GrowthAfterResetResumesProgress) {
    FinalizeProgressTracker t(/*stall_window_ms=*/500);
    ASSERT_EQ(t.Observe(9000, 0), FinalizeWaitDecision::KeepWaiting);
    EXPECT_EQ(t.Observe(50, 100), FinalizeWaitDecision::KeepWaiting);  // dropped
    EXPECT_EQ(t.Observe(80, 300), FinalizeWaitDecision::KeepWaiting);  // grew above the low
    EXPECT_EQ(t.Observe(80, 700), FinalizeWaitDecision::KeepWaiting);  // 400 ms since that growth
    EXPECT_EQ(t.Observe(80, 800), FinalizeWaitDecision::StalledAbort); // 500 ms since growth
}

// The optional hard cap is a safety valve: even a finalize that keeps inching
// forward is abandoned once total elapsed exceeds the cap. Disabled by default.
TEST(FinalizeProgress, HardCapAbortsEvenWithProgress) {
    FinalizeProgressTracker t(/*stall_window_ms=*/10000, /*hard_cap_ms=*/1000);
    uint64_t bytes = 0;
    ASSERT_EQ(t.Observe(bytes, 0), FinalizeWaitDecision::KeepWaiting);
    EXPECT_EQ(t.Observe(bytes += 1, 500), FinalizeWaitDecision::KeepWaiting);
    EXPECT_EQ(t.Observe(bytes += 1, 999), FinalizeWaitDecision::KeepWaiting);
    EXPECT_EQ(t.Observe(bytes += 1, 1000), FinalizeWaitDecision::StalledAbort); // cap reached
}

TEST(FinalizeProgress, HardCapDisabledByDefault) {
    FinalizeProgressTracker t(/*stall_window_ms=*/500);
    uint64_t bytes = 0;
    ASSERT_EQ(t.Observe(bytes, 0), FinalizeWaitDecision::KeepWaiting);
    // Far past any reasonable fixed timeout, but still progressing → keep waiting.
    EXPECT_EQ(t.Observe(bytes += 1, 10ull * 60 * 60 * 1000), FinalizeWaitDecision::KeepWaiting);
}

// ---------------------------------------------------------------------------
// ClassifyShutdownFault — error separation
// ---------------------------------------------------------------------------

TEST(ShutdownFaultClassify, CleanWhenBothPhasesSucceed) {
    EXPECT_EQ(ClassifyShutdownFault(/*producers_joined=*/true, /*finalize_completed=*/true), ShutdownFault::None);
}

TEST(ShutdownFaultClassify, ProducerHangReportedAsWorkerHang) {
    // A video/audio worker that failed to flush and join within the short Phase-1
    // budget is the real "worker hangs" fault — reported regardless of finalize.
    EXPECT_EQ(ClassifyShutdownFault(/*producers_joined=*/false, /*finalize_completed=*/true),
              ShutdownFault::WorkerHang);
    EXPECT_EQ(ClassifyShutdownFault(/*producers_joined=*/false, /*finalize_completed=*/false),
              ShutdownFault::WorkerHang);
}

TEST(ShutdownFaultClassify, FinalizeStallReportedDistinctly) {
    // Producers flushed cleanly; only finalize stalled. This must NOT be confused
    // with a hung producer worker — it is a distinct Phase-2 fault.
    EXPECT_EQ(ClassifyShutdownFault(/*producers_joined=*/true, /*finalize_completed=*/false),
              ShutdownFault::FinalizeStalled);
}

} // namespace
