// test_capture_drain_budget.cpp — how long the frame loop may keep draining.
//
// Origin: both capture drains were written as "keep going while the source has
// more". That terminates only as long as the source runs out. A source that
// stays ready -- a very high present rate, a virtual display driver that never
// reports empty, a backend replaying a backlog -- starves everything below the
// drain: the encode step, the pacing tick, and the stop check. The recording
// produces nothing and does not stop, and neither symptom points at the drain.
//
// The clock is a parameter, so an endlessly ready source is a loop in a test
// rather than a machine that has to be made fast enough to reproduce one.

#include <exosnap/engine/dxgi_od_capture_src.h>

#include <gtest/gtest.h>

#include <chrono>

using exosnap::engine::DrainBudgetForFrameInterval;
using exosnap::engine::DrainContinuation;
using exosnap::engine::NextDrainContinuation;
using namespace std::chrono_literals;

namespace {

// The budget a 60 fps session gets, so the cases below read in real numbers.
constexpr auto kInterval60 = std::chrono::microseconds{16667};

} // namespace

TEST(CaptureDrainBudget, DrainsWhileItHasBudget) {
    EXPECT_EQ(
        NextDrainContinuation(/*stop_requested=*/false, /*frames_drained=*/3, /*frame_budget=*/64, 1000us, 8000us),
        DrainContinuation::Continue);
}

TEST(CaptureDrainBudget, StopEndsTheDrainWithBudgetLeft) {
    // A stop must not have to wait for the budget: the drain is the only thing
    // between a stop request and the loop noticing it.
    EXPECT_EQ(NextDrainContinuation(/*stop_requested=*/true, /*frames_drained=*/0, /*frame_budget=*/64, 0us, 8000us),
              DrainContinuation::Stopped);
}

TEST(CaptureDrainBudget, TheFrameBudgetBoundsAnEndlesslyReadySource) {
    EXPECT_EQ(NextDrainContinuation(false, /*frames_drained=*/64, /*frame_budget=*/64, 0us, 8000us),
              DrainContinuation::FrameBudget);
}

TEST(CaptureDrainBudget, TheTimeBudgetBoundsASlowSource) {
    // Few frames, but each acquisition took long enough that the tick is spent.
    EXPECT_EQ(NextDrainContinuation(false, /*frames_drained=*/2, /*frame_budget=*/64, 8000us, 8000us),
              DrainContinuation::TimeBudget);
}

TEST(CaptureDrainBudget, AZeroBudgetIsUnbounded) {
    // Explicit, so a caller that means "no bound" says it, instead of passing a
    // number large enough to look like one.
    EXPECT_EQ(NextDrainContinuation(false, 1'000'000, /*frame_budget=*/0, 1h, 0us), DrainContinuation::Continue);
}

// The property the whole thing exists for: a source that is always ready is
// always left, on some axis, within a bounded number of iterations.
TEST(CaptureDrainBudget, AnAlwaysReadySourceCannotDrainForever) {
    const auto budget = DrainBudgetForFrameInterval(kInterval60);
    uint32_t frames = 0;
    auto elapsed = 0us;
    // Each iteration is one acquisition that succeeded instantly, which is the
    // pathological case: zero elapsed time means only the frame budget can stop
    // it.
    while (NextDrainContinuation(false, frames, budget.frames, elapsed, budget.time) == DrainContinuation::Continue) {
        ++frames;
        ASSERT_LT(frames, 100'000u) << "the drain never terminated against an always-ready source";
    }
    EXPECT_EQ(frames, budget.frames);
}

TEST(CaptureDrainBudget, ASlowAlwaysReadySourceIsBoundedByTime) {
    const auto budget = DrainBudgetForFrameInterval(kInterval60);
    uint32_t frames = 0;
    auto elapsed = 0us;
    // Each acquisition costs a millisecond, so the time budget is reached long
    // before the frame budget.
    while (NextDrainContinuation(false, frames, budget.frames, elapsed, budget.time) == DrainContinuation::Continue) {
        ++frames;
        elapsed += 1000us;
        ASSERT_LT(frames, 100'000u) << "the drain never terminated against a slow always-ready source";
    }
    EXPECT_LT(frames, budget.frames) << "time should have run out before the frame count did";
    EXPECT_EQ(NextDrainContinuation(false, frames, budget.frames, elapsed, budget.time), DrainContinuation::TimeBudget);
}

TEST(CaptureDrainBudget, StopIsObservedWithinOneAcquisition) {
    // The drain checks between acquisitions, so a stop raised during one is seen
    // by the next check and not later.
    const auto budget = DrainBudgetForFrameInterval(kInterval60);
    bool stop = false;
    uint32_t frames = 0;
    while (NextDrainContinuation(stop, frames, budget.frames, 0us, budget.time) == DrainContinuation::Continue) {
        ++frames;
        if (frames == 3)
            stop = true;
        ASSERT_LE(frames, 4u) << "the stop was not observed on the next check";
    }
    EXPECT_EQ(frames, 3u);
}

TEST(CaptureDrainBudget, HalfTheFrameIntervalAtOrdinaryRates) {
    // 60 fps is the ordinary case and must get the stated rule, not the ceiling:
    // the first ceiling chosen here was 8 ms, which silently clamped it.
    EXPECT_EQ(DrainBudgetForFrameInterval(kInterval60).time, std::chrono::microseconds{8333});
    EXPECT_EQ(DrainBudgetForFrameInterval(std::chrono::microseconds{33333}).time, std::chrono::microseconds{10000})
        << "30 fps must be clamped by the ceiling, not handed half of 33 ms";
}

TEST(CaptureDrainBudget, AVeryHighRateStillAllowsMoreThanOneAcquisition) {
    // 1000 fps: half the interval is 500 us, which is short enough that a single
    // acquisition could exceed it and the drain would never read a second frame.
    const auto budget = DrainBudgetForFrameInterval(std::chrono::microseconds{1000});
    EXPECT_GE(budget.time, std::chrono::microseconds{1000});
}

TEST(CaptureDrainBudget, ATimelapseRateDoesNotHideAStopForASecond) {
    // 1 fps. Half the interval would be half a second during which the loop
    // below the drain -- including the stop check -- never runs.
    const auto budget = DrainBudgetForFrameInterval(std::chrono::microseconds{1'000'000});
    EXPECT_LE(budget.time, std::chrono::microseconds{10000});
}
