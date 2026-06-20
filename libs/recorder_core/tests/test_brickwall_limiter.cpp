#include <gtest/gtest.h>

#include "brickwall_limiter.h"

#include <cmath>
#include <vector>

using recorder_core::BrickwallLimiter;
using recorder_core::LimiterCeilingDbToLinear;

namespace {

// Build an interleaved stereo buffer where both channels carry the same value.
std::vector<float> ConstStereo(float value, uint32_t frames) {
    return std::vector<float>(static_cast<std::size_t>(frames) * 2u, value);
}

float MaxAbs(const std::vector<float>& buf) {
    float m = 0.0f;
    for (float s : buf) {
        m = std::max(m, std::fabs(s));
    }
    return m;
}

BrickwallLimiter::Config StereoConfig(float ceiling_linear) {
    BrickwallLimiter::Config cfg;
    cfg.ceiling_linear = ceiling_linear;
    cfg.attack_ms = 1.0f;
    cfg.release_ms = 50.0f;
    cfg.sample_rate = 48000;
    cfg.channels = 2;
    return cfg;
}

} // namespace

// ---------------------------------------------------------------------------
// dB → linear ceiling conversion
// ---------------------------------------------------------------------------

TEST(BrickwallLimiter, CeilingDbToLinear_ZeroIsUnity) {
    EXPECT_FLOAT_EQ(LimiterCeilingDbToLinear(0.0f), 1.0f);
}

TEST(BrickwallLimiter, CeilingDbToLinear_MinusSixIsAboutHalf) {
    EXPECT_NEAR(LimiterCeilingDbToLinear(-6.0f), 0.5012f, 1e-3f);
}

TEST(BrickwallLimiter, CeilingDbToLinear_PositiveClampedToUnity) {
    EXPECT_FLOAT_EQ(LimiterCeilingDbToLinear(3.0f), 1.0f);
}

TEST(BrickwallLimiter, CeilingDbToLinear_NonFiniteFallsBackToUnity) {
    EXPECT_FLOAT_EQ(LimiterCeilingDbToLinear(std::nanf("")), 1.0f);
    EXPECT_FLOAT_EQ(LimiterCeilingDbToLinear(INFINITY), 1.0f);
}

// ---------------------------------------------------------------------------
// Brickwall guarantee — output never exceeds the ceiling
// ---------------------------------------------------------------------------

TEST(BrickwallLimiter, NeverExceedsCeiling_LoudConstant) {
    BrickwallLimiter lim(StereoConfig(1.0f));
    auto buf = ConstStereo(4.0f, 2000); // way over full scale
    lim.Process(buf.data(), 2000);
    // No sample may exceed the ceiling (1.0), within float epsilon.
    EXPECT_LE(MaxAbs(buf), 1.0f + 1e-6f);
}

TEST(BrickwallLimiter, NeverExceedsCeiling_SuddenTransientFirstSample) {
    // A jump straight to a huge value: the envelope lags, so the final clamp
    // must catch the very first transient sample.
    BrickwallLimiter lim(StereoConfig(1.0f));
    auto buf = ConstStereo(10.0f, 64);
    lim.Process(buf.data(), 64);
    EXPECT_LE(MaxAbs(buf), 1.0f + 1e-6f);
    // The first frame in particular must already be within the ceiling.
    EXPECT_LE(std::fabs(buf[0]), 1.0f + 1e-6f);
    EXPECT_LE(std::fabs(buf[1]), 1.0f + 1e-6f);
}

TEST(BrickwallLimiter, NeverExceedsLoweredCeiling) {
    const float ceil = LimiterCeilingDbToLinear(-6.0f); // ~0.501
    BrickwallLimiter lim(StereoConfig(ceil));
    auto buf = ConstStereo(0.9f, 4000);
    lim.Process(buf.data(), 4000);
    EXPECT_LE(MaxAbs(buf), ceil + 1e-6f);
}

// ---------------------------------------------------------------------------
// Transparency — signal below the ceiling passes through unchanged
// ---------------------------------------------------------------------------

TEST(BrickwallLimiter, TransparentBelowCeiling) {
    BrickwallLimiter lim(StereoConfig(1.0f));
    auto buf = ConstStereo(0.5f, 1000); // well under the ceiling
    const auto original = buf;
    lim.Process(buf.data(), 1000);
    for (std::size_t i = 0; i < buf.size(); ++i) {
        EXPECT_FLOAT_EQ(buf[i], original[i]) << "index " << i;
    }
    EXPECT_FLOAT_EQ(lim.CurrentGain(), 1.0f);
}

