#include <gtest/gtest.h>

#include "mixed_audio_src.h"
#include "output_format_audio_src.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
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

static std::vector<uint8_t> MakeInt16Bytes(uint32_t frames, uint32_t channels, int16_t value) {
    const size_t count = static_cast<size_t>(frames) * channels;
    std::vector<int16_t> buf(count, value);
    const auto* bytes = reinterpret_cast<const uint8_t*>(buf.data());
    return std::vector<uint8_t>(bytes, bytes + count * sizeof(int16_t));
}

static std::vector<float> MakeUnityGains(size_t count) {
    return std::vector<float>(count, 1.0f);
}

// A source that delivers a finite, ordered sequence of packets, one per
// Acquire/Release cycle, and reports the real pending-frame count of the packet
// at the head of the queue (0 once drained). Unlike MockAudioCaptureSource it
// decrements as packets are consumed, so it can model realistic device periods
// (e.g. 448- and 512-frame packets) and be drained the way the audio thread
// drains a real source.
class QueueMockSource final : public IAudioCaptureSource {
  public:
    // Enqueue `frames` of interleaved-stereo Float32 whose left and right sample
    // at global frame index (start_frame + i) both equal value_at(start_frame + i).
    // Lets a test lay a single continuous ramp across many differently sized
    // packets and then verify the emitted stream is that ramp, unbroken.
    template <typename Fn> void EnqueueRamp(uint32_t frames, uint32_t start_frame, Fn value_at) {
        std::vector<float> buf(static_cast<size_t>(frames) * 2u);
        for (uint32_t i = 0; i < frames; ++i) {
            const float v = value_at(start_frame + i);
            buf[(i * 2) + 0] = v;
            buf[(i * 2) + 1] = v;
        }
        queue_.push_back(Packet{frames, std::move(buf)});
    }

    uint32_t PendingFrameCount() override {
        return queue_.empty() ? 0u : queue_.front().frames;
    }

    bool AcquireBuffer(RawAudioBuffer& out_buf, std::string& out_error) override {
        out_error.clear();
        if (queue_.empty() || held_)
            return false;
        held_ = true;
        const Packet& p = queue_.front();
        out_buf = {reinterpret_cast<const uint8_t*>(p.data.data()), p.frames, false, false};
        return true;
    }

    void ReleaseBuffer() override {
        if (!held_)
            return;
        held_ = false;
        queue_.pop_front();
    }

    bool Init(std::string&) override {
        return true;
    }
    uint32_t SampleRate() const override {
        return 48000;
    }
    uint32_t Channels() const override {
        return 2;
    }
    AudioSampleFormat SampleFormat() const override {
        return AudioSampleFormat::Float32;
    }
    const std::string& EndpointName() const override {
        return name_;
    }
    void Shutdown() override {
    }

  private:
    struct Packet {
        uint32_t frames;
        std::vector<float> data;
    };
    std::deque<Packet> queue_;
    bool held_ = false;
    std::string name_{"queue-mock"};
};

// Drive the mixer the way AudioThread::Run does: while it reports pending
// frames, acquire, copy out the emitted stereo samples, release. Returns the
// concatenated emitted stream (interleaved stereo).
static std::vector<float> DrainMixer(MixedAudioSrc& mixer) {
    std::vector<float> out;
    std::string err;
    while (mixer.PendingFrameCount() > 0) {
        RawAudioBuffer buf{};
        if (!mixer.AcquireBuffer(buf, err))
            break;
        if (buf.num_frames > 0 && buf.bytes != nullptr) {
            const float* f = reinterpret_cast<const float*>(buf.bytes);
            out.insert(out.end(), f, f + static_cast<size_t>(buf.num_frames) * 2u);
        }
        mixer.ReleaseBuffer();
        if (buf.num_frames == 0)
            break; // no forward progress; avoid spinning
    }
    return out;
}

