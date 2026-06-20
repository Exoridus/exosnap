#include <gtest/gtest.h>

#include "mic_dsp_audio_src.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace recorder_core {
namespace {

// Minimal mock inner source (mirrors the mock in test_mixed_audio_src.cpp).
class MockAudioCaptureSource final : public IAudioCaptureSource {
  public:
    explicit MockAudioCaptureSource(uint32_t channels = 2, AudioSampleFormat fmt = AudioSampleFormat::Float32)
        : channels_(channels), format_(fmt) {
    }

    void SetPendingFrames(uint32_t frames) {
        pending_frames_ = frames;
    }
    void SetData(std::vector<uint8_t> data) {
        data_ = std::move(data);
    }
    void SetSilent(bool s) {
        silent_ = s;
    }

    int acquire_call_count = 0;
    int release_call_count = 0;
    int shutdown_call_count = 0;
    int init_call_count = 0;

    bool Init(std::string& out_error) override {
        ++init_call_count;
        (void)out_error;
        return true;
    }

    uint32_t PendingFrameCount() override {
        return pending_frames_;
    }

    bool AcquireBuffer(RawAudioBuffer& out_buf, std::string& out_error) override {
        out_error.clear();
        if (pending_frames_ == 0)
            return false;
        ++acquire_call_count;
        if (data_.empty()) {
            out_buf = {nullptr, 0, true};
        } else {
            out_buf = {data_.data(), pending_frames_, silent_};
        }
        return true;
    }

    void ReleaseBuffer() override {
        ++release_call_count;
    }

    uint32_t SampleRate() const override {
        return 48000;
    }
    uint32_t Channels() const override {
        return channels_;
    }
    AudioSampleFormat SampleFormat() const override {
        return format_;
    }
    const std::string& EndpointName() const override {
        return name_;
    }
    void Shutdown() override {
        ++shutdown_call_count;
    }

  private:
    uint32_t channels_;
    AudioSampleFormat format_;
    uint32_t pending_frames_ = 0;
    bool silent_ = false;
    std::vector<uint8_t> data_;
    std::string name_{"mock mic"};
};

std::vector<uint8_t> MakeFloat32Bytes(uint32_t frames, uint32_t channels, float value) {
    const std::size_t count = static_cast<std::size_t>(frames) * channels;
    std::vector<float> buf(count, value);
    const auto* bytes = reinterpret_cast<const uint8_t*>(buf.data());
    return std::vector<uint8_t>(bytes, bytes + count * sizeof(float));
}

std::vector<uint8_t> MakeInt16Bytes(uint32_t frames, uint32_t channels, int16_t value) {
    const std::size_t count = static_cast<std::size_t>(frames) * channels;
    std::vector<int16_t> buf(count, value);
    const auto* bytes = reinterpret_cast<const uint8_t*>(buf.data());
    return std::vector<uint8_t>(bytes, bytes + count * sizeof(int16_t));
}

} // namespace

// ---------------------------------------------------------------------------
// AnyEnabled()
// ---------------------------------------------------------------------------

TEST(MicDspAudioSrcTest, AnyEnabled_DefaultIsFalse) {
    MicDspConfig cfg;
    EXPECT_FALSE(cfg.AnyEnabled());
    cfg.hpf_enabled = true;
    EXPECT_TRUE(cfg.AnyEnabled());
}

TEST(MicDspAudioSrcTest, AnyEnabled_GateAloneEnablesChain) {
    MicDspConfig cfg;
    EXPECT_FALSE(cfg.AnyEnabled());
    cfg.gate_enabled = true;
    EXPECT_TRUE(cfg.AnyEnabled());
}

TEST(MicDspAudioSrcTest, AnyEnabled_RnnoiseAloneEnablesChain) {
    MicDspConfig cfg;
    EXPECT_FALSE(cfg.AnyEnabled());
    cfg.rnnoise_enabled = true;
    EXPECT_TRUE(cfg.AnyEnabled());
}

