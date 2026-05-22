#include <gtest/gtest.h>

#include "mixed_audio_src.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace recorder_core {
namespace {

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
    void SetInitFail(bool fail, const std::string& msg = "mock init error") {
        init_fail_ = fail;
        init_fail_msg_ = msg;
    }

    int acquire_call_count = 0;
    int release_call_count = 0;
    int shutdown_call_count = 0;
    int init_call_count = 0;

    bool Init(std::string& out_error) override {
        ++init_call_count;
        if (init_fail_) {
            out_error = init_fail_msg_;
            return false;
        }
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
    bool init_fail_ = false;
    std::string init_fail_msg_;
    std::vector<uint8_t> data_;
    std::string name_{"mock"};
};

static std::vector<uint8_t> MakeFloat32Bytes(uint32_t frames, uint32_t channels, float value) {
    const size_t count = static_cast<size_t>(frames) * channels;
    std::vector<float> buf(count, value);
    const auto* bytes = reinterpret_cast<const uint8_t*>(buf.data());
    return std::vector<uint8_t>(bytes, bytes + count * sizeof(float));
}

TEST(MixedAudioSrcTest, MixedAudioSrc_ZeroSources_InitFails) {
    MixedAudioSrc mixer({});
    std::string err;
    EXPECT_FALSE(mixer.Init(err));
    EXPECT_FALSE(err.empty());
}

TEST(MixedAudioSrcTest, MixedAudioSrc_TwoSilentSources_OutputIsSilent) {
    std::vector<std::unique_ptr<IAudioCaptureSource>> sources;
    auto* s0 = new MockAudioCaptureSource();
    auto* s1 = new MockAudioCaptureSource();
    s0->SetPendingFrames(MixedAudioSrc::kMixFrameCount);
    s0->SetSilent(true);
    s1->SetPendingFrames(MixedAudioSrc::kMixFrameCount);
    s1->SetSilent(true);
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(s0));
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(s1));

    MixedAudioSrc mixer(std::move(sources));
    std::string err;
    ASSERT_TRUE(mixer.Init(err));

    RawAudioBuffer buf{};
    ASSERT_TRUE(mixer.AcquireBuffer(buf, err));
    ASSERT_NE(buf.bytes, nullptr);
    EXPECT_EQ(buf.num_frames, MixedAudioSrc::kMixFrameCount);

    const float* samples = reinterpret_cast<const float*>(buf.bytes);
    for (uint32_t i = 0; i < MixedAudioSrc::kMixFrameCount * 2u; ++i) {
        EXPECT_FLOAT_EQ(samples[i], 0.0f) << "at index " << i;
    }

    mixer.ReleaseBuffer();
    mixer.Shutdown();
}

TEST(MixedAudioSrcTest, MixedAudioSrc_OneSourceData_OtherSilent_OutputIsHalfScale) {
    // s0 has 1.0f stereo data; s1 has no pending frames (silence).
    // base_gain = 0.5 -> output should be 0.5f on both channels.
    const auto src_bytes = MakeFloat32Bytes(MixedAudioSrc::kMixFrameCount, 2, 1.0f);

    std::vector<std::unique_ptr<IAudioCaptureSource>> sources;
    auto* s0 = new MockAudioCaptureSource();
    auto* s1 = new MockAudioCaptureSource();
    s0->SetPendingFrames(MixedAudioSrc::kMixFrameCount);
    s0->SetData(src_bytes);
    s1->SetPendingFrames(0);
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(s0));
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(s1));

    MixedAudioSrc mixer(std::move(sources));
    std::string err;
    ASSERT_TRUE(mixer.Init(err));

    RawAudioBuffer buf{};
    ASSERT_TRUE(mixer.AcquireBuffer(buf, err));
    const float* samples = reinterpret_cast<const float*>(buf.bytes);
    for (uint32_t i = 0; i < MixedAudioSrc::kMixFrameCount * 2u; ++i) {
        EXPECT_NEAR(samples[i], 0.5f, 1e-5f) << "at index " << i;
    }

    mixer.ReleaseBuffer();
    mixer.Shutdown();
}

