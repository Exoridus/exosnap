#include <gtest/gtest.h>

#include "rnnoise_denoiser.h"

#include <cmath>
#include <vector>

using recorder_core::RnnoiseDenoiser;

namespace {

RnnoiseDenoiser::Config MakeConfig(uint32_t sample_rate, uint32_t channels) {
    RnnoiseDenoiser::Config cfg;
    cfg.sample_rate = sample_rate;
    cfg.channels = channels;
    return cfg;
}

// Interleaved sine across all channels (same phase per channel — content does
// not matter for plumbing, only that it is non-trivial and bounded in [-1, 1]).
std::vector<float> MakeSine(uint32_t frames, uint32_t channels, float amp, float cycles_per_buffer) {
    std::vector<float> buf(static_cast<std::size_t>(frames) * channels, 0.0f);
    for (uint32_t f = 0; f < frames; ++f) {
        const float t = static_cast<float>(f) / static_cast<float>(frames);
        const float s = amp * std::sin(2.0f * 3.14159265358979323846f * cycles_per_buffer * t);
        for (uint32_t c = 0; c < channels; ++c) {
            buf[static_cast<std::size_t>(f) * channels + c] = s;
        }
    }
    return buf;
}

bool AllFinite(const std::vector<float>& buf) {
    for (float v : buf) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    return true;
}

float MaxAbs(const std::vector<float>& buf) {
    float m = 0.0f;
    for (float v : buf) {
        m = std::max(m, std::fabs(v));
    }
    return m;
}

constexpr uint32_t kFrame = RnnoiseDenoiser::kFrameSize; // 480

} // namespace

// ---------------------------------------------------------------------------
// Active at 48 kHz, passthrough otherwise.
// ---------------------------------------------------------------------------

TEST(RnnoiseDenoiser, ActiveAt48kOnly) {
    RnnoiseDenoiser active(MakeConfig(48000, 1));
    EXPECT_TRUE(active.IsActive());

    RnnoiseDenoiser other(MakeConfig(44100, 1));
    EXPECT_FALSE(other.IsActive());
}

// At a non-48k rate the stage must not alter the buffer at all.
TEST(RnnoiseDenoiser, PassthroughAtNon48k) {
    RnnoiseDenoiser den(MakeConfig(44100, 2));
    ASSERT_FALSE(den.IsActive());

    auto buf = MakeSine(kFrame, 2, 0.5f, 8.0f);
    const auto original = buf;
    den.Process(buf.data(), kFrame);
    EXPECT_EQ(buf, original); // untouched
}

// ---------------------------------------------------------------------------
// Output is finite, bounded, and the buffer keeps its length (mono, one block).
// ---------------------------------------------------------------------------

TEST(RnnoiseDenoiser, MonoSingleBlockFiniteBounded) {
    RnnoiseDenoiser den(MakeConfig(48000, 1));
    ASSERT_TRUE(den.IsActive());

    auto buf = MakeSine(kFrame, 1, 0.5f, 12.0f);
    const std::size_t len = buf.size();
    den.Process(buf.data(), kFrame);

    EXPECT_EQ(buf.size(), len); // in-place; length unchanged
    EXPECT_TRUE(AllFinite(buf));
    EXPECT_LE(MaxAbs(buf), 4.0f); // generously bounded — must not explode
}

// The first block is priming silence (the inherent one-block latency).
TEST(RnnoiseDenoiser, FirstBlockIsPrimingSilence) {
    RnnoiseDenoiser den(MakeConfig(48000, 1));
    auto buf = MakeSine(kFrame, 1, 0.5f, 12.0f);
    den.Process(buf.data(), kFrame);
    EXPECT_FLOAT_EQ(MaxAbs(buf), 0.0f); // entirely zeros after the first 480-block
}