// ---------------------------------------------------------------------------
// Pass-through (DSP disabled) — output equals input within int16->float eps
// ---------------------------------------------------------------------------

TEST(MicDspAudioSrcTest, PassThroughWhenDisabled_Float32) {
    auto mock = std::make_unique<MockAudioCaptureSource>(2, AudioSampleFormat::Float32);
    auto* mock_ptr = mock.get();
    const uint32_t frames = 480;
    mock_ptr->SetPendingFrames(frames);
    mock_ptr->SetData(MakeFloat32Bytes(frames, 2, 0.25f));

    MicDspConfig cfg; // hpf_enabled = false → no-op chain
    MicDspAudioSrc src(std::move(mock), cfg);
    std::string err;
    ASSERT_TRUE(src.Init(err)) << err;

    RawAudioBuffer out{};
    ASSERT_TRUE(src.AcquireBuffer(out, err)) << err;
    ASSERT_NE(out.bytes, nullptr);
    EXPECT_EQ(out.num_frames, frames);

    const float* samples = reinterpret_cast<const float*>(out.bytes);
    for (uint32_t i = 0; i < frames * 2; ++i) {
        EXPECT_NEAR(samples[i], 0.25f, 1e-6f) << "index " << i;
    }
    src.ReleaseBuffer();
}

TEST(MicDspAudioSrcTest, PassThroughWhenDisabled_Int16RoundTrip) {
    auto mock = std::make_unique<MockAudioCaptureSource>(1, AudioSampleFormat::Int16);
    auto* mock_ptr = mock.get();
    const uint32_t frames = 256;
    const int16_t raw = 8192; // 0.25 of full scale (8192 / 32768)
    mock_ptr->SetPendingFrames(frames);
    mock_ptr->SetData(MakeInt16Bytes(frames, 1, raw));

    MicDspConfig cfg; // disabled
    MicDspAudioSrc src(std::move(mock), cfg);
    std::string err;
    ASSERT_TRUE(src.Init(err)) << err;

    RawAudioBuffer out{};
    ASSERT_TRUE(src.AcquireBuffer(out, err)) << err;
    ASSERT_NE(out.bytes, nullptr);

    const float expected = static_cast<float>(raw) / 32768.0f;
    const float* samples = reinterpret_cast<const float*>(out.bytes);
    for (uint32_t i = 0; i < frames; ++i) {
        EXPECT_NEAR(samples[i], expected, 1e-6f) << "index " << i;
    }
    src.ReleaseBuffer();
}

// ---------------------------------------------------------------------------
// HPF enabled attenuates a DC input
// ---------------------------------------------------------------------------

TEST(MicDspAudioSrcTest, HpfAttenuatesDcWhenEnabled) {
    auto mock = std::make_unique<MockAudioCaptureSource>(2, AudioSampleFormat::Float32);
    auto* mock_ptr = mock.get();
    const uint32_t frames = 4000;
    mock_ptr->SetPendingFrames(frames);
    mock_ptr->SetData(MakeFloat32Bytes(frames, 2, 0.8f)); // pure DC

    MicDspConfig cfg;
    cfg.hpf_enabled = true;
    cfg.hpf_cutoff_hz = 80.0f;
    MicDspAudioSrc src(std::move(mock), cfg);
    std::string err;
    ASSERT_TRUE(src.Init(err)) << err;

    RawAudioBuffer out{};
    ASSERT_TRUE(src.AcquireBuffer(out, err)) << err;
    ASSERT_NE(out.bytes, nullptr);

    // The DC component must decay toward zero in the tail.
    const float* samples = reinterpret_cast<const float*>(out.bytes);
    float tail = 0.0f;
    for (uint32_t i = (frames * 2 - 200); i < frames * 2; ++i) {
        tail = std::max(tail, std::fabs(samples[i]));
    }
    EXPECT_LT(tail, 1e-2f);
    src.ReleaseBuffer();
}

