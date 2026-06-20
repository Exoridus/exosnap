#include <gtest/gtest.h>

#include "automatic_gain_control.h"

#include <cmath>
#include <vector>

using recorder_core::AutomaticGainControl;

namespace {

AutomaticGainControl::Config StereoConfig(float target_db) {
    AutomaticGainControl::Config cfg;
    cfg.target_db = target_db;
    cfg.max_gain_db = 30.0f;
    cfg.attack_ms = 50.0f;
    cfg.release_ms = 400.0f;
    cfg.noise_floor_db = -55.0f;
    cfg.sample_rate = 48000;
    cfg.channels = 2;
    return cfg;
}

// Fill an interleaved buffer with a constant amplitude on every channel.
std::vector<float> MakeConstant(uint32_t frames, uint32_t channels, float value) {
    return std::vector<float>(static_cast<std::size_t>(frames) * channels, value);
}

// Peak abs amplitude over the tail (last `tail_frames`) of an interleaved buffer.
float TailPeak(const std::vector<float>& buf, uint32_t channels, uint32_t tail_frames) {
    const std::size_t total = buf.size();
    const std::size_t tail = static_cast<std::size_t>(tail_frames) * channels;
    const std::size_t start = (tail < total) ? (total - tail) : 0u;
    float peak = 0.0f;
    for (std::size_t i = start; i < total; ++i) {
        peak = std::max(peak, std::fabs(buf[i]));
    }
    return peak;
}

float DbToLin(float db) {
    return std::pow(10.0f, db / 20.0f);
}

} // namespace

// ---------------------------------------------------------------------------
// A quiet sustained signal is boosted toward the target over time.
// ---------------------------------------------------------------------------

TEST(AutomaticGainControl, QuietSignalIsBoostedTowardTarget) {
    AutomaticGainControl agc(StereoConfig(-18.0f));
    const float target = DbToLin(-18.0f); // ≈ 0.126
    // Input ≈ -36 dBFS (0.0158): well above the noise floor, ~18 dB below target,
    // within the 30 dB max boost → the AGC should pull it up to the target.
    const float input = DbToLin(-36.0f);
    auto buf = MakeConstant(96000, 2, input); // 2 s — enough for the slow attack

    EXPECT_NEAR(agc.CurrentGain(), 1.0f, 1e-6f); // starts at unity
    agc.Process(buf.data(), 96000);

    // Gain has risen well above unity and the output level approaches the target.
    EXPECT_GT(agc.CurrentGain(), 2.0f);
    const float tail = TailPeak(buf, 2, 480);
    EXPECT_NEAR(tail, target, target * 0.1f);
}

// ---------------------------------------------------------------------------
// A loud signal is attenuated toward the target over time.
// ---------------------------------------------------------------------------

TEST(AutomaticGainControl, LoudSignalIsAttenuatedTowardTarget) {
    AutomaticGainControl agc(StereoConfig(-18.0f));
    const float target = DbToLin(-18.0f);
    // Input ≈ 0 dBFS (1.0): ~18 dB above target → the AGC pulls it down.
    const float input = 1.0f;
    auto buf = MakeConstant(96000, 2, input);

    agc.Process(buf.data(), 96000);

    EXPECT_LT(agc.CurrentGain(), 1.0f);
    const float tail = TailPeak(buf, 2, 480);
    EXPECT_NEAR(tail, target, target * 0.1f);
}

// ---------------------------------------------------------------------------
// max_gain clamp respected: a very quiet signal cannot be boosted past the cap.
// ---------------------------------------------------------------------------

TEST(AutomaticGainControl, MaxGainClampRespected) {
    AutomaticGainControl::Config cfg = StereoConfig(-18.0f);
    cfg.max_gain_db = 12.0f; // cap at +12 dB (≈ 3.98x)
    AutomaticGainControl agc(cfg);
    // Input ≈ -45 dBFS: above the -55 dB floor but ~27 dB below target. The
    // ideal makeup would be +27 dB, but the cap is +12 dB.
    const float input = DbToLin(-45.0f);
    auto buf = MakeConstant(192000, 2, input); // 4 s, ample time to converge

    agc.Process(buf.data(), 192000);

    const float max_lin = DbToLin(12.0f);
    EXPECT_LE(agc.CurrentGain(), max_lin + 1e-3f);
    EXPECT_NEAR(agc.CurrentGain(), max_lin, max_lin * 0.05f);
}

