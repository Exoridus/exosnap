#include <gtest/gtest.h>

#include "high_pass_filter.h"

#include <cmath>
#include <vector>

using recorder_core::HighPassFilter;

namespace {

constexpr float kPi = 3.14159265358979323846f;

HighPassFilter::Config StereoConfig(float cutoff_hz) {
    HighPassFilter::Config cfg;
    cfg.cutoff_hz = cutoff_hz;
    cfg.sample_rate = 48000;
    cfg.channels = 2;
    return cfg;
}

// Generate a mono sine and return its peak amplitude after running through the
// filter (steady state — discard the first half to skip the transient).
float SteadyStatePeak(HighPassFilter& hpf, float freq_hz, uint32_t sample_rate, uint32_t channels, float amplitude,
                      uint32_t frames) {
    std::vector<float> buf(static_cast<std::size_t>(frames) * channels, 0.0f);
    const float w = 2.0f * kPi * freq_hz / static_cast<float>(sample_rate);
    for (uint32_t f = 0; f < frames; ++f) {
        const float s = amplitude * std::sin(w * static_cast<float>(f));
        for (uint32_t c = 0; c < channels; ++c) {
            buf[static_cast<std::size_t>(f) * channels + c] = s;
        }
    }
    hpf.Process(buf.data(), frames);

    float peak = 0.0f;
    const uint32_t start = frames / 2;
    for (uint32_t f = start; f < frames; ++f) {
        for (uint32_t c = 0; c < channels; ++c) {
            peak = std::max(peak, std::fabs(buf[static_cast<std::size_t>(f) * channels + c]));
        }
    }
    return peak;
}

bool HasNonFinite(const std::vector<float>& buf) {
    for (float s : buf) {
        if (!std::isfinite(s)) {
            return true;
        }
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Low-frequency attenuation
// ---------------------------------------------------------------------------

TEST(HighPassFilter, DcIsStronglyAttenuated) {
    HighPassFilter hpf(StereoConfig(80.0f));
    // A pure DC offset must decay essentially to zero in steady state.
    std::vector<float> buf(2000u * 2u, 0.8f);
    hpf.Process(buf.data(), 2000);
    // Inspect the tail — the DC component is removed.
    float tail = 0.0f;
    for (std::size_t i = (buf.size() - 200); i < buf.size(); ++i) {
        tail = std::max(tail, std::fabs(buf[i]));
    }
    EXPECT_LT(tail, 1e-3f);
}

TEST(HighPassFilter, VeryLowFrequencyIsAttenuated) {
    HighPassFilter hpf(StereoConfig(80.0f));
    // 10 Hz is well below the 80 Hz cutoff: should be attenuated by a lot.
    const float peak = SteadyStatePeak(hpf, 10.0f, 48000, 2, 1.0f, 24000);
    EXPECT_LT(peak, 0.2f);
}

TEST(HighPassFilter, AttenuationAtCutoffIsAboutMinus3dB) {
    HighPassFilter hpf(StereoConfig(80.0f));
    // At the cutoff a Butterworth high-pass is ~−3 dB (≈0.707 of input).
    const float peak = SteadyStatePeak(hpf, 80.0f, 48000, 2, 1.0f, 48000);
    EXPECT_NEAR(peak, 0.707f, 0.05f);
}

// ---------------------------------------------------------------------------
// Passband — high frequencies pass near unity
// ---------------------------------------------------------------------------

TEST(HighPassFilter, QuarterNyquistPassesNearUnity) {
    HighPassFilter hpf(StereoConfig(80.0f));
    // 12 kHz (quarter Nyquist at 48 kHz) is deep in the passband.
    const float peak = SteadyStatePeak(hpf, 12000.0f, 48000, 2, 1.0f, 24000);
    EXPECT_NEAR(peak, 1.0f, 0.02f);
}

// ---------------------------------------------------------------------------
// Stability — no NaN/blowup over many samples
// ---------------------------------------------------------------------------

TEST(HighPassFilter, StableOverManySamples) {
    HighPassFilter hpf(StereoConfig(80.0f));
    std::vector<float> buf(100000u * 2u, 0.0f);
    const float w = 2.0f * kPi * 200.0f / 48000.0f;
    for (uint32_t f = 0; f < 100000; ++f) {
        const float s = 0.9f * std::sin(w * static_cast<float>(f));
        buf[static_cast<std::size_t>(f) * 2 + 0] = s;
        buf[static_cast<std::size_t>(f) * 2 + 1] = s;
    }
    hpf.Process(buf.data(), 100000);
    EXPECT_FALSE(HasNonFinite(buf));
    // The passband signal stays bounded near its input amplitude.
    float peak = 0.0f;
    for (std::size_t i = buf.size() / 2; i < buf.size(); ++i) {
        peak = std::max(peak, std::fabs(buf[i]));
    }
    EXPECT_LT(peak, 1.5f);
}

// ---------------------------------------------------------------------------
// Config sanitization
// ---------------------------------------------------------------------------

TEST(HighPassFilter, ConfigSanitizesDegenerateInputs) {
    HighPassFilter::Config cfg;
    cfg.cutoff_hz = 0.0f; // invalid → 80
    cfg.channels = 0;     // invalid → 1
    cfg.sample_rate = 0;  // invalid → 48000
    HighPassFilter hpf(cfg);
    EXPECT_FLOAT_EQ(hpf.GetConfig().cutoff_hz, 80.0f);
    EXPECT_EQ(hpf.GetConfig().channels, 1u);
    EXPECT_EQ(hpf.GetConfig().sample_rate, 48000u);
}

TEST(HighPassFilter, ConfigClampsCutoffBelowNyquist) {
    HighPassFilter::Config cfg;
    cfg.cutoff_hz = 100000.0f; // above Nyquist (24 kHz) → clamped
    cfg.channels = 2;
    cfg.sample_rate = 48000;
    HighPassFilter hpf(cfg);
    EXPECT_LT(hpf.GetConfig().cutoff_hz, 24000.0f);
    EXPECT_GT(hpf.GetConfig().cutoff_hz, 0.0f);
}

TEST(HighPassFilter, ConfigClampsChannelsToMax) {
    HighPassFilter::Config cfg;
    cfg.cutoff_hz = 80.0f;
    cfg.channels = 99; // above kMaxChannels
    cfg.sample_rate = 48000;
    HighPassFilter hpf(cfg);
    EXPECT_EQ(hpf.GetConfig().channels, HighPassFilter::kMaxChannels);
}

TEST(HighPassFilter, ConfigNonFiniteCutoffResetsToDefault) {
    HighPassFilter::Config cfg;
    cfg.cutoff_hz = std::nanf("");
    cfg.channels = 2;
    cfg.sample_rate = 48000;
    HighPassFilter hpf(cfg);
    EXPECT_FLOAT_EQ(hpf.GetConfig().cutoff_hz, 80.0f);
}

// ---------------------------------------------------------------------------
// Per-channel independence
// ---------------------------------------------------------------------------

TEST(HighPassFilter, PerChannelStateIsIndependent) {
    HighPassFilter hpf(StereoConfig(80.0f));
    // Left = high-frequency passband sine, right = DC. The two channels must not
    // bleed into each other: the right channel decays to ~0, the left survives.
    const uint32_t frames = 24000;
    std::vector<float> buf(static_cast<std::size_t>(frames) * 2u, 0.0f);
    const float w = 2.0f * kPi * 12000.0f / 48000.0f;
    for (uint32_t f = 0; f < frames; ++f) {
        buf[static_cast<std::size_t>(f) * 2 + 0] = std::sin(w * static_cast<float>(f)); // L: passband
        buf[static_cast<std::size_t>(f) * 2 + 1] = 0.7f;                                // R: DC
    }
    hpf.Process(buf.data(), frames);

    float left_peak = 0.0f;
    float right_peak = 0.0f;
    for (uint32_t f = frames / 2; f < frames; ++f) {
        left_peak = std::max(left_peak, std::fabs(buf[static_cast<std::size_t>(f) * 2 + 0]));
        right_peak = std::max(right_peak, std::fabs(buf[static_cast<std::size_t>(f) * 2 + 1]));
    }
    EXPECT_NEAR(left_peak, 1.0f, 0.05f);
    EXPECT_LT(right_peak, 1e-3f);
}

// ---------------------------------------------------------------------------
// Reset clears state
// ---------------------------------------------------------------------------

TEST(HighPassFilter, ResetClearsState) {
    HighPassFilter hpf(StereoConfig(80.0f));
    // Prime the state with a DC block.
    std::vector<float> prime(1000u * 2u, 0.9f);
    hpf.Process(prime.data(), 1000);

    // Reset, then process a single impulse. With cleared state, the first output
    // sample equals b0 * x exactly (no contribution from stale registers).
    hpf.Reset();
    std::vector<float> imp(2u, 0.0f);
    imp[0] = 1.0f;
    imp[1] = 1.0f;
    hpf.Process(imp.data(), 1);
    // After Reset the first sample is purely b0 * 1.0; b0 is in (0, 1] for an HPF.
    EXPECT_GT(imp[0], 0.0f);
    EXPECT_LE(imp[0], 1.0f + 1e-6f);
    EXPECT_FLOAT_EQ(imp[0], imp[1]);
}