TEST(BrickwallLimiter, SilencePassesThrough) {
    BrickwallLimiter lim(StereoConfig(1.0f));
    auto buf = ConstStereo(0.0f, 256);
    lim.Process(buf.data(), 256);
    EXPECT_FLOAT_EQ(MaxAbs(buf), 0.0f);
}

// ---------------------------------------------------------------------------
// Envelope behavior
// ---------------------------------------------------------------------------

TEST(BrickwallLimiter, GainConvergesTowardCeilingRatioUnderSustainedOver) {
    BrickwallLimiter lim(StereoConfig(1.0f));
    auto buf = ConstStereo(2.0f, 8000); // 2x over → steady-state gain ~0.5
    lim.Process(buf.data(), 8000);
    EXPECT_NEAR(lim.CurrentGain(), 0.5f, 1e-2f);
}

TEST(BrickwallLimiter, ReleaseRecoversTowardUnityAfterPeak) {
    BrickwallLimiter lim(StereoConfig(1.0f));
    // Drive it down with a loud block...
    auto loud = ConstStereo(4.0f, 4000);
    lim.Process(loud.data(), 4000);
    const float reduced_gain = lim.CurrentGain();
    EXPECT_LT(reduced_gain, 0.5f);
    // ...then feed quiet audio: gain should release back up toward unity.
    auto quiet = ConstStereo(0.1f, 48000); // ~1s of release time
    lim.Process(quiet.data(), 48000);
    EXPECT_GT(lim.CurrentGain(), reduced_gain);
    EXPECT_NEAR(lim.CurrentGain(), 1.0f, 1e-2f);
}

TEST(BrickwallLimiter, ResetClearsEnvelope) {
    BrickwallLimiter lim(StereoConfig(1.0f));
    auto loud = ConstStereo(4.0f, 4000);
    lim.Process(loud.data(), 4000);
    EXPECT_LT(lim.CurrentGain(), 1.0f);
    lim.Reset();
    EXPECT_FLOAT_EQ(lim.CurrentGain(), 1.0f);
}

// ---------------------------------------------------------------------------
// Stereo linking — both channels share one gain (image is preserved)
// ---------------------------------------------------------------------------

TEST(BrickwallLimiter, StereoLinked_LoudOnOneChannelAttenuatesBothEqually) {
    BrickwallLimiter lim(StereoConfig(1.0f));
    // Left loud, right quiet but non-zero; gain is driven by the louder channel
    // and applied to both, so their ratio is preserved.
    std::vector<float> buf;
    const uint32_t frames = 4000;
    buf.reserve(static_cast<std::size_t>(frames) * 2u);
    for (uint32_t f = 0; f < frames; ++f) {
        buf.push_back(2.0f); // L
        buf.push_back(0.2f); // R
    }
    lim.Process(buf.data(), frames);
    // After convergence, inspect a late frame: L:R ratio stays 10:1.
    const std::size_t i = (static_cast<std::size_t>(frames) - 1u) * 2u;
    EXPECT_GT(buf[i], 0.0f);
    EXPECT_NEAR(buf[i] / buf[i + 1], 10.0f, 1e-3f);
    EXPECT_LE(std::fabs(buf[i]), 1.0f + 1e-6f);
}

// ---------------------------------------------------------------------------
// Config sanitization
// ---------------------------------------------------------------------------

TEST(BrickwallLimiter, ConfigSanitizesDegenerateInputs) {
    BrickwallLimiter::Config cfg;
    cfg.ceiling_linear = 0.0f; // invalid → 1.0
    cfg.channels = 0;          // invalid → 1
    cfg.sample_rate = 0;       // invalid → 48000
    BrickwallLimiter lim(cfg);
    EXPECT_FLOAT_EQ(lim.GetConfig().ceiling_linear, 1.0f);
    EXPECT_EQ(lim.GetConfig().channels, 1u);
    EXPECT_EQ(lim.GetConfig().sample_rate, 48000u);
}

TEST(BrickwallLimiter, MonoProcessingRespectsCeiling) {
    BrickwallLimiter::Config cfg;
    cfg.ceiling_linear = 1.0f;
    cfg.channels = 1;
    cfg.sample_rate = 48000;
    BrickwallLimiter lim(cfg);
    std::vector<float> buf(1000, 3.0f);
    lim.Process(buf.data(), 1000);
    EXPECT_LE(MaxAbs(buf), 1.0f + 1e-6f);
}