// ---------------------------------------------------------------------------
// Below the noise floor the gain does NOT boost (frozen) — no runaway gain.
// ---------------------------------------------------------------------------

TEST(AutomaticGainControl, BelowNoiseFloorGainIsFrozen) {
    AutomaticGainControl agc(StereoConfig(-18.0f)); // floor -55 dB
    // Input ≈ -70 dBFS: well below the -55 dB noise floor → freeze at unity.
    const float input = DbToLin(-70.0f);
    auto buf = MakeConstant(96000, 2, input);

    agc.Process(buf.data(), 96000);

    // The gain must stay at the starting unity value — it never cranks up on
    // near-silence (which would otherwise be a runaway-gain / noise-pump defect).
    EXPECT_NEAR(agc.CurrentGain(), 1.0f, 1e-3f);
}

// ---------------------------------------------------------------------------
// Config sanitization
// ---------------------------------------------------------------------------

TEST(AutomaticGainControl, ConfigSanitizesDegenerateInputs) {
    AutomaticGainControl::Config cfg;
    cfg.channels = 0;    // invalid → 1
    cfg.sample_rate = 0; // invalid → 48000
    AutomaticGainControl agc(cfg);
    EXPECT_EQ(agc.GetConfig().channels, 1u);
    EXPECT_EQ(agc.GetConfig().sample_rate, 48000u);
}

TEST(AutomaticGainControl, ConfigClampsChannelsToMax) {
    AutomaticGainControl::Config cfg;
    cfg.channels = 99; // above kMaxChannels
    cfg.sample_rate = 48000;
    AutomaticGainControl agc(cfg);
    EXPECT_EQ(agc.GetConfig().channels, AutomaticGainControl::kMaxChannels);
}

TEST(AutomaticGainControl, ConfigNonFiniteFieldsResetToDefaults) {
    AutomaticGainControl::Config cfg;
    cfg.target_db = std::nanf("");
    cfg.max_gain_db = std::nanf("");
    cfg.attack_ms = std::nanf("");
    cfg.release_ms = std::nanf("");
    cfg.noise_floor_db = std::nanf("");
    cfg.channels = 2;
    cfg.sample_rate = 48000;
    AutomaticGainControl agc(cfg);
    EXPECT_FLOAT_EQ(agc.GetConfig().target_db, -18.0f);
    EXPECT_FLOAT_EQ(agc.GetConfig().max_gain_db, 30.0f);
    EXPECT_FLOAT_EQ(agc.GetConfig().attack_ms, 50.0f);
    EXPECT_FLOAT_EQ(agc.GetConfig().release_ms, 400.0f);
    EXPECT_FLOAT_EQ(agc.GetConfig().noise_floor_db, -55.0f);
}

TEST(AutomaticGainControl, ConfigNegativeMaxGainMadeNonNegative) {
    AutomaticGainControl::Config cfg = StereoConfig(-18.0f);
    cfg.max_gain_db = -24.0f; // negative → made positive (magnitude)
    AutomaticGainControl agc(cfg);
    EXPECT_GE(agc.GetConfig().max_gain_db, 0.0f);
    EXPECT_FLOAT_EQ(agc.GetConfig().max_gain_db, 24.0f);
}

// ---------------------------------------------------------------------------
// Reset restores unity gain and clears the envelope.
// ---------------------------------------------------------------------------

TEST(AutomaticGainControl, ResetRestoresUnityGain) {
    AutomaticGainControl agc(StereoConfig(-18.0f));
    // Drive the gain up with a quiet sustained signal.
    auto buf = MakeConstant(96000, 2, DbToLin(-36.0f));
    agc.Process(buf.data(), 96000);
    ASSERT_GT(agc.CurrentGain(), 1.5f);

    agc.Reset();
    EXPECT_FLOAT_EQ(agc.CurrentGain(), 1.0f);

    // After Reset, a single below-floor sample stays at unity (frozen) and is
    // passed through essentially unchanged.
    const float quiet = DbToLin(-70.0f);
    std::vector<float> tiny = MakeConstant(1, 2, quiet);
    agc.Process(tiny.data(), 1);
    EXPECT_NEAR(tiny[0], quiet, 1e-6f);
    EXPECT_NEAR(agc.CurrentGain(), 1.0f, 1e-3f);
}