TEST(MixedAudioSrcTest, MixedAudioSrc_ZeroSources_InitFails) {
    MixedAudioSrc mixer({}, {});
    std::string err;
    EXPECT_FALSE(mixer.Init(err));
    EXPECT_FALSE(err.empty());
}

TEST(MixedAudioSrcTest, MixedAudioSrc_GainCountMismatch_InitFails) {
    std::vector<std::unique_ptr<IAudioCaptureSource>> sources;
    sources.push_back(std::make_unique<MockAudioCaptureSource>());
    MixedAudioSrc mixer(std::move(sources), {});
    std::string err;
    EXPECT_FALSE(mixer.Init(err));
    EXPECT_FALSE(err.empty());
}

TEST(MixedAudioSrcTest, MixedAudioSrc_TwoSilentSources_OutputIsSilent) {
    std::vector<std::unique_ptr<IAudioCaptureSource>> sources;
    // Silent sources still deliver frames (WASAPI flags a buffer SILENT rather
    // than shrinking it); the mixer must carry those frames as zeros.
    const auto silent_bytes = MakeFloat32Bytes(MixedAudioSrc::kMixFrameCount, 2, 1.0f);
    auto* s0 = new MockAudioCaptureSource();
    auto* s1 = new MockAudioCaptureSource();
    s0->SetPendingFrames(MixedAudioSrc::kMixFrameCount);
    s0->SetData(silent_bytes);
    s0->SetSilent(true);
    s1->SetPendingFrames(MixedAudioSrc::kMixFrameCount);
    s1->SetData(silent_bytes);
    s1->SetSilent(true);
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(s0));
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(s1));

    MixedAudioSrc mixer(std::move(sources), MakeUnityGains(2));
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

    MixedAudioSrc mixer(std::move(sources), MakeUnityGains(2));
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

    MixedAudioSrc mixer(std::move(sources), MakeUnityGains(2));
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

TEST(MixedAudioSrcTest, MixedAudioSrc_LimiterEnabled_RespectsCustomCeiling) {
    // Two sources at 1.0 → mixed sum = 1.0 (within full scale). The legacy
    // hard-clamp path would leave this at 1.0; with the limiter enabled at a
    // 0.5 ceiling the output must be brought down to <= 0.5.
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

    MixedAudioSrc mixer(std::move(sources), MakeUnityGains(2), /*limiter_enabled=*/true,
                        /*limiter_ceiling_linear=*/0.5f);
    std::string err;
    ASSERT_TRUE(mixer.Init(err));

    RawAudioBuffer buf{};
    ASSERT_TRUE(mixer.AcquireBuffer(buf, err));
    const float* samples = reinterpret_cast<const float*>(buf.bytes);
    float max_abs = 0.0f;
    for (uint32_t i = 0; i < MixedAudioSrc::kMixFrameCount * 2u; ++i) {
        max_abs = std::max(max_abs, std::fabs(samples[i]));
    }
    EXPECT_LE(max_abs, 0.5f + 1e-5f);

    mixer.ReleaseBuffer();
    mixer.Shutdown();
}

TEST(MixedAudioSrcTest, MixedAudioSrc_AppSysNoMic_DoesNotApplyMicGainToLastSource) {
    const auto src_bytes = MakeFloat32Bytes(MixedAudioSrc::kMixFrameCount, 2, 1.0f);

    std::vector<std::unique_ptr<IAudioCaptureSource>> sources;
    auto* app = new MockAudioCaptureSource();
    auto* sys = new MockAudioCaptureSource();
    app->SetPendingFrames(MixedAudioSrc::kMixFrameCount);
    app->SetData(src_bytes);
    sys->SetPendingFrames(MixedAudioSrc::kMixFrameCount);
    sys->SetData(src_bytes);
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(app));
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(sys));

    MixedAudioSrc mixer(std::move(sources), {1.0f, 1.0f});
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