// ---------------------------------------------------------------------------
// Gate enabled attenuates a below-threshold input
// ---------------------------------------------------------------------------

TEST(MicDspAudioSrcTest, GateAttenuatesQuietInputWhenEnabled) {
    auto mock = std::make_unique<MockAudioCaptureSource>(2, AudioSampleFormat::Float32);
    auto* mock_ptr = mock.get();
    const uint32_t frames = 48000;
    mock_ptr->SetPendingFrames(frames);
    // 0.001 ≈ -60 dBFS is below the -45 dB threshold → gate stays closed.
    mock_ptr->SetData(MakeFloat32Bytes(frames, 2, 0.001f));

    MicDspConfig cfg;
    cfg.gate_enabled = true;
    cfg.gate_threshold_db = -45.0f;
    MicDspAudioSrc src(std::move(mock), cfg);
    std::string err;
    ASSERT_TRUE(src.Init(err)) << err;

    RawAudioBuffer out{};
    ASSERT_TRUE(src.AcquireBuffer(out, err)) << err;
    ASSERT_NE(out.bytes, nullptr);

    // The closed gate drives the quiet tail toward silence.
    const float* samples = reinterpret_cast<const float*>(out.bytes);
    float tail = 0.0f;
    for (uint32_t i = (frames * 2 - 200); i < frames * 2; ++i) {
        tail = std::max(tail, std::fabs(samples[i]));
    }
    EXPECT_LT(tail, 1e-4f);
    src.ReleaseBuffer();
}

// ---------------------------------------------------------------------------
// Chain order: HPF -> gate (both enabled). Low-frequency rumble below the gate
// threshold is removed by the HPF first, so it cannot hold the gate open.
// ---------------------------------------------------------------------------

TEST(MicDspAudioSrcTest, HpfThenGateChainAttenuatesLowFreqRumble) {
    auto mock = std::make_unique<MockAudioCaptureSource>(2, AudioSampleFormat::Float32);
    auto* mock_ptr = mock.get();
    const uint32_t frames = 48000;
    mock_ptr->SetPendingFrames(frames);
    mock_ptr->SetData(MakeFloat32Bytes(frames, 2, 0.6f)); // strong DC rumble

    MicDspConfig cfg;
    cfg.hpf_enabled = true;
    cfg.hpf_cutoff_hz = 80.0f;
    cfg.gate_enabled = true;
    cfg.gate_threshold_db = -45.0f;
    MicDspAudioSrc src(std::move(mock), cfg);
    std::string err;
    ASSERT_TRUE(src.Init(err)) << err;

    RawAudioBuffer out{};
    ASSERT_TRUE(src.AcquireBuffer(out, err)) << err;
    ASSERT_NE(out.bytes, nullptr);

    // HPF strips the DC so the residual is well below threshold; the gate then
    // closes. The combined result decays to silence in the tail.
    const float* samples = reinterpret_cast<const float*>(out.bytes);
    float tail = 0.0f;
    for (uint32_t i = (frames * 2 - 200); i < frames * 2; ++i) {
        tail = std::max(tail, std::fabs(samples[i]));
    }
    EXPECT_LT(tail, 1e-3f);
    src.ReleaseBuffer();
}

// ---------------------------------------------------------------------------
// AnyEnabled() — AGC alone enables the chain
// ---------------------------------------------------------------------------

TEST(MicDspAudioSrcTest, AnyEnabled_AgcAloneEnablesChain) {
    MicDspConfig cfg;
    EXPECT_FALSE(cfg.AnyEnabled());
    cfg.agc_enabled = true;
    EXPECT_TRUE(cfg.AnyEnabled());
}

// ---------------------------------------------------------------------------
// AGC enabled boosts a quiet (above-floor) input toward the target.
// ---------------------------------------------------------------------------

