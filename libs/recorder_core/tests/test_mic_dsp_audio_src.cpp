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