TEST(MixedAudioSrcTest, MixedAudioSrc_AppMic_AppliesMicGainOnlyToMicSource) {
    const auto app_bytes = MakeFloat32Bytes(MixedAudioSrc::kMixFrameCount, 2, 0.0f);
    const auto mic_bytes = MakeFloat32Bytes(MixedAudioSrc::kMixFrameCount, 2, 0.25f);

    std::vector<std::unique_ptr<IAudioCaptureSource>> sources;
    auto* app = new MockAudioCaptureSource();
    auto* mic = new MockAudioCaptureSource();
    app->SetPendingFrames(MixedAudioSrc::kMixFrameCount);
    app->SetData(app_bytes);
    mic->SetPendingFrames(MixedAudioSrc::kMixFrameCount);
    mic->SetData(mic_bytes);
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(app));
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(mic));

    MixedAudioSrc mixer(std::move(sources), {1.0f, 2.0f});
    std::string err;
    ASSERT_TRUE(mixer.Init(err));

    RawAudioBuffer buf{};
    ASSERT_TRUE(mixer.AcquireBuffer(buf, err));
    const float* samples = reinterpret_cast<const float*>(buf.bytes);
    for (uint32_t i = 0; i < MixedAudioSrc::kMixFrameCount * 2u; ++i) {
        EXPECT_NEAR(samples[i], 0.25f, 1e-5f) << "at index " << i;
    }

    mixer.ReleaseBuffer();
    mixer.Shutdown();
}

TEST(MixedAudioSrcTest, MixedAudioSrc_Int16Conversion) {
    const auto src_bytes = MakeInt16Bytes(MixedAudioSrc::kMixFrameCount, 2, 16384);

    std::vector<std::unique_ptr<IAudioCaptureSource>> sources;
    auto* s0 = new MockAudioCaptureSource(2, AudioSampleFormat::Int16);
    s0->SetPendingFrames(MixedAudioSrc::kMixFrameCount);
    s0->SetData(src_bytes);
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(s0));

    MixedAudioSrc mixer(std::move(sources), MakeUnityGains(1));
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

TEST(MixedAudioSrcTest, MixedAudioSrc_MonoSource_DuplicatesToStereo) {
    const auto src_bytes = MakeFloat32Bytes(MixedAudioSrc::kMixFrameCount, 1, 0.25f);

    std::vector<std::unique_ptr<IAudioCaptureSource>> sources;
    auto* mono = new MockAudioCaptureSource(1, AudioSampleFormat::Float32);
    mono->SetPendingFrames(MixedAudioSrc::kMixFrameCount);
    mono->SetData(src_bytes);
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(mono));

    MixedAudioSrc mixer(std::move(sources), MakeUnityGains(1));
    std::string err;
    ASSERT_TRUE(mixer.Init(err));

    RawAudioBuffer buf{};
    ASSERT_TRUE(mixer.AcquireBuffer(buf, err));
    const float* samples = reinterpret_cast<const float*>(buf.bytes);
    for (uint32_t frame = 0; frame < MixedAudioSrc::kMixFrameCount; ++frame) {
        EXPECT_NEAR(samples[(frame * 2) + 0], 0.25f, 1e-5f) << "left at frame " << frame;
        EXPECT_NEAR(samples[(frame * 2) + 1], 0.25f, 1e-5f) << "right at frame " << frame;
    }

    mixer.ReleaseBuffer();
    mixer.Shutdown();
}

