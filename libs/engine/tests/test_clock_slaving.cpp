// ClockSlavingController: the pure P-controller behind gentle A/V clock slaving.
// These tests pin the engage latch, the P control law values, the slew and ppm
// caps, the min-step quantization, the 1 Hz update gating, and — the real proof
// that the three layers agree in sign — a closed-loop convergence simulation
// against a modelled plant (out = in * (1 + p*1e-6)).

#include <gtest/gtest.h>

#include "clock_slaving.h"

#include <cmath>
#include <cstdint>

namespace {

using exosnap::engine::ClockSlavingController;

constexpr uint64_t kSecondNs = 1'000'000'000ULL;

TEST(ClockSlaving, NoDrift_NeverEngages_PpmStaysZero) {
    ClockSlavingController c;
    for (uint64_t i = 0; i < 100; ++i) {
        const bool applied = c.Update(/*drift*/ 10.0, /*applied*/ 0.0, i * kSecondNs);
        EXPECT_FALSE(applied);
        EXPECT_FALSE(c.Engaged());
        EXPECT_DOUBLE_EQ(c.Ppm(), 0.0);
    }
}

TEST(ClockSlaving, BelowThreshold_JustUnder_NeverEngages) {
    ClockSlavingController c;
    // 14.9 ms is below the 15 ms engage threshold — must stay a no-op forever.
    for (uint64_t i = 0; i < 1000; ++i) {
        c.Update(14.9, 0.0, i * kSecondNs);
    }
    EXPECT_FALSE(c.Engaged());
    EXPECT_DOUBLE_EQ(c.Ppm(), 0.0);
}

TEST(ClockSlaving, ModestRate_EngagesFromTheRateNotFromTheAccumulatedError) {
    // 30 ppm, which is an ordinary crystal. The error it produces needs eight
    // minutes to reach the 15 ms engage threshold, and every second of that is
    // drift in the file. The rate is measurable after one minute, and that is
    // what has to arm the correction.
    ClockSlavingController c;
    int engaged_at_s = -1;
    for (int t = 0; t <= 300; ++t) {
        c.Update(/*drift*/ 0.03 * t, /*applied*/ 0.0, static_cast<uint64_t>(t) * kSecondNs);
        if (engaged_at_s < 0 && c.Engaged())
            engaged_at_s = t;
    }
    ASSERT_GE(engaged_at_s, 0) << "never engaged inside five minutes";
    EXPECT_LE(engaged_at_s, 90) << "waited for the error instead of reading the rate";
    // The drift the file carries at that point, versus the 15 ms it used to take.
    EXPECT_LT(0.03 * engaged_at_s, 3.0);
}

TEST(ClockSlaving, RateTooSmallToMatter_KeepsThePassthrough) {
    // 5 ppm reaches 3 ms over the ten-minute projection horizon -- far under the
    // engage threshold and far under audibility. Engaging would put every such
    // device through the resampler permanently for nothing, and the passthrough
    // is the thing being protected.
    ClockSlavingController c;
    for (int t = 0; t <= 600; ++t) {
        c.Update(0.005 * t, 0.0, static_cast<uint64_t>(t) * kSecondNs);
    }
    EXPECT_FALSE(c.Engaged());
    EXPECT_DOUBLE_EQ(c.Ppm(), 0.0);
}

TEST(ClockSlaving, ConstantOffsetIsNotARate) {
    // A fixed head start between the two clocks stays fixed: the slope is zero,
    // so the feed-forward path must not read it as a rate. Measuring
    // drift/elapsed instead of the slope would report 167 ppm at one minute and
    // engage on an offset that is not growing.
    ClockSlavingController c;
    for (int t = 0; t <= 600; ++t) {
        c.Update(10.0, 0.0, static_cast<uint64_t>(t) * kSecondNs);
    }
    EXPECT_FALSE(c.Engaged());
    EXPECT_DOUBLE_EQ(c.Ppm(), 0.0);
}

TEST(ClockSlaving, Engage_Latches_StaysEngagedAfterDriftDropsBack) {
    ClockSlavingController c;
    EXPECT_FALSE(c.Engaged());
    c.Update(16.0, 0.0, 0);
    EXPECT_TRUE(c.Engaged());
    // Drift falls back well below the threshold: the latch keeps it engaged so
    // the behaviour is monotone (no engage/disengage sawtooth).
    c.Update(1.0, 0.0, 5 * kSecondNs);
    EXPECT_TRUE(c.Engaged());
}

TEST(ClockSlaving, ResidualIsDriftMinusApplied_RefreshedEveryCall) {
    ClockSlavingController c;
    c.Update(/*drift*/ 20.0, /*applied*/ 8.0, 0);
    EXPECT_DOUBLE_EQ(c.ResidualMs(), 12.0);
    // Even a gated (sub-second) call refreshes the residual query.
    c.Update(20.0, 5.0, 10'000'000ULL); // 10 ms later, gated out
    EXPECT_DOUBLE_EQ(c.ResidualMs(), 15.0);
}

TEST(ClockSlaving, FirstEngagedStep_IsSlewLimited) {
    ClockSlavingController c;
    // Residual 15.1 ms -> p_target = 15.1/60*1000 = 251.7 ppm, but the first
    // step is slew-limited to kMaxSlewPpmPerS (125) over the nominal period.
    const bool applied = c.Update(15.1, 0.0, 0);
    EXPECT_TRUE(applied);
    EXPECT_NEAR(c.Ppm(), 125.0, 1e-9);
}

TEST(ClockSlaving, PositiveDrift_ProducesPositivePpm_SignContract) {
    // Positive drift (audio leads) must produce a positive ppm (stretch the
    // output). A sign inversion here would double the drift.
    ClockSlavingController c;
    c.Update(30.0, 0.0, 0);
    EXPECT_GT(c.Ppm(), 0.0);

    // Negative drift (audio lags) must produce a negative ppm (compress).
    ClockSlavingController c2;
    c2.Update(-30.0, 0.0, 0);
    EXPECT_LT(c2.Ppm(), 0.0);
}

TEST(ClockSlaving, SlewLimit_RampsAt125PpmPerSecond) {
    ClockSlavingController c;
    // Huge residual keeps p_target pinned at the cap; the rate must climb in
    // 125 ppm/s slew steps, not jump.
    EXPECT_TRUE(c.Update(1000.0, 0.0, 0 * kSecondNs));
    EXPECT_NEAR(c.Ppm(), 125.0, 1e-9);
    EXPECT_TRUE(c.Update(1000.0, 0.0, 1 * kSecondNs));
    EXPECT_NEAR(c.Ppm(), 250.0, 1e-9);
    EXPECT_TRUE(c.Update(1000.0, 0.0, 2 * kSecondNs));
    EXPECT_NEAR(c.Ppm(), 375.0, 1e-9);
    EXPECT_TRUE(c.Update(1000.0, 0.0, 3 * kSecondNs));
    EXPECT_NEAR(c.Ppm(), 500.0, 1e-9); // reaches the cap
}

TEST(ClockSlaving, PpmCap_NeverExceedsKMaxPpm) {
    ClockSlavingController c;
    // 200 ms: far past the 30 ms residual that already commands the cap, and
    // still a drift a pair of clocks could physically produce -- the
    // plausibility gate rejects a value no crystal can reach, and a test that
    // drove one would be exercising the gate instead of the cap.
    for (uint64_t i = 0; i < 100; ++i) {
        c.Update(200.0, 0.0, i * kSecondNs);
    }
    EXPECT_NEAR(c.Ppm(), ClockSlavingController::kMaxPpm, 1e-9);
    EXPECT_LE(c.Ppm(), ClockSlavingController::kMaxPpm + 1e-9);
}

TEST(ClockSlaving, MinPpmStep_SkipsSubStepNudges) {
    ClockSlavingController c;
    EXPECT_TRUE(c.Update(15.1, 0.0, 0)); // p = 125
    // Residual 7.8 -> p_target = 130 -> delta 5 ppm (< kMinPpmStep = 10): not
    // applied, p unchanged.
    const bool applied = c.Update(7.8, 0.0, 1 * kSecondNs);
    EXPECT_FALSE(applied);
    EXPECT_NEAR(c.Ppm(), 125.0, 1e-9);
}

TEST(ClockSlaving, UpdateGating_OncePerPeriodOverQpc) {
    ClockSlavingController c;
    EXPECT_TRUE(c.Update(30.0, 0.0, 0)); // engage + first eval -> p = 125
    EXPECT_NEAR(c.Ppm(), 125.0, 1e-9);
    // 0.5 s later: below the 1 s update period, gated out.
    EXPECT_FALSE(c.Update(30.0, 0.0, 500'000'000ULL));
    EXPECT_NEAR(c.Ppm(), 125.0, 1e-9);
    // 1.0 s after the first eval: re-evaluates, p advances one slew step.
    EXPECT_TRUE(c.Update(30.0, 0.0, 1 * kSecondNs));
    EXPECT_NEAR(c.Ppm(), 250.0, 1e-9);
}

// --- Closed-loop convergence: the real cross-layer sign/behaviour proof. ------
// Model a device crystal running at r ppm error. The raw drift the estimator
// reports grows unboundedly at r ppm (resampling shifts neither axis). The
// decorator's applied compensation A accumulates as the modelled plant stretches
// the output by the current ppm. Residual R = D - A must converge to the
// stationary residual R_ss = r * kControlHorizonS / 1000 that a P-controller
// leaves — NOT to zero (that would demand the deliberately-omitted PI integrator).
} // namespace

