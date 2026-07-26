// Unit tests for OutputFormatAudioSrc (ADR 0030 — 0.6.0).
//
// Tests the resampling/channel-conversion decorator using a synthetic
// IAudioCaptureSource stub that delivers pre-filled Float32 frames.
// Links libswresample (FFmpeg) — not a pure-logic test.
//
// Covers:
//   - Passthrough mode (48k/stereo source, 48k/stereo target) — byte-identical
//     for Float32 inners; Int16 inners are converted to real Float32 samples.
//   - Int16 inner through the resampling path (swr input format is S16).
//   - Stereo-to-mono downmix at 48 kHz (channel reduction, no rate change).
//   - Stereo-at-48k-to-stereo-at-44.1k (rate change only).
//   - SampleRate()/Channels() report target values after Init.
//   - SampleFormat() always returns Float32.
//   - Silent buffer propagates the silent flag (Float32 and Int16 inners).
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

// A source that reports Int16 samples (like WasapiProcessLoopbackSrc, and mic
// capture on Int16-only endpoints). Its bytes are little-endian interleaved
// int16 frames; SampleFormat() reports Int16.
struct Int16StubSource final : IAudioCaptureSource {
    uint32_t sample_rate = 48000;
    uint32_t channels = 2;
    uint32_t frames = 480;
    bool silent = false;
    bool data_discontinuity = false;
    std::vector<int16_t> data;
    std::string endpoint = "int16stub";