TEST(MixedAudioSrcTest, MixedAudioSrc_ShortBuffer_EmitsExactFrameCountNoPadding) {
    // A source packet shorter than the nominal device period must be emitted at
    // its real length — the mixer must not pad it up to kMixFrameCount with
    // fabricated silence (doing so inflated the sample-count-based PTS).
    constexpr uint32_t kShortFrames = 24;
    const auto src_bytes = MakeFloat32Bytes(kShortFrames, 2, 0.7f);

    std::vector<std::unique_ptr<IAudioCaptureSource>> sources;
    auto* s0 = new MockAudioCaptureSource();
    s0->SetPendingFrames(kShortFrames);
    s0->SetData(src_bytes);
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(s0));

    MixedAudioSrc mixer(std::move(sources), MakeUnityGains(1));
    std::string err;
    ASSERT_TRUE(mixer.Init(err));

    RawAudioBuffer buf{};
    ASSERT_TRUE(mixer.AcquireBuffer(buf, err));
    EXPECT_EQ(buf.num_frames, kShortFrames);
    const float* samples = reinterpret_cast<const float*>(buf.bytes);
    for (uint32_t frame = 0; frame < kShortFrames; ++frame) {
        EXPECT_NEAR(samples[(frame * 2) + 0], 0.7f, 1e-5f) << "left at frame " << frame;
        EXPECT_NEAR(samples[(frame * 2) + 1], 0.7f, 1e-5f) << "right at frame " << frame;
    }

    mixer.ReleaseBuffer();
    mixer.Shutdown();
}

TEST(MixedAudioSrcTest, MixedAudioSrc_PartialInitFailure_ShutsDownEveryTouchedSourceIncludingTheFailedOne) {
    // REGRESSION: a source whose own Init() fails can still have partially
    // acquired resources (e.g. WasapiLoopback::Init() acquiring a device/audio
    // client before a later step fails) — it must be shut down too, not just the
    // sources that already succeeded.
    std::vector<std::unique_ptr<IAudioCaptureSource>> sources;
    auto* first = new MockAudioCaptureSource();
    auto* second = new MockAudioCaptureSource();
    second->SetInitFail(true, "second init fail");
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(first));
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(second));

    MixedAudioSrc mixer(std::move(sources), MakeUnityGains(2));
    std::string err;
    EXPECT_FALSE(mixer.Init(err));
    EXPECT_FALSE(err.empty());
    EXPECT_EQ(first->init_call_count, 1);
    EXPECT_EQ(second->init_call_count, 1);
    EXPECT_EQ(first->shutdown_call_count, 1);
    EXPECT_EQ(second->shutdown_call_count, 1);
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

    MixedAudioSrc mixer(std::move(sources), MakeUnityGains(2));
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

    MixedAudioSrc mixer(std::move(sources), MakeUnityGains(2));
    std::string err;
    ASSERT_TRUE(mixer.Init(err));

    mixer.Shutdown();

    EXPECT_EQ(s0->shutdown_call_count, 1);
    EXPECT_EQ(s1->shutdown_call_count, 1);
}