// After priming, a later block carries non-silent denoised content for a
// non-silent input (sanity that the denoised stream actually flows through).
TEST(RnnoiseDenoiser, EmitsContentAfterPriming) {
    RnnoiseDenoiser den(MakeConfig(48000, 1));
    // Process several full blocks; collect the last block's output.
    float last_peak = 0.0f;
    for (int i = 0; i < 6; ++i) {
        auto buf = MakeSine(kFrame, 1, 0.6f, 16.0f);
        den.Process(buf.data(), kFrame);
        EXPECT_TRUE(AllFinite(buf));
        last_peak = MaxAbs(buf);
    }
    EXPECT_GT(last_peak, 0.0f); // denoised content present after priming
}

// ---------------------------------------------------------------------------
// Odd (non-480-multiple) block sizes: length preserved, finite, bounded across
// many calls (exercises the accumulator + FIFO carry-over).
// ---------------------------------------------------------------------------

TEST(RnnoiseDenoiser, OddBlockSizesCarryOver) {
    RnnoiseDenoiser den(MakeConfig(48000, 2));
    ASSERT_TRUE(den.IsActive());

    const uint32_t block = 200; // not a multiple of 480
    for (int i = 0; i < 20; ++i) {
        auto buf = MakeSine(block, 2, 0.4f, 5.0f);
        const std::size_t len = buf.size();
        den.Process(buf.data(), block);
        EXPECT_EQ(buf.size(), len);
        EXPECT_TRUE(AllFinite(buf));
        EXPECT_LE(MaxAbs(buf), 4.0f);
    }
}

// ---------------------------------------------------------------------------
// Multi-channel: independent per-channel states, no crash, finite, length kept.
// ---------------------------------------------------------------------------

TEST(RnnoiseDenoiser, MultiChannelNoCrash) {
    RnnoiseDenoiser den(MakeConfig(48000, 4));
    ASSERT_TRUE(den.IsActive());

    for (int i = 0; i < 4; ++i) {
        auto buf = MakeSine(kFrame, 4, 0.5f, 10.0f);
        const std::size_t len = buf.size();
        den.Process(buf.data(), kFrame);
        EXPECT_EQ(buf.size(), len);
        EXPECT_TRUE(AllFinite(buf));
    }
}

// Channel count is clamped to [1, kMaxChannels].
TEST(RnnoiseDenoiser, ClampsChannelCount) {
    RnnoiseDenoiser zero(MakeConfig(48000, 0));
    EXPECT_EQ(zero.GetConfig().channels, 1u);

    RnnoiseDenoiser huge(MakeConfig(48000, 999));
    EXPECT_EQ(huge.GetConfig().channels, RnnoiseDenoiser::kMaxChannels);
}

// ---------------------------------------------------------------------------
// Reset re-primes the latency: after Reset the next block is silence again.
// ---------------------------------------------------------------------------

TEST(RnnoiseDenoiser, ResetRePrimes) {
    RnnoiseDenoiser den(MakeConfig(48000, 1));

    // Prime past the first block.
    for (int i = 0; i < 4; ++i) {
        auto buf = MakeSine(kFrame, 1, 0.6f, 16.0f);
        den.Process(buf.data(), kFrame);
    }

    den.Reset();

    auto buf = MakeSine(kFrame, 1, 0.6f, 16.0f);
    den.Process(buf.data(), kFrame);
    EXPECT_FLOAT_EQ(MaxAbs(buf), 0.0f); // first block after Reset is priming silence again
}

// ---------------------------------------------------------------------------
// Null / empty inputs are safe no-ops.
// ---------------------------------------------------------------------------

TEST(RnnoiseDenoiser, NullAndEmptyAreSafe) {
    RnnoiseDenoiser den(MakeConfig(48000, 1));
    den.Process(nullptr, kFrame); // must not crash
    std::vector<float> buf(kFrame, 0.1f);
    den.Process(buf.data(), 0); // zero frames: no-op
    EXPECT_FLOAT_EQ(buf[0], 0.1f);
}
