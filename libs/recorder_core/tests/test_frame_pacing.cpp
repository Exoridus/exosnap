#include "recorder_core/frame_pacing.h"
#include <gtest/gtest.h>
#include <span>
using namespace recorder_core;

TEST(PacingRingSize, AdaptiveFromRatio) {
    EXPECT_EQ(ComputePacingRingSize(144, 60), 5u); // ceil(144/60)=3, +2 = 5
    EXPECT_EQ(ComputePacingRingSize(240, 60), 6u); // ceil(240/60)=4, +2 = 6
    EXPECT_EQ(ComputePacingRingSize(60, 60), 4u);  // ceil=1, +2=3 -> clamped up to min 4
}
TEST(PacingRingSize, ClampsToMax) {
    EXPECT_EQ(ComputePacingRingSize(1000, 60), 12u); // huge ratio clamps to 12
}
TEST(PacingRingSize, UnknownRefreshFallsBackTo8) {
    EXPECT_EQ(ComputePacingRingSize(0, 60), 8u);
    EXPECT_EQ(ComputePacingRingSize(144, 0), 8u);
}

TEST(SelectFrame, SmoothPicksNearestFresh) {
    const uint64_t ring[] = {100, 200, 300, 400}; // present QPCs, ascending
    // slot=250, last_emitted=0 → nearest is 200 (idx1) or 300 (idx2); |250-200|=|250-300| → tie→lower idx
    auto d = SelectFrameForSlot(std::span<const uint64_t>(ring), 250, 0, FramePacingMode::Smooth);
    EXPECT_TRUE(d.emit);
    EXPECT_EQ(d.index, 1u);         // 200 chosen
    EXPECT_EQ(d.newly_dropped, 1u); // entry 100 skipped (fresh, older than chosen)
}
TEST(SelectFrame, NewestPicksLastFresh) {
    const uint64_t ring[] = {100, 200, 300};
    auto d = SelectFrameForSlot(std::span<const uint64_t>(ring), 150, 0, FramePacingMode::Newest);
    EXPECT_TRUE(d.emit);
    EXPECT_EQ(d.index, 2u);         // newest (300)
    EXPECT_EQ(d.newly_dropped, 2u); // 100 and 200 skipped
}
TEST(SelectFrame, FiltersAlreadyEmitted) {
    const uint64_t ring[] = {100, 200, 300};
    // last_emitted=200 → only 300 is fresh
    auto d = SelectFrameForSlot(std::span<const uint64_t>(ring), 250, 200, FramePacingMode::Smooth);
    EXPECT_TRUE(d.emit);
    EXPECT_EQ(d.index, 2u);
    EXPECT_EQ(d.newly_dropped, 0u);
}
TEST(SelectFrame, NoFreshEntriesDuplicates) {
    const uint64_t ring[] = {100, 200};
    auto d = SelectFrameForSlot(std::span<const uint64_t>(ring), 999, 200, FramePacingMode::Smooth);
    EXPECT_FALSE(d.emit); // all <= last_emitted → duplicate
    EXPECT_EQ(d.newly_dropped, 0u);
}
TEST(SelectFrame, EmptyRingDuplicates) {
    auto d = SelectFrameForSlot(std::span<const uint64_t>(), 100, 0, FramePacingMode::Smooth);
    EXPECT_FALSE(d.emit);
}

// ---------------------------------------------------------------------------
// ComputeCatchUpSkip — a persistently starving encoder must not compress time.
// ---------------------------------------------------------------------------
TEST(CatchUpSkip, WithinBudgetSkipsNothing) {
    const uint64_t interval = 166667; // 60 fps in 100 ns
    const uint64_t maxCatchUp = 60;
    // Behind by exactly one catch-up budget (1 s): the ordinary loop absorbs it.
    EXPECT_EQ(ComputeCatchUpSkip(maxCatchUp * interval, interval, maxCatchUp), 0u);
    // Behind by half a budget: still nothing to skip.
    EXPECT_EQ(ComputeCatchUpSkip(30 * interval, interval, maxCatchUp), 0u);
}
TEST(CatchUpSkip, BeyondBudgetSkipsTheExcess) {
    const uint64_t interval = 166667;
    const uint64_t maxCatchUp = 60;
    // Behind by 150 frames → skip 150 - 60 (cushion) = 90.
    EXPECT_EQ(ComputeCatchUpSkip(150 * interval, interval, maxCatchUp), 90u);
    // A partial frame past the budget boundary rounds down (whole frames only).
    EXPECT_EQ(ComputeCatchUpSkip(61 * interval + interval / 2, interval, maxCatchUp), 1u);
}
TEST(CatchUpSkip, ZeroIntervalIsSafe) {
    EXPECT_EQ(ComputeCatchUpSkip(1000000, 0, 60), 0u);
}