    bool Init(std::string& /*out_error*/) override {
        if (data.empty()) {
            data.assign(static_cast<size_t>(frames) * channels, 0);
        }
        return true;
    }
    uint32_t PendingFrameCount() override {
        return frames;
    }
    bool AcquireBuffer(RawAudioBuffer& out_buf, std::string& /*out_error*/) override {
        out_buf.bytes = reinterpret_cast<const uint8_t*>(data.data());
        out_buf.num_frames = frames;
        out_buf.silent = silent;
        out_buf.data_discontinuity = data_discontinuity;
        return true;
    }
    void ReleaseBuffer() override {
    }
    uint32_t SampleRate() const override {
        return sample_rate;
    }
    uint32_t Channels() const override {
        return channels;
    }
    AudioSampleFormat SampleFormat() const override {
        return AudioSampleFormat::Int16;
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
// Int16 inner source, passthrough rate/channels: the decorator claims Float32
// via SampleFormat(), so it MUST actually convert the Int16 samples to Float32
// rather than hand the raw int16 bytes through. Otherwise the encoder reads
// int16 bytes as float garbage (silent audio corruption).
// ---------------------------------------------------------------------------

TEST(OutputFormatAudioSrc, Passthrough_Int16Source_ConvertsToFloat32) {
    auto stub = std::make_unique<Int16StubSource>();
    stub->sample_rate = 48000;
    stub->channels = 2;
    stub->frames = 480;
    stub->data.resize(480 * 2);
    // A deterministic bipolar ramp spanning most of the int16 range.
    for (size_t i = 0; i < stub->data.size(); ++i) {
        stub->data[i] = static_cast<int16_t>(((static_cast<int>(i) * 37) % 65536) - 32768);
    }
    std::vector<int16_t> expected_i16 = stub->data;

    OutputFormatAudioSrc src(std::move(stub), 48000, 2);
    std::string err;
    ASSERT_TRUE(src.Init(err)) << err;

    // Contract: the decorator advertises Float32 to the encoder.
    EXPECT_EQ(src.SampleFormat(), AudioSampleFormat::Float32);

    RawAudioBuffer buf{};
    ASSERT_TRUE(src.AcquireBuffer(buf, err)) << err;
    EXPECT_EQ(buf.num_frames, 480u);
    ASSERT_NE(buf.bytes, nullptr);

    // The exposed bytes must be real Float32 samples equal to int16 / 32768.
    const float* got = reinterpret_cast<const float*>(buf.bytes);
    for (size_t i = 0; i < expected_i16.size(); ++i) {
        const float want = static_cast<float>(expected_i16[i]) / 32768.0f;
        EXPECT_NEAR(got[i], want, 1e-6f) << " at index " << i;
    }
    src.ReleaseBuffer();
}

// ---------------------------------------------------------------------------
// Int16 inner source through the resampling (swr) path: the resampler input
// format must be S16, not FLT. A constant 0.5 signal must survive resampling.
// ---------------------------------------------------------------------------

TEST(OutputFormatAudioSrc, RateConversion_Int16Source_ProducesConvertedFloat) {
    auto stub = std::make_unique<Int16StubSource>();
    stub->sample_rate = 48000;
    stub->channels = 2;
    stub->frames = 480;
    // Constant +0.5 full-scale (16384 / 32768 == 0.5).
    stub->data.assign(480 * 2, static_cast<int16_t>(16384));

    OutputFormatAudioSrc src(std::move(stub), 44100, 2);
    std::string err;
    ASSERT_TRUE(src.Init(err)) << err;

    RawAudioBuffer buf{};
    ASSERT_TRUE(src.AcquireBuffer(buf, err)) << err;
    EXPECT_GT(buf.num_frames, 0u);
    ASSERT_NE(buf.bytes, nullptr);

    const float* out = reinterpret_cast<const float*>(buf.bytes);
    const size_t out_samples = static_cast<size_t>(buf.num_frames) * 2u;
    // A constant input resamples to a constant ~0.5 output (swr edge frames may
    // ramp; check the settled interior). If the input were misread as FLT the
    // values would be denormal/garbage far from 0.5.
    for (size_t i = 4 * 2; i < out_samples; ++i) {
        EXPECT_NEAR(out[i], 0.5f, 0.02f) << " at sample " << i;
    }
    src.ReleaseBuffer();
}

// ---------------------------------------------------------------------------
// Int16 inner source, silent buffer: still propagates the silent flag through
// the converting passthrough without dereferencing bytes.
// ---------------------------------------------------------------------------

TEST(OutputFormatAudioSrc, Passthrough_Int16Source_SilentPropagates) {
    auto stub = std::make_unique<Int16StubSource>();
    stub->sample_rate = 48000;
    stub->channels = 2;
    stub->frames = 480;
    stub->silent = true;

    OutputFormatAudioSrc src(std::move(stub), 48000, 2);
    std::string err;
    ASSERT_TRUE(src.Init(err)) << err;
    RawAudioBuffer buf{};
    ASSERT_TRUE(src.AcquireBuffer(buf, err)) << err;
    EXPECT_TRUE(buf.silent);
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

// ---------------------------------------------------------------------------
// A/V clock slaving (H-3): swr compensation sign contract + frame accounting.
// ---------------------------------------------------------------------------

namespace {
struct SlaveResult {
    int64_t total_out_frames = 0;
    double applied_ms = 0.0;
};

// Drive a 48 k/stereo decorator (default passthrough) at a fixed compensation
// ppm across `packets` buffers of `frames` Float32 stereo frames, returning the
// total output frame count and the final AppliedCompensationMs.
SlaveResult DriveCompensated(double ppm, int packets, uint32_t frames = 480) {
    auto stub = std::make_unique<StubSource>();
    stub->sample_rate = 48000;
    stub->channels = 2;
    stub->frames = frames;
    stub->data.assign(static_cast<size_t>(frames) * 2u, 0.1f);

    OutputFormatAudioSrc src(std::move(stub), 48000, 2);
    std::string err;
    EXPECT_TRUE(src.Init(err)) << err;
    if (ppm != 0.0) {
        src.SetCompensationPpm(ppm);
    }
    SlaveResult r;
    for (int i = 0; i < packets; ++i) {
        RawAudioBuffer buf{};
        if (!src.AcquireBuffer(buf, err)) {
            break;
        }
        r.total_out_frames += buf.num_frames;
        src.ReleaseBuffer();
    }
    r.applied_ms = src.AppliedCompensationMs();
    return r;
}
} // namespace

TEST(OutputFormatAudioSrc, ClockSlaving_SignContract_PositivePpmProducesMoreOutput) {
    // The load-bearing cross-layer check: a positive ppm must yield MORE output
    // frames than input, a negative ppm fewer. A sign inversion here would double
    // the drift instead of correcting it.
    constexpr int kPackets = 2000;
    constexpr int64_t kInputFrames = static_cast<int64_t>(kPackets) * 480; // 960000

    const SlaveResult pos = DriveCompensated(+500.0, kPackets);
    const SlaveResult neg = DriveCompensated(-500.0, kPackets);

    // 500 ppm over 960k frames = ~480 frames of compensation, far above the swr
    // filter hold (tens of frames), so the direction is unambiguous.
    EXPECT_GT(pos.total_out_frames, kInputFrames) << "+500 ppm must stretch (more output)";
    EXPECT_LT(neg.total_out_frames, kInputFrames) << "-500 ppm must compress (less output)";
    EXPECT_GT(pos.total_out_frames - neg.total_out_frames, 700)
        << "the +/-500 ppm spread must be ~2x the per-side compensation";
}

TEST(OutputFormatAudioSrc, ClockSlaving_AppliedCompensationMs_SignAndMagnitude) {
    const SlaveResult pos = DriveCompensated(+500.0, 2000);
    const SlaveResult neg = DriveCompensated(-500.0, 2000);
    // +500 ppm over 20 s (~960k frames / 48 kHz) stretches ~10 ms; -500 compresses.
    EXPECT_GT(pos.applied_ms, 0.0);
    EXPECT_LT(neg.applied_ms, 0.0);
    EXPECT_NEAR(pos.applied_ms, 10.0, 3.0);
    EXPECT_NEAR(neg.applied_ms, -10.0, 3.0);
}

TEST(OutputFormatAudioSrc, ClockSlaving_ZeroPpm_StaysByteIdenticalPassthrough) {
    // A session that never engages compensation must remain the byte-identical
    // passthrough (no swr, A == 0) — the bit-exact guarantee for PCM/FLAC.
    auto stub = std::make_unique<StubSource>();
    stub->data.resize(480 * 2);
    for (size_t i = 0; i < stub->data.size(); ++i) {
        stub->data[i] = static_cast<float>(i) / 1000.0f;
    }
    const std::vector<float> expected = stub->data;

    OutputFormatAudioSrc src(std::move(stub), 48000, 2);
    std::string err;
    ASSERT_TRUE(src.Init(err)) << err;
    EXPECT_DOUBLE_EQ(src.AppliedCompensationMs(), 0.0);

    RawAudioBuffer buf{};
    ASSERT_TRUE(src.AcquireBuffer(buf, err)) << err;
    ASSERT_NE(buf.bytes, nullptr);
    const float* got = reinterpret_cast<const float*>(buf.bytes);
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_FLOAT_EQ(got[i], expected[i]) << " at index " << i;
    }
    src.ReleaseBuffer();
    EXPECT_DOUBLE_EQ(src.AppliedCompensationMs(), 0.0);
}

TEST(OutputFormatAudioSrc, ClockSlaving_EngageTransition_PreservesFlags) {
    // Engaging compensation lazily must not lose silent / data_discontinuity /
    // gap_frames on the buffers that follow.
    auto stub = std::make_unique<StubSource>();
    stub->sample_rate = 48000;
    stub->channels = 2;
    stub->frames = 480;
    stub->data.assign(480 * 2, 0.2f);
    stub->data_discontinuity = true;

    OutputFormatAudioSrc src(std::move(stub), 48000, 2);
    std::string err;
    ASSERT_TRUE(src.Init(err)) << err;
    src.SetCompensationPpm(200.0); // leave passthrough

    RawAudioBuffer buf{};
    ASSERT_TRUE(src.AcquireBuffer(buf, err)) << err;
    EXPECT_TRUE(buf.data_discontinuity);
    EXPECT_GT(buf.num_frames, 0u);
    src.ReleaseBuffer();
}

TEST(OutputFormatAudioSrc, ClockSlaving_ZeroAfterEngage_KeepsContextCancelsDelta) {
    // SetCompensationPpm(0) after engage must keep the (now resampling) context
    // and simply stop stretching — not crash or revert to passthrough.
    auto stub = std::make_unique<StubSource>();
    stub->sample_rate = 48000;
    stub->channels = 2;
    stub->frames = 480;
    stub->data.assign(480 * 2, 0.1f);

    OutputFormatAudioSrc src(std::move(stub), 48000, 2);
    std::string err;
    ASSERT_TRUE(src.Init(err)) << err;
    src.SetCompensationPpm(500.0);
    for (int i = 0; i < 100; ++i) {
        RawAudioBuffer buf{};
        ASSERT_TRUE(src.AcquireBuffer(buf, err)) << err;
        src.ReleaseBuffer();
    }
    const double applied_at_cut = src.AppliedCompensationMs();
    EXPECT_GT(applied_at_cut, 0.0);

    src.SetCompensationPpm(0.0);                                        // stop stretching, keep context
    const int64_t before = static_cast<int64_t>(applied_at_cut * 48.0); // ~frames
    (void)before;
    for (int i = 0; i < 100; ++i) {
        RawAudioBuffer buf{};
        ASSERT_TRUE(src.AcquireBuffer(buf, err)) << err;
        src.ReleaseBuffer();
    }
    // A stopped stretching: the applied compensation no longer grows meaningfully
    // (a zero delta is re-armed each acquire).
    EXPECT_NEAR(src.AppliedCompensationMs(), applied_at_cut, 0.5);
}

TEST(OutputFormatAudioSrc, ClockSlaving_OnResamplePath_44kCompensates) {
    // On a genuine resample target (44.1 k) compensation rides the existing
    // context; positive ppm still stretches (more output than the uncompensated
    // baseline).
    const auto count_out = [](double ppm) {
        auto stub = std::make_unique<StubSource>();
        stub->sample_rate = 48000;
        stub->channels = 2;
        stub->frames = 480;
        stub->data.assign(480 * 2, 0.1f);
        OutputFormatAudioSrc src(std::move(stub), 44100, 2);
        std::string err;
        EXPECT_TRUE(src.Init(err)) << err;
        if (ppm != 0.0) {
            src.SetCompensationPpm(ppm);
        }
        int64_t total = 0;
        for (int i = 0; i < 2000; ++i) {
            RawAudioBuffer buf{};
            if (!src.AcquireBuffer(buf, err)) {
                break;
            }
            total += buf.num_frames;
            src.ReleaseBuffer();
        }
        return total;
    };
    EXPECT_GT(count_out(+500.0), count_out(-500.0));
}

// ---------------------------------------------------------------------------
// End-of-stream drain: the resampler's filter delay must reach the encoder.
// ---------------------------------------------------------------------------

namespace {
struct DrainResult {
    int64_t frames_before = 0; // frames the acquire loop produced
    uint32_t drained = 0;      // frames DrainResampler flushed out
    int64_t frames_after = 0;  // frames_before + drained
};

// Drive `packets` buffers of `frames` 48 kHz stereo Float32 frames through a
// decorator targeting target_rate/target_channels, then drain it at stop.
DrainResult DriveThenDrain(uint32_t target_rate, uint32_t target_channels, int packets, double ppm = 0.0,
                           uint32_t frames = 480) {
    auto stub = std::make_unique<StubSource>();
    stub->sample_rate = 48000;
    stub->channels = 2;
    stub->frames = frames;
    stub->data.assign(static_cast<size_t>(frames) * 2u, 0.25f);

    OutputFormatAudioSrc src(std::move(stub), target_rate, target_channels);
    std::string err;
    EXPECT_TRUE(src.Init(err)) << err;
    if (ppm != 0.0) {
        src.SetCompensationPpm(ppm);
    }

    DrainResult r;
    for (int i = 0; i < packets; ++i) {
        RawAudioBuffer buf{};
        if (!src.AcquireBuffer(buf, err)) {
            break;
        }
        r.frames_before += buf.num_frames;
        src.ReleaseBuffer();
    }

    RawAudioBuffer tail{};
    r.drained = src.DrainResampler(tail);
    EXPECT_EQ(tail.num_frames, r.drained);
    if (r.drained > 0) {
        EXPECT_NE(tail.bytes, nullptr);
        // A second drain has nothing left: the delay line was emptied.
        RawAudioBuffer again{};
        EXPECT_EQ(src.DrainResampler(again), 0u);
    }
    r.frames_after = r.frames_before + static_cast<int64_t>(r.drained);
    return r;
}
} // namespace

TEST(OutputFormatAudioSrc, Drain_RateConversion_RecoversFullInputDuration) {
    // 100 x 480 frames at 48 kHz is exactly 1.000 s, i.e. exactly 44100 frames at
    // 44.1 kHz. Without the end-of-stream drain the resampler kept ~10 ms in its
    // filter delay and the audio track ended that much short of the video.
    const DrainResult r = DriveThenDrain(44100, 2, 100);
    EXPECT_GT(r.drained, 0u) << "the filter delay holds real frames at stop";
    EXPECT_LT(r.frames_before, 44100) << "pre-drain output is short by the filter delay";
    EXPECT_GE(r.frames_after, 44099);
    EXPECT_LE(r.frames_after, 44101);
}

TEST(OutputFormatAudioSrc, Drain_CompensationEngaged_RecoversTailAtSetRate) {
    // Clock slaving keeps re-arming a compensation window; the drain must still
    // flush the (identity-rate) resampler with that compensation in effect.
    // +500 ppm can only stretch the timeline, so the drained total must not fall
    // below the input duration.
    constexpr int64_t kInputFrames = 100 * 480;
    const DrainResult r = DriveThenDrain(48000, 2, 100, +500.0);
    EXPECT_GT(r.drained, 0u) << "a compensating context also buffers a filter delay";
    EXPECT_LT(r.frames_before, r.frames_after);
    EXPECT_GE(r.frames_after, kInputFrames) << "the tail must not be lost under compensation";
}

TEST(OutputFormatAudioSrc, Drain_Passthrough_ProducesNothing) {
    // The fast path buffers nothing, so stop has nothing to flush and the frame
    // count already matches the input exactly.
    const DrainResult r = DriveThenDrain(48000, 2, 100);
    EXPECT_EQ(r.drained, 0u);
    EXPECT_EQ(r.frames_before, 100 * 480);
}

TEST(OutputFormatAudioSrc, Drain_RematrixOnly_ProducesNothing) {
    // A stereo->mono context at the same rate rematrixes frame-for-frame; it has
    // no resampling filter, so there is no tail to recover either.
    const DrainResult r = DriveThenDrain(48000, 1, 100);
    EXPECT_EQ(r.drained, 0u);
    EXPECT_EQ(r.frames_before, 100 * 480);
}

} // namespace