TEST(MicDspAudioSrcTest, AgcBoostsQuietInputWhenEnabled) {
    auto mock = std::make_unique<MockAudioCaptureSource>(2, AudioSampleFormat::Float32);
    auto* mock_ptr = mock.get();
    const uint32_t frames = 96000; // 2 s — time for the slow attack to converge
    mock_ptr->SetPendingFrames(frames);
    // 0.0158 ≈ -36 dBFS: above the noise floor, ~18 dB below the -18 dB target.
    const float input = 0.0158f;
    mock_ptr->SetData(MakeFloat32Bytes(frames, 2, input));

    MicDspConfig cfg;
    cfg.agc_enabled = true;
    cfg.agc_target_db = -18.0f;
    MicDspAudioSrc src(std::move(mock), cfg);
    std::string err;
    ASSERT_TRUE(src.Init(err)) << err;

    RawAudioBuffer out{};
    ASSERT_TRUE(src.AcquireBuffer(out, err)) << err;
    ASSERT_NE(out.bytes, nullptr);

    // The makeup gain pulls the quiet input up: the tail is well above the input.
    const float* samples = reinterpret_cast<const float*>(out.bytes);
    float tail = 0.0f;
    for (uint32_t i = (frames * 2 - 480); i < frames * 2; ++i) {
        tail = std::max(tail, std::fabs(samples[i]));
    }
    EXPECT_GT(tail, input * 2.0f);
}

// ---------------------------------------------------------------------------
// Full chain HPF + gate + AGC all enabled runs without blowup.
// ---------------------------------------------------------------------------

TEST(MicDspAudioSrcTest, FullChainHpfGateAgcRunsWithoutBlowup) {
    auto mock = std::make_unique<MockAudioCaptureSource>(2, AudioSampleFormat::Float32);
    auto* mock_ptr = mock.get();
    const uint32_t frames = 48000;
    mock_ptr->SetPendingFrames(frames);
    // Moderate speech-level signal, comfortably above the gate threshold.
    mock_ptr->SetData(MakeFloat32Bytes(frames, 2, 0.2f));

    MicDspConfig cfg;
    cfg.hpf_enabled = true;
    cfg.hpf_cutoff_hz = 80.0f;
    cfg.gate_enabled = true;
    cfg.gate_threshold_db = -45.0f;
    cfg.agc_enabled = true;
    cfg.agc_target_db = -18.0f;
    MicDspAudioSrc src(std::move(mock), cfg);
    std::string err;
    ASSERT_TRUE(src.Init(err)) << err;

    RawAudioBuffer out{};
    ASSERT_TRUE(src.AcquireBuffer(out, err)) << err;
    ASSERT_NE(out.bytes, nullptr);

    // Every output sample is finite and bounded (no NaN/Inf, no runaway gain).
    const float* samples = reinterpret_cast<const float*>(out.bytes);
    for (uint32_t i = 0; i < frames * 2; ++i) {
        ASSERT_TRUE(std::isfinite(samples[i])) << "index " << i;
        EXPECT_LE(std::fabs(samples[i]), 4.0f) << "index " << i;
    }
    src.ReleaseBuffer();
}