// Simulate the CFR scheduler against an encoder that runs at half real time. The
// scheduler mirrors video_thread.cpp: wall clock advances, the bounded catch-up
// loop emits (capped by both the per-iteration budget and the encoder), and — with
// the fix — sustained lag resyncs the timeline by skipping (and dropping) indices.
namespace {
struct SlowEncoderSim {
    uint64_t frame_idx = 0; // media frames (emitted + skipped) => media time = idx * interval
    uint64_t drops = 0;     // indices skipped as real drops
    uint64_t final_lag_frames = 0;
};
SlowEncoderSim SimulateSlowEncoder(bool with_resync, int iterations) {
    const uint64_t interval = 166667;        // 60 fps, 100 ns
    const uint64_t maxCatchUp = 60;          // one second per iteration
    const uint64_t wallStepFrames = 60;      // one second of wall time per iteration
    const uint64_t encoderBudgetFrames = 30; // encoder keeps up with only half real time
    const int sustainedThreshold = 3;        // consecutive lagging iterations before resync
    uint64_t elapsed = 0, next_tick = 0;
    int sustainedLag = 0;
    SlowEncoderSim s;
    for (int it = 0; it < iterations; ++it) {
        elapsed += wallStepFrames * interval;
        if (with_resync) {
            const uint64_t lag = (elapsed > next_tick) ? (elapsed - next_tick) : 0;
            if (lag > maxCatchUp * interval)
                ++sustainedLag;
            else
                sustainedLag = 0;
            if (sustainedLag >= sustainedThreshold) {
                const uint64_t skip = ComputeCatchUpSkip(lag, interval, maxCatchUp);
                s.frame_idx += skip;
                next_tick += skip * interval;
                s.drops += skip;
                sustainedLag = 0;
            }
        }
        uint64_t emitted = 0;
        while (elapsed >= next_tick && emitted < maxCatchUp && emitted < encoderBudgetFrames) {
            ++s.frame_idx;
            next_tick += interval;
            ++emitted;
        }
    }
    s.final_lag_frames = (elapsed > next_tick) ? (elapsed - next_tick) / interval : 0;
    return s;
}
} // namespace

TEST(CatchUpSkip, WithoutResyncTheTimelineCompressesUnbounded) {
    const auto s = SimulateSlowEncoder(/*with_resync=*/false, /*iterations=*/100);
    // The media clock falls ~30 frames behind wall clock every iteration and nothing
    // catches it up, so it ends thousands of frames (many seconds) behind — and no
    // drops are ever recorded, so the compression is silent.
    EXPECT_GT(s.final_lag_frames, 2000u);
    EXPECT_EQ(s.drops, 0u);
}
TEST(CatchUpSkip, WithResyncTheTimelineStaysWallClockTrueAndDropsAreCounted) {
    const auto s = SimulateSlowEncoder(/*with_resync=*/true, /*iterations=*/100);
    const auto baseline = SimulateSlowEncoder(/*with_resync=*/false, /*iterations=*/100);
    // Media time now tracks the wall clock: the residual lag stays within a small
    // multiple of one catch-up budget instead of growing without bound.
    EXPECT_LT(s.final_lag_frames, 4u * 60u);
    EXPECT_LT(s.final_lag_frames, baseline.final_lag_frames / 4u);
    // The frames the encoder could never emit are counted as real drops, not hidden.
    EXPECT_GT(s.drops, 0u);
}

// ---------------------------------------------------------------------------
// ShouldRecompositeHeldScreen — a still desktop must not freeze the webcam.
// ---------------------------------------------------------------------------
using recorder_core::ShouldRecompositeHeldScreen;

TEST(HeldScreenRecomposite, StillDesktopWithLiveWebcamRecomposites) {
    EXPECT_TRUE(ShouldRecompositeHeldScreen(/*has_fresh_source=*/false, /*od_holding=*/false,
                                            /*dynamic_overlay_changed=*/true, /*has_held_screen=*/true));
}

TEST(HeldScreenRecomposite, FreshFrameCompositesThatInstead) {
    EXPECT_FALSE(ShouldRecompositeHeldScreen(true, false, true, true));
}

TEST(HeldScreenRecomposite, NoWebcamKeepsCheapDuplicate) {
    EXPECT_FALSE(ShouldRecompositeHeldScreen(false, false, /*dynamic_overlay_changed=*/false, true));
}