// Cross-path symmetry (recorder_session single-source track construction):
//   * gain == 1.0  -> the raw source is used directly, then wrapped in
//                     OutputFormatAudioSrc by the audio thread.
//   * gain != 1.0  -> the source is wrapped in MixedAudioSrc (which converts to
//                     Float32), then wrapped in OutputFormatAudioSrc.
// For an Int16 source both paths must deliver the same Float32 signal to the
// encoder. Before the OutputFormatAudioSrc Int16 fix, the direct path handed
// raw int16 bytes through as if they were Float32 (garbage), so the two paths
// diverged.
TEST(MixedAudioSrcTest, SingleSource_Int16_DirectAndMixedPathsAgree) {
    constexpr uint32_t kFrames = MixedAudioSrc::kMixFrameCount; // 480
    constexpr int16_t kVal = 16384;                             // 0.5 full scale
    const std::vector<uint8_t> i16_bytes = MakeInt16Bytes(kFrames, 2, kVal);

    // --- Direct path: OutputFormatAudioSrc(Int16 source) ---
    auto direct_src = std::make_unique<MockAudioCaptureSource>(2, AudioSampleFormat::Int16);
    direct_src->SetPendingFrames(kFrames);
    direct_src->SetData(i16_bytes);
    OutputFormatAudioSrc direct(std::move(direct_src), 48000, 2);
    std::string err;
    ASSERT_TRUE(direct.Init(err)) << err;
    RawAudioBuffer direct_buf{};
    ASSERT_TRUE(direct.AcquireBuffer(direct_buf, err)) << err;
    ASSERT_NE(direct_buf.bytes, nullptr);
    const float* direct_f = reinterpret_cast<const float*>(direct_buf.bytes);
    std::vector<float> direct_samples(direct_f, direct_f + static_cast<size_t>(direct_buf.num_frames) * 2u);
    direct.ReleaseBuffer();

    // --- Mixed path: OutputFormatAudioSrc(MixedAudioSrc([Int16 source], gain 1.0)) ---
    auto mixed_inner = std::make_unique<MockAudioCaptureSource>(2, AudioSampleFormat::Int16);
    mixed_inner->SetPendingFrames(kFrames);
    mixed_inner->SetData(i16_bytes);
    std::vector<std::unique_ptr<IAudioCaptureSource>> mixed_sources;
    mixed_sources.push_back(std::move(mixed_inner));
    auto mixer = std::make_unique<MixedAudioSrc>(std::move(mixed_sources), MakeUnityGains(1));
    OutputFormatAudioSrc mixed(std::move(mixer), 48000, 2);
    ASSERT_TRUE(mixed.Init(err)) << err;
    RawAudioBuffer mixed_buf{};
    ASSERT_TRUE(mixed.AcquireBuffer(mixed_buf, err)) << err;
    ASSERT_NE(mixed_buf.bytes, nullptr);
    const float* mixed_f = reinterpret_cast<const float*>(mixed_buf.bytes);
    std::vector<float> mixed_samples(mixed_f, mixed_f + static_cast<size_t>(mixed_buf.num_frames) * 2u);
    mixed.ReleaseBuffer();

    ASSERT_EQ(direct_samples.size(), mixed_samples.size());

    auto rms = [](const std::vector<float>& v) {
        double acc = 0.0;
        for (float s : v)
            acc += static_cast<double>(s) * s;
        return v.empty() ? 0.0 : std::sqrt(acc / static_cast<double>(v.size()));
    };

    // Both paths carry the same 0.5 full-scale signal.
    for (size_t i = 0; i < direct_samples.size(); ++i) {
        EXPECT_NEAR(direct_samples[i], 0.5f, 1e-6f) << " direct at " << i;
        EXPECT_NEAR(direct_samples[i], mixed_samples[i], 1e-6f) << " path mismatch at " << i;
    }
    EXPECT_NEAR(rms(direct_samples), rms(mixed_samples), 1e-6);
}

// ---------------------------------------------------------------------------
// Frame-accounting regression tests: the mixer must neither discard a source
// packet's tail nor fabricate trailing silence, and the emitted sample count
// must equal what the source delivered (so the sample-count-based PTS is
// drift-free on realistic device periods that are not exactly 480 frames).
// ---------------------------------------------------------------------------

// A ramp value keyed to the global frame index — distinct per frame so any
// dropped or inserted frame shows up as a discontinuity.
static float RampAt(uint32_t frame) {
    return static_cast<float>(frame % 4096) * (1.0f / 8192.0f); // stays within +-0.5
}

TEST(MixedAudioSrcTest, MixedAudioSrc_PacketLargerThanNominal_NoTailDiscarded) {
    // One 512-frame packet (a 10.67 ms device period). The buggy mixer emitted a
    // fixed 480 and discarded the trailing 32 frames.
    constexpr uint32_t kFrames = 512;
    auto* src = new QueueMockSource();
    src->EnqueueRamp(kFrames, 0, RampAt);

    std::vector<std::unique_ptr<IAudioCaptureSource>> sources;
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(src));
    MixedAudioSrc mixer(std::move(sources), MakeUnityGains(1));
    std::string err;
    ASSERT_TRUE(mixer.Init(err));

    const std::vector<float> out = DrainMixer(mixer);
    ASSERT_EQ(out.size(), static_cast<size_t>(kFrames) * 2u); // all 512 frames survive
    for (uint32_t f = 0; f < kFrames; ++f) {
        EXPECT_NEAR(out[(f * 2) + 0], RampAt(f), 1e-6f) << "left at frame " << f;
        EXPECT_NEAR(out[(f * 2) + 1], RampAt(f), 1e-6f) << "right at frame " << f;
    }
    mixer.Shutdown();
}