// The full mic-DSP chain with RNNoise as the 4th stage (HPF + gate + AGC +
// RNNoise all on) runs end-to-end without blowing up; output is finite/bounded.
TEST(MicDspAudioSrcTest, FullChainWithRnnoiseRunsWithoutBlowup) {
    auto mock = std::make_unique<MockAudioCaptureSource>(2, AudioSampleFormat::Float32);
    auto* mock_ptr = mock.get();
    const uint32_t frames = 48000; // 1 s at 48 kHz — RNNoise's required rate
    mock_ptr->SetPendingFrames(frames);
    mock_ptr->SetData(MakeFloat32Bytes(frames, 2, 0.2f));

    MicDspConfig cfg;
    cfg.hpf_enabled = true;
    cfg.hpf_cutoff_hz = 80.0f;
    cfg.gate_enabled = true;
    cfg.gate_threshold_db = -45.0f;
    cfg.agc_enabled = true;
    cfg.agc_target_db = -18.0f;
    cfg.rnnoise_enabled = true;
    ASSERT_TRUE(cfg.AnyEnabled());
    MicDspAudioSrc src(std::move(mock), cfg);
    std::string err;
    ASSERT_TRUE(src.Init(err)) << err;

    RawAudioBuffer out{};
    ASSERT_TRUE(src.AcquireBuffer(out, err)) << err;
    ASSERT_NE(out.bytes, nullptr);
    EXPECT_EQ(out.num_frames, frames); // length preserved through the chain

    const float* samples = reinterpret_cast<const float*>(out.bytes);
    for (uint32_t i = 0; i < frames * 2; ++i) {
        ASSERT_TRUE(std::isfinite(samples[i])) << "index " << i;
        EXPECT_LE(std::fabs(samples[i]), 4.0f) << "index " << i;
    }
    src.ReleaseBuffer();
}

// ---------------------------------------------------------------------------
// Output format / delegation
// ---------------------------------------------------------------------------

TEST(MicDspAudioSrcTest, OutputSampleFormatIsFloat32) {
    auto mock = std::make_unique<MockAudioCaptureSource>(2, AudioSampleFormat::Int16);
    MicDspConfig cfg;
    cfg.hpf_enabled = true;
    MicDspAudioSrc src(std::move(mock), cfg);
    std::string err;
    ASSERT_TRUE(src.Init(err)) << err;
    EXPECT_EQ(src.SampleFormat(), AudioSampleFormat::Float32);
    EXPECT_EQ(src.Channels(), 2u);
    EXPECT_EQ(src.SampleRate(), 48000u);
}

TEST(MicDspAudioSrcTest, SilentBufferPassesThrough) {
    auto mock = std::make_unique<MockAudioCaptureSource>(2, AudioSampleFormat::Float32);
    auto* mock_ptr = mock.get();
    mock_ptr->SetPendingFrames(480);
    mock_ptr->SetData(MakeFloat32Bytes(480, 2, 0.5f));
    mock_ptr->SetSilent(true);

    MicDspConfig cfg;
    cfg.hpf_enabled = true;
    MicDspAudioSrc src(std::move(mock), cfg);
    std::string err;
    ASSERT_TRUE(src.Init(err)) << err;

    RawAudioBuffer out{};
    ASSERT_TRUE(src.AcquireBuffer(out, err)) << err;
    EXPECT_TRUE(out.silent);
    src.ReleaseBuffer();
}

TEST(MicDspAudioSrcTest, DelegatesLifecycleToInner) {
    auto mock = std::make_unique<MockAudioCaptureSource>(2, AudioSampleFormat::Float32);
    auto* mock_ptr = mock.get();
    mock_ptr->SetPendingFrames(480);
    mock_ptr->SetData(MakeFloat32Bytes(480, 2, 0.1f));

    MicDspConfig cfg;
    cfg.hpf_enabled = true;
    MicDspAudioSrc src(std::move(mock), cfg);
    std::string err;
    ASSERT_TRUE(src.Init(err)) << err;
    EXPECT_EQ(mock_ptr->init_call_count, 1);
    EXPECT_EQ(src.PendingFrameCount(), 480u);

    RawAudioBuffer out{};
    ASSERT_TRUE(src.AcquireBuffer(out, err)) << err;
    EXPECT_EQ(mock_ptr->acquire_call_count, 1);
    src.ReleaseBuffer();
    EXPECT_EQ(mock_ptr->release_call_count, 1);
    src.Shutdown();
    EXPECT_EQ(mock_ptr->shutdown_call_count, 1);
}

} // namespace recorder_core