// Mid-reopen the capture's display-tied resources are gone; touching them is a crash.
TEST(HeldScreenRecomposite, HoldingNeverRecomposites) {
    EXPECT_FALSE(ShouldRecompositeHeldScreen(false, /*od_holding=*/true, true, true));
}

TEST(HeldScreenRecomposite, NothingHeldNothingToCompositeOnto) {
    EXPECT_FALSE(ShouldRecompositeHeldScreen(false, false, true, /*has_held_screen=*/false));
}

// The invariant the crash fix depends on: re-composition never happens while holding,
// whatever else is true.
TEST(HeldScreenRecomposite, HoldingDominatesEveryOtherInput) {
    for (const bool fresh : {false, true}) {
        for (const bool webcam : {false, true}) {
            for (const bool held : {false, true}) {
                EXPECT_FALSE(ShouldRecompositeHeldScreen(fresh, /*od_holding=*/true, webcam, held));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// ClassifyCfrTickDrop — a lost frame must never be filed as benign pacing.
// ---------------------------------------------------------------------------
using recorder_core::CfrTickDropCause;
using recorder_core::ClassifyCfrTickDrop;

// The bug this classifier exists for: a VideoProcessorBlt (or input-view) failure
// happens with a source frame in hand, and used to be counted as CFR pacing.
TEST(CfrTickDrop, ConversionFailureWithASourceFrameIsAProcessingFailure) {
    EXPECT_EQ(ClassifyCfrTickDrop(/*had_source_frame=*/true, /*reference_storage_available=*/true),
              CfrTickDropCause::ProcessingFailure);
}

// Session start: nothing captured yet and nothing to hold. Genuinely benign.
TEST(CfrTickDrop, NoFrameYetIsBenignPacing) {
    EXPECT_EQ(ClassifyCfrTickDrop(/*had_source_frame=*/false, /*reference_storage_available=*/true),
              CfrTickDropCause::Pacing);
}

// The reference texture failed to allocate: every still-source tick loses a frame
// for the rest of the session, which is a failure, not pacing.
TEST(CfrTickDrop, MissingReferenceStorageIsAProcessingFailure) {
    EXPECT_EQ(ClassifyCfrTickDrop(/*had_source_frame=*/false, /*reference_storage_available=*/false),
              CfrTickDropCause::ProcessingFailure);
}

// Only one combination may ever be benign.
TEST(CfrTickDrop, PacingIsTheSoleBenignCombination) {
    for (const bool had_source : {false, true}) {
        for (const bool have_ref : {false, true}) {
            const bool benign = ClassifyCfrTickDrop(had_source, have_ref) == CfrTickDropCause::Pacing;
            EXPECT_EQ(benign, !had_source && have_ref);
        }
    }
}

// --- Held-screen ownership rotation --------------------------------------
// Modelled over identity handles: the production ring holds com_ptrs, and what a
// future rework can break is the rotation's direction and the no-aliasing rule,
// not the swap itself.
namespace {
struct RingSlot {
    int tex = 0;
    uint64_t present_qpc = 0;
};
} // namespace

TEST(HeldScreen, EmittedBecomesHeldAndPreviousHeldReturnsToRing) {
    RingSlot ring[]{{1, 100}, {2, 200}, {3, 300}};
    int held = 0; // the session's first frame

    // Slot 1 is emitted: consumed first, then rotated in.
    ring[1].present_qpc = 0;
    AdoptEmittedAsHeldScreen(ring[1].tex, held);

    EXPECT_EQ(held, 2);        // held screen is the emitted capture
    EXPECT_EQ(ring[1].tex, 0); // the previously held texture is the free slot
    EXPECT_EQ(ring[1].present_qpc, 0u);
}

TEST(HeldScreen, ConsecutiveEmitsRotateWithoutAliasingTheHeldTexture) {
    RingSlot ring[]{{1, 100}, {2, 200}, {3, 300}};
    int held = 0;

    for (std::size_t emitted : {std::size_t{0}, std::size_t{2}, std::size_t{1}}) {
        ring[emitted].present_qpc = 0;
        AdoptEmittedAsHeldScreen(ring[emitted].tex, held);
        // The drain may overwrite any free slot at any time, so the held screen
        // must not be one of them.
        for (const RingSlot& slot : ring)
            EXPECT_NE(slot.tex, held);
    }

    EXPECT_EQ(held, 2); // the capture emitted last, not the session's first frame
}