TEST(MixedAudioSrcTest, MixedAudioSrc_PacketSmallerThanNominal_NoSilenceInserted) {
    // One 448-frame packet (a ~9.3 ms device period). The buggy mixer emitted a
    // fixed 480 and appended 32 fabricated silent frames.
    constexpr uint32_t kFrames = 448;
    auto* src = new QueueMockSource();
    src->EnqueueRamp(kFrames, 0, RampAt);

    std::vector<std::unique_ptr<IAudioCaptureSource>> sources;
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(src));
    MixedAudioSrc mixer(std::move(sources), MakeUnityGains(1));
    std::string err;
    ASSERT_TRUE(mixer.Init(err));

    const std::vector<float> out = DrainMixer(mixer);
    ASSERT_EQ(out.size(), static_cast<size_t>(kFrames) * 2u); // exactly 448, no padding
    for (uint32_t f = 0; f < kFrames; ++f) {
        EXPECT_NEAR(out[(f * 2) + 0], RampAt(f), 1e-6f) << "frame " << f;
    }
    mixer.Shutdown();
}

TEST(MixedAudioSrcTest, MixedAudioSrc_ManyPackets_SamplePreservingAndContinuous) {
    // A single continuous ramp split across packets of realistic, varying device
    // periods. After draining, the emitted stream must be that exact ramp with
    // no gaps and no extra samples — i.e. Sigma in == Sigma out and PTS-continuous.
    const uint32_t sizes[] = {448, 512, 480, 400, 530, 441, 512, 448};
    auto* src = new QueueMockSource();
    uint32_t total = 0;
    for (uint32_t s : sizes) {
        src->EnqueueRamp(s, total, RampAt);
        total += s;
    }

    std::vector<std::unique_ptr<IAudioCaptureSource>> sources;
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(src));
    MixedAudioSrc mixer(std::move(sources), MakeUnityGains(1));
    std::string err;
    ASSERT_TRUE(mixer.Init(err));

    const std::vector<float> out = DrainMixer(mixer);
    ASSERT_EQ(out.size(), static_cast<size_t>(total) * 2u);
    for (uint32_t f = 0; f < total; ++f) {
        EXPECT_NEAR(out[(f * 2) + 0], RampAt(f), 1e-6f) << "left at frame " << f;
        EXPECT_NEAR(out[(f * 2) + 1], RampAt(f), 1e-6f) << "right at frame " << f;
    }
    mixer.Shutdown();
}

TEST(MixedAudioSrcTest, MixedAudioSrc_TwoSourcesMismatchedPeriods_AlignedNoDiscardNoInsert) {
    // Two sources deliver the same total number of frames of two independent
    // ramps, but split on different packet boundaries (512+448 vs 480+480). The
    // mixer must align them sample-for-sample: surplus from the longer packet is
    // buffered (not discarded) and the shorter side is not silence-padded, so the
    // output is exactly 0.5*A + 0.5*B for every one of the 960 frames.
    auto ramp_a = [](uint32_t f) { return static_cast<float>(f % 1000) * (1.0f / 4000.0f); };
    auto ramp_b = [](uint32_t f) { return -static_cast<float>(f % 700) * (1.0f / 4000.0f); };

    auto* a = new QueueMockSource();
    a->EnqueueRamp(512, 0, ramp_a);
    a->EnqueueRamp(448, 512, ramp_a);
    auto* b = new QueueMockSource();
    b->EnqueueRamp(480, 0, ramp_b);
    b->EnqueueRamp(480, 480, ramp_b);

    std::vector<std::unique_ptr<IAudioCaptureSource>> sources;
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(a));
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(b));
    MixedAudioSrc mixer(std::move(sources), MakeUnityGains(2));
    std::string err;
    ASSERT_TRUE(mixer.Init(err));

    const std::vector<float> out = DrainMixer(mixer);
    constexpr uint32_t kTotal = 960;
    ASSERT_EQ(out.size(), static_cast<size_t>(kTotal) * 2u);
    for (uint32_t f = 0; f < kTotal; ++f) {
        const float expected = 0.5f * ramp_a(f) + 0.5f * ramp_b(f);
        EXPECT_NEAR(out[(f * 2) + 0], expected, 1e-6f) << "left at frame " << f;
        EXPECT_NEAR(out[(f * 2) + 1], expected, 1e-6f) << "right at frame " << f;
    }
    mixer.Shutdown();
}

