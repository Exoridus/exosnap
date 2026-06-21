// Unit tests for OutputFormatAudioSrc (ADR 0030 — 0.6.0).
//
// Tests the resampling/channel-conversion decorator using a synthetic
// IAudioCaptureSource stub that delivers pre-filled Float32 frames.
// Links libswresample (FFmpeg) — not a pure-logic test.
//
// Covers:
//   - Passthrough mode (48k/stereo source, 48k/stereo target) — byte-identical.
//   - Stereo-to-mono downmix at 48 kHz (channel reduction, no rate change).
//   - Stereo-at-48k-to-stereo-at-44.1k (rate change only).
//   - SampleRate()/Channels() report target values after Init.
//   - SampleFormat() always returns Float32.
//   - Silent buffer propagates the silent flag.
//   - data_discontinuity propagates.

#include <gtest/gtest.h>

#include "output_format_audio_src.h"

#include <recorder_core/interfaces/IAudioCaptureSource.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using recorder_core::AudioSampleFormat;
using recorder_core::IAudioCaptureSource;
using recorder_core::OutputFormatAudioSrc;
using recorder_core::RawAudioBuffer;

// ---------------------------------------------------------------------------
// Stub source
// ---------------------------------------------------------------------------

struct StubSource final : IAudioCaptureSource {
    uint32_t sample_rate = 48000;
    uint32_t channels = 2;
    uint32_t frames = 480; // 10 ms at 48 kHz
    bool silent = false;
    bool data_discontinuity = false;
    std::vector<float> data;
    bool acquired = false;
    std::string endpoint = "stub";

    bool Init(std::string& /*out_error*/) override {
        // Fill with a simple ramp if no data is pre-loaded.
        if (data.empty()) {
            data.assign(static_cast<size_t>(frames) * channels, 0.25f);
        }
        return true;
    }
    uint32_t PendingFrameCount() override {
        return frames;
    }
    bool AcquireBuffer(RawAudioBuffer& out_buf, std::string& /*out_error*/) override {
        acquired = true;
        out_buf.bytes = reinterpret_cast<const uint8_t*>(data.data());
        out_buf.num_frames = frames;
        out_buf.silent = silent;
        out_buf.data_discontinuity = data_discontinuity;
        return true;
    }
    void ReleaseBuffer() override {
        acquired = false;
    }
    uint32_t SampleRate() const override {
        return sample_rate;
    }
    uint32_t Channels() const override {
        return channels;
    }
    AudioSampleFormat SampleFormat() const override {
        return AudioSampleFormat::Float32;
    }
    const std::string& EndpointName() const override {
        return endpoint;
    }
    void Shutdown() override {
    }
};

// ---------------------------------------------------------------------------
// Passthrough: target == source format
// ---------------------------------------------------------------------------

TEST(OutputFormatAudioSrc, Passthrough_SameRateChannels_ByteIdentical) {
    auto stub = std::make_unique<StubSource>();
    stub->sample_rate = 48000;
    stub->channels = 2;
    stub->frames = 480;
    // Fill with a known pattern.
    stub->data.resize(480 * 2);
    for (size_t i = 0; i < stub->data.size(); ++i) {
        stub->data[i] = static_cast<float>(i) / 1000.0f;
    }
    const float* expected = stub->data.data();

    OutputFormatAudioSrc src(std::move(stub), 48000, 2);
    std::string err;
    ASSERT_TRUE(src.Init(err)) << err;

    EXPECT_EQ(src.SampleRate(), 48000u);
    EXPECT_EQ(src.Channels(), 2u);
    EXPECT_EQ(src.SampleFormat(), AudioSampleFormat::Float32);

    RawAudioBuffer buf{};
    ASSERT_TRUE(src.AcquireBuffer(buf, err)) << err;
    EXPECT_EQ(buf.num_frames, 480u);
    EXPECT_FALSE(buf.silent);

    // In passthrough mode the bytes pointer is the original stub buffer.
    ASSERT_NE(buf.bytes, nullptr);
    const float* got = reinterpret_cast<const float*>(buf.bytes);
    for (size_t i = 0; i < 480 * 2; ++i) {
        EXPECT_FLOAT_EQ(got[i], expected[i]) << " at index " << i;
    }
    src.ReleaseBuffer();
}

// ---------------------------------------------------------------------------
// SampleRate/Channels after Init report target values
// ---------------------------------------------------------------------------

TEST(OutputFormatAudioSrc, SampleRate_And_Channels_ReportTarget) {
    auto stub = std::make_unique<StubSource>();
    stub->sample_rate = 48000;
    stub->channels = 2;

    OutputFormatAudioSrc src(std::move(stub), 44100, 1);
    std::string err;
    ASSERT_TRUE(src.Init(err)) << err;

    EXPECT_EQ(src.SampleRate(), 44100u);
    EXPECT_EQ(src.Channels(), 1u);
}

// ---------------------------------------------------------------------------
// Stereo→Mono downmix at 48 kHz (no rate change)
// ---------------------------------------------------------------------------