namespace {
struct LoopResult {
    double residual_ms = 0.0;
    double final_ppm = 0.0;
};
LoopResult SimulateClosedLoop(double rate_ppm, int seconds) {
    ClockSlavingController c;
    double drift_ms = 0.0;   // D: raw device-vs-QPC drift, grows at r ppm
    double applied_ms = 0.0; // A: cumulative applied compensation
    uint64_t qpc = 0;
    for (int s = 0; s < seconds; ++s) {
        drift_ms += rate_ppm / 1000.0; // r ppm = r/1000 ms per second
        qpc += kSecondNs;
        c.Update(drift_ms, applied_ms, qpc);
        // Plant: the current ppm stretches this second's output by p/1000 ms.
        applied_ms += c.Ppm() / 1000.0;
    }
    return {drift_ms - applied_ms, c.Ppm()};
}

TEST(ClockSlaving, ClosedLoop_100ppm_ConvergesToZeroResidual) {
    // The P-only controller parked here at R_ss = 100 * 60 / 1000 = 6 ms, and the
    // bound used to be 7. Feed-forward carries the standing rate, which leaves
    // the P term free to close the misalignment instead of sustaining the rate,
    // so the fixed point is zero. Bound at 1 ms: loose enough for the slewed
    // approach, far below the 6 ms the old design could not get under.
    const LoopResult r = SimulateClosedLoop(/*ppm*/ 100.0, /*seconds*/ 30 * 60);
    EXPECT_LT(std::abs(r.residual_ms), 1.0);
    // The rate settles in a ±20 ppm band around the true 100 ppm error and does
    // not run away.
    EXPECT_GT(r.final_ppm, 80.0);
    EXPECT_LT(r.final_ppm, 120.0);
}

TEST(ClockSlaving, ClosedLoop_NegativeRate_Symmetric) {
    const LoopResult r = SimulateClosedLoop(/*ppm*/ -100.0, /*seconds*/ 30 * 60);
    EXPECT_LT(std::abs(r.residual_ms), 1.0);
    EXPECT_LT(r.final_ppm, -80.0);
    EXPECT_GT(r.final_ppm, -120.0);
}

TEST(ClockSlaving, ClosedLoop_500ppm_ResidualFreezesAtTheCap) {
    // 500 ppm is the one rate the correction envelope cannot beat: p saturates at
    // kMaxPpm, which is exactly the drift rate, so compensation tracks drift and
    // the residual stops moving instead of closing. What is left is whatever
    // accumulated before the cap was reached -- which feed-forward shortens,
    // because the rate is commanded outright rather than climbed to through the
    // error it produces. Bound it, and pin that it does not close: a test that
    // only asserted "small" would pass on a controller that never engaged.
    const LoopResult r = SimulateClosedLoop(/*ppm*/ 500.0, /*seconds*/ 60 * 60);
    EXPECT_LT(std::abs(r.residual_ms), 25.0);
    EXPECT_GT(std::abs(r.residual_ms), 5.0);
    EXPECT_NEAR(r.final_ppm, ClockSlavingController::kMaxPpm, 1.0);
}

TEST(ClockSlaving, ImplausibleDrift_DoesNotEngage_AndLatchesFaulted) {
    // A frozen device position reads as drift the size of the stall. The engage
    // latch is permanent, so accepting one such sample would arm slaving for the
    // whole session on a measurement that describes nothing.
    ClockSlavingController c;
    const uint64_t t = 600ULL * 1'000'000'000ULL; // 600 s in
    EXPECT_FALSE(c.Update(0.0, 0.0, 0));          // establishes the elapsed origin
    EXPECT_FALSE(c.Update(830'000.0, 0.0, t));
    EXPECT_FALSE(c.Engaged());
    EXPECT_DOUBLE_EQ(c.Ppm(), 0.0);
    EXPECT_TRUE(c.MeasurementFaulted());
}

TEST(ClockSlaving, FaultLatches_AcrossLaterPlausibleSamples) {
    // A metric that silently returns to green hides the fault from every report
    // that reads it afterwards.
    ClockSlavingController c;
    EXPECT_FALSE(c.Update(0.0, 0.0, 0));
    EXPECT_FALSE(c.Update(500'000.0, 0.0, 300ULL * 1'000'000'000ULL));
    ASSERT_TRUE(c.MeasurementFaulted());
    for (uint64_t i = 301; i < 400; ++i) {
        c.Update(20.0, 0.0, i * 1'000'000'000ULL);
    }
    EXPECT_TRUE(c.MeasurementFaulted());
}

TEST(ClockSlaving, LargeButPlausibleDrift_StillEngages) {
    // The gate must not swallow the case slaving exists for. 200 ms after an
    // hour is a wildly out-of-spec crystal and still physically reachable.
    ClockSlavingController c;
    EXPECT_FALSE(c.Update(0.0, 0.0, 0));
    const uint64_t t = 3600ULL * 1'000'000'000ULL;
    EXPECT_TRUE(c.Update(200.0, 0.0, t));
    EXPECT_TRUE(c.Engaged());
    EXPECT_FALSE(c.MeasurementFaulted());
    EXPECT_GT(c.Ppm(), 0.0);
}

TEST(ClockSlaving, EarlyOffsetUnderTheFloor_IsNotAFault) {
    // Seconds into a session the rate-derived bound is near zero; the floor is
    // what keeps an ordinary startup offset from being called impossible.
    ClockSlavingController c;
    EXPECT_FALSE(c.Update(0.0, 0.0, 0));
    EXPECT_TRUE(c.Update(40.0, 0.0, 2ULL * 1'000'000'000ULL));
    EXPECT_FALSE(c.MeasurementFaulted());
    EXPECT_TRUE(c.Engaged());
}

} // namespace