TEST(MixedAudioSrcTest, MixedAudioSrc_IdleSourceDoesNotStallActiveSource) {
    // One source streams while the other is idle (a WASAPI loopback endpoint
    // delivers no packets at all while the system is silent). The active source
    // must keep flowing — the idle one contributes silence, it does not gate the
    // mix — and every active frame must survive.
    const uint32_t sizes[] = {480, 512, 448};
    auto* active = new QueueMockSource();
    uint32_t total = 0;
    for (uint32_t s : sizes) {
        active->EnqueueRamp(s, total, RampAt);
        total += s;
    }
    auto* idle = new QueueMockSource(); // never enqueued: always 0 pending

    std::vector<std::unique_ptr<IAudioCaptureSource>> sources;
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(active));
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(idle));
    MixedAudioSrc mixer(std::move(sources), MakeUnityGains(2));
    std::string err;
    ASSERT_TRUE(mixer.Init(err));

    const std::vector<float> out = DrainMixer(mixer);
    ASSERT_EQ(out.size(), static_cast<size_t>(total) * 2u);
    for (uint32_t f = 0; f < total; ++f) {
        // base_gain = 1/2, idle contributes 0 -> output is 0.5 * active.
        EXPECT_NEAR(out[(f * 2) + 0], 0.5f * RampAt(f), 1e-6f) << "frame " << f;
    }
    mixer.Shutdown();
}

// ---------------------------------------------------------------------------
// H-3: single-source pass-through of device timing and the discontinuity gap.
// A gain-wrapped single MIC track is wrapped in MixedAudioSrc; it must still
// surface the honest A/V drift metric and feed its measured gaps.
// ---------------------------------------------------------------------------

class TimingGapMockSource final : public IAudioCaptureSource {
  public:
    void Enqueue(uint32_t frames, uint32_t gap) {
        queue_.push_back(Packet{frames, gap, std::vector<float>(static_cast<size_t>(frames) * 2u, 0.2f)});
    }
    void SetTiming(uint64_t dev, uint64_t qpc) {
        has_timing_ = true;
        dev_ = dev;
        qpc_ = qpc;
    }

    uint32_t PendingFrameCount() override {
        return queue_.empty() ? 0u : queue_.front().frames;
    }
    bool AcquireBuffer(RawAudioBuffer& out_buf, std::string& out_error) override {
        out_error.clear();
        if (queue_.empty() || held_)
            return false;
        held_ = true;
        const Packet& p = queue_.front();
        out_buf = {};
        out_buf.bytes = reinterpret_cast<const uint8_t*>(p.data.data());
        out_buf.num_frames = p.frames;
        out_buf.gap_frames = p.gap;
        return true;
    }
    void ReleaseBuffer() override {
        if (held_) {
            held_ = false;
            queue_.pop_front();
        }
    }
    bool Init(std::string&) override {
        return true;
    }
    uint32_t SampleRate() const override {
        return 48000;
    }
    uint32_t Channels() const override {
        return 2;
    }
    AudioSampleFormat SampleFormat() const override {
        return AudioSampleFormat::Float32;
    }
    const std::string& EndpointName() const override {
        return name_;
    }
    bool LastBufferDeviceTiming(AudioDeviceTiming& out_timing) const override {
        if (!has_timing_)
            return false;
        out_timing.device_position_ns = dev_;
        out_timing.qpc_position_ns = qpc_;
        return true;
    }
    void Shutdown() override {
    }