TEST(OutputFormatAudioSrc, StereoToMono_48k_ProducesMonoBuffer) {
    auto stub = std::make_unique<StubSource>();
    stub->sample_rate = 48000;
    stub->channels = 2;
    stub->frames = 480;
    stub->data.assign(480 * 2, 0.5f); // stereo constant signal

    OutputFormatAudioSrc src(std::move(stub), 48000, 1);
    std::string err;
    ASSERT_TRUE(src.Init(err)) << err;

    RawAudioBuffer buf{};
    ASSERT_TRUE(src.AcquireBuffer(buf, err)) << err;

    // Mono output: 480 frames * 1 channel.
    EXPECT_EQ(buf.num_frames, 480u);
    ASSERT_NE(buf.bytes, nullptr);

    const float* out = reinterpret_cast<const float*>(buf.bytes);
    // ITU downmix of stereo 0.5/0.5 should yield ~0.5 (stereo-to-mono average).
    for (size_t i = 0; i < buf.num_frames; ++i) {
        EXPECT_NEAR(out[i], 0.5f, 0.05f) << " at frame " << i;
    }
    src.ReleaseBuffer();
}

// ---------------------------------------------------------------------------
// Rate conversion: 48k stereo → 44.1k stereo
// ---------------------------------------------------------------------------

TEST(OutputFormatAudioSrc, RateConversion_48kTo44k_ProducesOutput) {
    auto stub = std::make_unique<StubSource>();
    stub->sample_rate = 48000;
    stub->channels = 2;
    stub->frames = 480; // 10 ms of input
    stub->data.assign(480 * 2, 0.25f);

    OutputFormatAudioSrc src(std::move(stub), 44100, 2);
    std::string err;
    ASSERT_TRUE(src.Init(err)) << err;

    RawAudioBuffer buf{};
    ASSERT_TRUE(src.AcquireBuffer(buf, err)) << err;

    // At 44100 Hz, 10 ms ≈ 441 frames; swresample may produce slightly fewer
    // (it buffers a few) but must produce > 0.
    EXPECT_GT(buf.num_frames, 0u);
    // Output frame count is approximately correct (±1 frame tolerance from swr
    // internal buffering).
    EXPECT_LE(buf.num_frames, 441u);
    src.ReleaseBuffer();
}

// ---------------------------------------------------------------------------
// Silent buffer propagates the silent flag
// ---------------------------------------------------------------------------

TEST(OutputFormatAudioSrc, SilentBuffer_PropagatesSilentFlag) {
    auto stub = std::make_unique<StubSource>();
    stub->sample_rate = 48000;
    stub->channels = 2;
    stub->frames = 480;
    stub->silent = true;

    // Passthrough mode: target == inner.
    OutputFormatAudioSrc src_pt(std::make_unique<StubSource>(*stub), 48000, 2);
    std::string err;
    ASSERT_TRUE(src_pt.Init(err)) << err;
    RawAudioBuffer buf_pt{};
    ASSERT_TRUE(src_pt.AcquireBuffer(buf_pt, err));
    EXPECT_TRUE(buf_pt.silent);
    src_pt.ReleaseBuffer();

    // Resampling mode: target != inner — should not crash and silent should be set.
    OutputFormatAudioSrc src_rs(std::make_unique<StubSource>(*stub), 44100, 2);
    ASSERT_TRUE(src_rs.Init(err)) << err;
    RawAudioBuffer buf_rs{};
    ASSERT_TRUE(src_rs.AcquireBuffer(buf_rs, err));
    // silent is set if inner was silent
    EXPECT_TRUE(buf_rs.silent);
    src_rs.ReleaseBuffer();
}

// ---------------------------------------------------------------------------
// data_discontinuity propagates
// ---------------------------------------------------------------------------

TEST(OutputFormatAudioSrc, DataDiscontinuity_Propagates) {
    auto stub = std::make_unique<StubSource>();
    stub->sample_rate = 48000;
    stub->channels = 2;
    stub->frames = 480;
    stub->data_discontinuity = true;

    OutputFormatAudioSrc src(std::move(stub), 48000, 2);
    std::string err;
    ASSERT_TRUE(src.Init(err)) << err;
    RawAudioBuffer buf{};
    ASSERT_TRUE(src.AcquireBuffer(buf, err));
    EXPECT_TRUE(buf.data_discontinuity);
    src.ReleaseBuffer();
}

// ---------------------------------------------------------------------------
// SampleFormat always Float32
// ---------------------------------------------------------------------------

TEST(OutputFormatAudioSrc, SampleFormat_IsFloat32) {
    auto stub = std::make_unique<StubSource>();
    OutputFormatAudioSrc src(std::move(stub), 48000, 2);
    std::string err;
    ASSERT_TRUE(src.Init(err)) << err;
    EXPECT_EQ(src.SampleFormat(), AudioSampleFormat::Float32);
}

// ---------------------------------------------------------------------------
// Null inner → Init fails cleanly
// ---------------------------------------------------------------------------

TEST(OutputFormatAudioSrc, NullInner_InitFails) {
    OutputFormatAudioSrc src(nullptr, 48000, 2);
    std::string err;
    EXPECT_FALSE(src.Init(err));
    EXPECT_FALSE(err.empty());
}

} // namespace
