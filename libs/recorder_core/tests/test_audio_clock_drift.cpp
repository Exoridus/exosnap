// AudioClockDriftEstimator: the pure math behind the honest A/V drift metric.
// Synthetic device-position/QPC sequences pin the sign convention (positive =
// audio leads video), baseline normalization, smoothing, and the property that
// a device underrun (discontinuity) does not disturb the estimate because the
// device position keeps counting through the gap.

#include <gtest/gtest.h>

#include "audio_clock_drift.h"

#include <recorder_core/interfaces/IAudioCaptureSource.h>

#include <cmath>
#include <cstdint>

namespace {

using recorder_core::AudioClockDriftEstimator;
using recorder_core::DeviceFramesToNs;

constexpr uint64_t kPacketNs = 10'000'000ULL; // 10 ms per capture packet

TEST(AudioClockDrift, NoObservations_NoEstimate) {
    AudioClockDriftEstimator est;
    EXPECT_FALSE(est.HasEstimate());
    EXPECT_DOUBLE_EQ(est.DriftMs(), 0.0);
}

TEST(AudioClockDrift, FirstObservationIsBaseline_ZeroDrift) {
    AudioClockDriftEstimator est;
    // Arbitrary large initial offsets on both axes must cancel out.
    est.AddObservation(/*device*/ 987'654'321'000ULL, /*qpc*/ 123'456'789'000ULL);
    EXPECT_TRUE(est.HasEstimate());
    EXPECT_DOUBLE_EQ(est.DriftMs(), 0.0);
}

TEST(AudioClockDrift, IdealClocks_DriftStaysZero) {
    AudioClockDriftEstimator est;
    for (uint64_t i = 0; i < 500; ++i) {
        est.AddObservation(i * kPacketNs, 5'000'000'000ULL + i * kPacketNs);
    }
    EXPECT_NEAR(est.DriftMs(), 0.0, 1e-9);
}

TEST(AudioClockDrift, FastDeviceClock_DriftGrowsNegative_AudioLags) {
    // Device clock 0.01% fast: for every real (QPC) 10 ms the device claims to
    // have produced 10.001 ms of audio. Audio events land at later PTS than
    // their video frames -> audio lags -> negative drift.
    AudioClockDriftEstimator est(/*window_size=*/8);
    double prev = 0.0;
    for (uint64_t i = 1; i <= 6000; ++i) { // one minute of packets
        const uint64_t qpc_ns = i * kPacketNs;
        const uint64_t device_ns = qpc_ns + (qpc_ns / 10000); // +0.01 %
        est.AddObservation(device_ns, qpc_ns);
        if (i % 1000 == 0) {
            const double now = est.DriftMs();
            EXPECT_LT(now, prev); // magnitude keeps growing, sign negative
            prev = now;
        }
    }
    // After 60 s at +0.01 % the device is ~6 ms ahead of QPC.
    EXPECT_NEAR(est.DriftMs(), -6.0, 0.1);
}

TEST(AudioClockDrift, SlowDeviceClock_DriftGrowsPositive_AudioLeads) {
    AudioClockDriftEstimator est(/*window_size=*/8);
    for (uint64_t i = 1; i <= 6000; ++i) {
        const uint64_t qpc_ns = i * kPacketNs;
        const uint64_t device_ns = qpc_ns - (qpc_ns / 10000); // -0.01 %
        est.AddObservation(device_ns, qpc_ns);
    }
    EXPECT_NEAR(est.DriftMs(), 6.0, 0.1);
}

TEST(AudioClockDrift, DiscontinuityGap_NoJump) {
    // A device underrun drops packets but the device position keeps counting:
    // the packet after the gap reports a position AND a QPC that both advanced
    // by the gap length. The estimate must sail through unchanged — this is
    // the same device-position property the gap fill uses to keep the encoder
    // sample axis continuous.
    AudioClockDriftEstimator est(/*window_size=*/16);
    uint64_t t_ns = 0;
    for (int i = 0; i < 100; ++i) {
        est.AddObservation(t_ns, t_ns);
        t_ns += kPacketNs;
    }
    ASSERT_NEAR(est.DriftMs(), 0.0, 1e-9);

    t_ns += 250 * kPacketNs; // 2.5 s of lost packets
    for (int i = 0; i < 100; ++i) {
        est.AddObservation(t_ns, t_ns);
        t_ns += kPacketNs;
    }
    EXPECT_NEAR(est.DriftMs(), 0.0, 1e-9);
}

TEST(AudioClockDrift, QpcJitterIsSmoothed) {
    // +-0.5 ms of alternating QPC timestamp jitter on otherwise ideal clocks:
    // the windowed mean must stay an order of magnitude below the per-packet
    // jitter amplitude instead of flapping the reported drift around.
    AudioClockDriftEstimator est; // default window
    est.AddObservation(0, 0);     // jitter-free baseline
    for (uint64_t i = 1; i <= 1000; ++i) {
        const uint64_t device_ns = i * kPacketNs;
        const int64_t jitter_ns = (i % 2 == 0) ? 500'000 : -500'000;
        est.AddObservation(device_ns, static_cast<uint64_t>(static_cast<int64_t>(device_ns) + jitter_ns));
        if (i >= 16) {
            EXPECT_LT(std::abs(est.DriftMs()), 0.05);
        }
    }
    EXPECT_NEAR(est.DriftMs(), 0.0, 0.01);
}

TEST(AudioClockDrift, WindowForgetsOldObservations) {
    // A constant offset that appears after the window has filled must fully
    // replace the old zero-drift samples once window_size new ones arrived.
    AudioClockDriftEstimator est(/*window_size=*/4);
    for (uint64_t i = 0; i < 10; ++i) {
        est.AddObservation(i * kPacketNs, i * kPacketNs);
    }
    for (uint64_t i = 10; i < 20; ++i) {
        est.AddObservation(i * kPacketNs, i * kPacketNs + 3'000'000ULL); // qpc +3 ms
    }
    EXPECT_NEAR(est.DriftMs(), 3.0, 1e-9);
}

TEST(AudioClockDrift, DeviceFramesToNs_NoOverflowOnLongRuns) {
    // 200 hours at 48 kHz: naive frames * 1e9 overflows uint64; the helper
    // must not.
    const uint64_t frames = 48'000ULL * 3600ULL * 200ULL;
    EXPECT_EQ(DeviceFramesToNs(frames, 48'000), 3600ULL * 200ULL * 1'000'000'000ULL);
    // Sub-second remainder stays sample-exact.
    EXPECT_EQ(DeviceFramesToNs(48'000 + 24'000, 48'000), 1'500'000'000ULL);
    EXPECT_EQ(DeviceFramesToNs(123, 0), 0ULL);
}

} // namespace