  private:
    struct Packet {
        uint32_t frames;
        uint32_t gap;
        std::vector<float> data;
    };
    std::deque<Packet> queue_;
    bool held_ = false;
    bool has_timing_ = false;
    uint64_t dev_ = 0;
    uint64_t qpc_ = 0;
    std::string name_{"timing-gap-mock"};
};

TEST(MixedAudioSrcTest, SingleSource_ForwardsDeviceTiming) {
    auto* s = new TimingGapMockSource();
    s->SetTiming(1000, 2000);
    s->Enqueue(480, 0);
    std::vector<std::unique_ptr<IAudioCaptureSource>> sources;
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(s));
    MixedAudioSrc mixer(std::move(sources), MakeUnityGains(1));
    std::string err;
    ASSERT_TRUE(mixer.Init(err));

    RawAudioBuffer buf{};
    ASSERT_TRUE(mixer.AcquireBuffer(buf, err));
    AudioDeviceTiming t{};
    EXPECT_TRUE(mixer.LastBufferDeviceTiming(t));
    EXPECT_EQ(t.device_position_ns, 1000u);
    EXPECT_EQ(t.qpc_position_ns, 2000u);
    mixer.ReleaseBuffer();
    mixer.Shutdown();
}

TEST(MixedAudioSrcTest, TwoSources_NoDeviceTiming) {
    auto* s0 = new TimingGapMockSource();
    auto* s1 = new TimingGapMockSource();
    s0->SetTiming(1000, 2000);
    s1->SetTiming(3000, 4000);
    std::vector<std::unique_ptr<IAudioCaptureSource>> sources;
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(s0));
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(s1));
    MixedAudioSrc mixer(std::move(sources), MakeUnityGains(2));
    std::string err;
    ASSERT_TRUE(mixer.Init(err));

    AudioDeviceTiming t{};
    EXPECT_FALSE(mixer.LastBufferDeviceTiming(t)); // several device clocks -> unavailable
    mixer.Shutdown();
}

TEST(MixedAudioSrcTest, SingleSource_ForwardsGapFrames) {
    auto* s = new TimingGapMockSource();
    s->Enqueue(480, 100); // 100 frames lost before this packet
    std::vector<std::unique_ptr<IAudioCaptureSource>> sources;
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(s));
    MixedAudioSrc mixer(std::move(sources), MakeUnityGains(1));
    std::string err;
    ASSERT_TRUE(mixer.Init(err));

    RawAudioBuffer buf{};
    ASSERT_TRUE(mixer.AcquireBuffer(buf, err));
    EXPECT_EQ(buf.num_frames, 480u);
    EXPECT_EQ(buf.gap_frames, 100u); // 48 kHz -> 48 kHz identity scale
    mixer.ReleaseBuffer();
    mixer.Shutdown();
}

TEST(MixedAudioSrcTest, TwoSources_GapNotForwarded) {
    auto* s0 = new TimingGapMockSource();
    auto* s1 = new TimingGapMockSource();
    s0->Enqueue(480, 100);
    s1->Enqueue(480, 50);
    std::vector<std::unique_ptr<IAudioCaptureSource>> sources;
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(s0));
    sources.push_back(std::unique_ptr<IAudioCaptureSource>(s1));
    MixedAudioSrc mixer(std::move(sources), MakeUnityGains(2));
    std::string err;
    ASSERT_TRUE(mixer.Init(err));

    RawAudioBuffer buf{};
    ASSERT_TRUE(mixer.AcquireBuffer(buf, err));
    EXPECT_EQ(buf.gap_frames, 0u); // merges mix several clocks; no single gap axis
    mixer.ReleaseBuffer();
    mixer.Shutdown();
}

} // namespace
} // namespace recorder_core