TEST(MixedAudioSrcTest, MixedAudioSrc_TwoSourcesData_SummedAndClamped) {
    // Both sources output 1.0f stereo.
    // base_gain = 0.5 -> per-source contribution 0.5 -> sum 1.0 -> no clamping needed.
    const auto src_bytes = MakeFloat32Bytes(MixedAudioSrc::kMixFrameCount, 2, 1.0f);

    std::vector<std::unique_ptr<IAudioCaptureSource>> sources;
    auto* s0 = new MockAudioCaptureSource();
    auto* s1 = new MockAudioCaptureSource();
    s0->SetPendingFrames(MixedAudioSrc::kMixFrameCount);
    s0->SetData(src_bytes);
    s1->SetPendingFrames(MixedAudioSrc::kMixFrameCount);
    s1->SetData(src_bytes);
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(s0));
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(s1));

    MixedAudioSrc mixer(std::move(sources));
    std::string err;
    ASSERT_TRUE(mixer.Init(err));

    RawAudioBuffer buf{};
    ASSERT_TRUE(mixer.AcquireBuffer(buf, err));
    const float* samples = reinterpret_cast<const float*>(buf.bytes);
    for (uint32_t i = 0; i < MixedAudioSrc::kMixFrameCount * 2u; ++i) {
        EXPECT_NEAR(samples[i], 1.0f, 1e-5f) << "at index " << i;
    }

    mixer.ReleaseBuffer();
    mixer.Shutdown();
}

TEST(MixedAudioSrcTest, MixedAudioSrc_MicGain_AppliedToLastSource) {
    // Both sources output 0.4f stereo. base_gain = 0.5, mic_gain_linear = 2.0.
    // s0 contribution = 0.5 * 0.4 = 0.2
    // s1 (last/mic) contribution = (0.5 * 2.0) * 0.4 = 0.4
    // expected output = 0.6f (no clamping)
    const auto src_bytes = MakeFloat32Bytes(MixedAudioSrc::kMixFrameCount, 2, 0.4f);

    std::vector<std::unique_ptr<IAudioCaptureSource>> sources;
    auto* s0 = new MockAudioCaptureSource();
    auto* s1 = new MockAudioCaptureSource();
    s0->SetPendingFrames(MixedAudioSrc::kMixFrameCount);
    s0->SetData(src_bytes);
    s1->SetPendingFrames(MixedAudioSrc::kMixFrameCount);
    s1->SetData(src_bytes);
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(s0));
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(s1));

    MixedAudioSrc mixer(std::move(sources), 2.0f);
    std::string err;
    ASSERT_TRUE(mixer.Init(err));

    RawAudioBuffer buf{};
    ASSERT_TRUE(mixer.AcquireBuffer(buf, err));
    const float* samples = reinterpret_cast<const float*>(buf.bytes);
    for (uint32_t i = 0; i < MixedAudioSrc::kMixFrameCount * 2u; ++i) {
        EXPECT_NEAR(samples[i], 0.6f, 1e-5f) << "at index " << i;
    }

    mixer.ReleaseBuffer();
    mixer.Shutdown();
}

TEST(MixedAudioSrcTest, MixedAudioSrc_ReleaseBuffer_OnlyReleasesAcquiredSources) {
    // s0 has data; s1 has no pending frames.
    // ReleaseBuffer should only call ReleaseBuffer on s0.
    const auto src_bytes = MakeFloat32Bytes(MixedAudioSrc::kMixFrameCount, 2, 0.5f);

    std::vector<std::unique_ptr<IAudioCaptureSource>> sources;
    auto* s0 = new MockAudioCaptureSource();
    auto* s1 = new MockAudioCaptureSource();
    s0->SetPendingFrames(MixedAudioSrc::kMixFrameCount);
    s0->SetData(src_bytes);
    s1->SetPendingFrames(0);
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(s0));
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(s1));

    MixedAudioSrc mixer(std::move(sources));
    std::string err;
    ASSERT_TRUE(mixer.Init(err));

    RawAudioBuffer buf{};
    ASSERT_TRUE(mixer.AcquireBuffer(buf, err));
    mixer.ReleaseBuffer();

    EXPECT_EQ(s0->release_call_count, 1);
    EXPECT_EQ(s1->release_call_count, 0);

    mixer.Shutdown();
}

TEST(MixedAudioSrcTest, MixedAudioSrc_Shutdown_CallsAllSources) {
    std::vector<std::unique_ptr<IAudioCaptureSource>> sources;
    auto* s0 = new MockAudioCaptureSource();
    auto* s1 = new MockAudioCaptureSource();
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(s0));
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(s1));

    MixedAudioSrc mixer(std::move(sources));
    std::string err;
    ASSERT_TRUE(mixer.Init(err));

    mixer.Shutdown();

    EXPECT_EQ(s0->shutdown_call_count, 1);
    EXPECT_EQ(s1->shutdown_call_count, 1);
}

} // namespace
} // namespace recorder_core
