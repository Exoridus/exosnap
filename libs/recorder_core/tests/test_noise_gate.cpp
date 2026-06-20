#include <gtest/gtest.h>

#include "noise_gate.h"

#include <cmath>
#include <vector>

using recorder_core::NoiseGate;

namespace {

NoiseGate::Config StereoConfig(float threshold_db) {
    NoiseGate::Config cfg;
    cfg.threshold_db = threshold_db;
    cfg.attack_ms = 2.0f;
    cfg.hold_ms = 120.0f;
    cfg.release_ms = 150.0f;
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

} // namespace

// ---------------------------------------------------------------------------
// Loud signal passes near unity after attack
// ---------------------------------------------------------------------------

TEST(NoiseGate, LoudSignalPassesNearUnity) {
    NoiseGate gate(StereoConfig(-45.0f));
    // 0.5 == -6 dBFS, well above the -45 dB threshold → gate opens.
    auto buf = MakeConstant(4800, 2, 0.5f);
    gate.Process(buf.data(), 4800);

    // After the short attack, the tail is essentially unchanged (gain ≈ 1.0).
    const float tail = TailPeak(buf, 2, 200);
    EXPECT_NEAR(tail, 0.5f, 0.01f);
    EXPECT_NEAR(gate.CurrentGain(), 1.0f, 1e-3f);
}

// ---------------------------------------------------------------------------
// Quiet signal is attenuated toward zero after release
// ---------------------------------------------------------------------------

TEST(NoiseGate, QuietSignalIsAttenuated) {
    NoiseGate gate(StereoConfig(-45.0f));
    // 0.001 ≈ -60 dBFS, below the -45 dB threshold → gate stays closed.
    auto buf = MakeConstant(48000, 2, 0.001f);
    gate.Process(buf.data(), 48000);

    // The gate never opens (hold timer is never armed) and the gain decays to ~0.
    const float tail = TailPeak(buf, 2, 200);
    EXPECT_LT(tail, 1e-4f);
    EXPECT_LT(gate.CurrentGain(), 1e-2f);
}

// ---------------------------------------------------------------------------
// Hold keeps the gate open briefly after a peak
// ---------------------------------------------------------------------------

TEST(NoiseGate, HoldKeepsGateOpenAfterPeak) {
    NoiseGate gate(StereoConfig(-45.0f));
    const uint32_t channels = 2;

    // 480 frames (10 ms) loud, then 2400 frames (50 ms) silence. With a 120 ms
    // hold the gate must still be substantially open right after the loud burst.
    std::vector<float> loud = MakeConstant(480, channels, 0.5f);
    gate.Process(loud.data(), 480);
    ASSERT_NEAR(gate.CurrentGain(), 1.0f, 1e-2f);

    std::vector<float> silence = MakeConstant(2400, channels, 0.0f);
    gate.Process(silence.data(), 2400);
    // Within the hold window the gain has not collapsed to zero.
    EXPECT_GT(gate.CurrentGain(), 0.5f);

    // Long past the hold + release window the gate has fully closed.
    std::vector<float> tail = MakeConstant(48000, channels, 0.0f);
    gate.Process(tail.data(), 48000);
    EXPECT_LT(gate.CurrentGain(), 1e-2f);
}

// ---------------------------------------------------------------------------
// Stereo-linked: a loud L with quiet R opens both channels equally
// ---------------------------------------------------------------------------

TEST(NoiseGate, StereoLinkedPreservesImage) {
    NoiseGate gate(StereoConfig(-45.0f));
    const uint32_t frames = 4800;
    std::vector<float> buf(static_cast<std::size_t>(frames) * 2u, 0.0f);
    // Left loud (0.5), right quiet (0.01). Detection uses the loudest channel,
    // so the gate opens and one shared gain is applied to both channels — the
    // L/R ratio is preserved (no per-channel gating).
    for (uint32_t f = 0; f < frames; ++f) {
        buf[static_cast<std::size_t>(f) * 2 + 0] = 0.5f;
        buf[static_cast<std::size_t>(f) * 2 + 1] = 0.01f;
    }
    gate.Process(buf.data(), frames);

    // Inspect a steady-state frame in the tail.
    const std::size_t idx = static_cast<std::size_t>(frames - 1) * 2;
    const float l = buf[idx + 0];
    const float r = buf[idx + 1];
    EXPECT_NEAR(l, 0.5f, 0.01f);
    EXPECT_NEAR(r, 0.01f, 0.01f);
    // Ratio preserved (shared gain): r / l ≈ 0.02.
    ASSERT_GT(l, 1e-6f);
    EXPECT_NEAR(r / l, 0.02f, 5e-3f);
}

// ---------------------------------------------------------------------------
// Config sanitization
// ---------------------------------------------------------------------------

TEST(NoiseGate, ConfigSanitizesDegenerateInputs) {
    NoiseGate::Config cfg;
    cfg.threshold_db = -45.0f;
    cfg.channels = 0;    // invalid → 1
    cfg.sample_rate = 0; // invalid → 48000
    NoiseGate gate(cfg);
    EXPECT_EQ(gate.GetConfig().channels, 1u);
    EXPECT_EQ(gate.GetConfig().sample_rate, 48000u);
}

TEST(NoiseGate, ConfigClampsChannelsToMax) {
    NoiseGate::Config cfg;
    cfg.threshold_db = -45.0f;
    cfg.channels = 99; // above kMaxChannels
    cfg.sample_rate = 48000;
    NoiseGate gate(cfg);
    EXPECT_EQ(gate.GetConfig().channels, NoiseGate::kMaxChannels);
}

TEST(NoiseGate, ConfigNonFiniteThresholdResetsToDefault) {
    NoiseGate::Config cfg;
    cfg.threshold_db = std::nanf("");
    cfg.channels = 2;
    cfg.sample_rate = 48000;
    NoiseGate gate(cfg);
    EXPECT_FLOAT_EQ(gate.GetConfig().threshold_db, -45.0f);
}

// ---------------------------------------------------------------------------
// Reset clears gain + hold state
// ---------------------------------------------------------------------------

TEST(NoiseGate, ResetClearsState) {
    NoiseGate gate(StereoConfig(-45.0f));
    // Open the gate with a loud block.
    auto loud = MakeConstant(4800, 2, 0.5f);
    gate.Process(loud.data(), 4800);
    ASSERT_GT(gate.CurrentGain(), 0.5f);

    gate.Reset();
    EXPECT_FLOAT_EQ(gate.CurrentGain(), 0.0f);

    // After Reset a single quiet sample stays closed (gain ≈ 0, output ≈ 0).
    std::vector<float> quiet = MakeConstant(1, 2, 0.001f);
    gate.Process(quiet.data(), 1);
    EXPECT_LT(std::fabs(quiet[0]), 1e-4f);
}
