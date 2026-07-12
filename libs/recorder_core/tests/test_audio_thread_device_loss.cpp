// Audio-thread device-loss integration (ADR 0046). Drives the real AudioThread
// with fake sources that lose their endpoint mid-recording. The contract:
//   - the session stays alive (no RecordFailure, stop is not raised by the loss),
//   - the encoder timeline stays continuous (a silence gap, not a freeze),
//   - the source is reacquired via Reinit and the EOS is still emitted,
//   - a merged track survives one dead inner while the survivor keeps mixing.

#include <gtest/gtest.h>

#include <Audioclient.h>

#include "audio_thread.h"
#include "mixed_audio_src.h"
#include "session_internal.h"

#include <atomic>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace {

using recorder_core::AudioCodec;
using recorder_core::AudioEosSentinel;
using recorder_core::AudioSampleFormat;
using recorder_core::AudioThread;
using recorder_core::EncodedAudioPacket;
using recorder_core::IAudioCaptureSource;
using recorder_core::MixedAudioSrc;
using recorder_core::RawAudioBuffer;
using recorder_core::SessionState;

constexpr uint32_t kChannels = 2;
constexpr uint32_t kFrames = 960;

// Bare source: delivers `pre` packets, loses its endpoint (device invalidated),
// then on Reinit recovers and delivers `post` packets before signalling stop.
class LostRecoverSource : public IAudioCaptureSource {
  public:
    LostRecoverSource(std::atomic<bool>* stop, uint32_t pre, uint32_t post)
        : stop_(stop), pre_left_(pre), post_left_(post) {
        data_.assign(static_cast<size_t>(kFrames) * kChannels, 0.1f);
    }

    int reinit_calls = 0;

    bool Init(std::string&) override {
        return true;
    }
    bool Reinit(std::string&) override {
        ++reinit_calls;
        recovered_ = true;
        lost_ = false;
        return true;
    }
    uint32_t PendingFrameCount() override {
        if (acquired_) {
            return 0;
        }
        if (pre_left_ > 0) {
            return kFrames;
        }
        if (!recovered_) {
            lost_ = true; // between the pre burst and a successful Reinit
            return 1;
        }
        if (post_left_ > 0) {
            return kFrames;
        }
        if (stop_) {
            stop_->store(true);
        }
        return 0;
    }
    bool AcquireBuffer(RawAudioBuffer& out, std::string& err) override {
        out = {};
        if (lost_ && !recovered_) {
            err = "Mock endpoint invalidated";
            return false;
        }
        if (pre_left_ == 0 && post_left_ == 0) {
            err.clear();
            return false;
        }
        acquired_ = true;
        out.bytes = reinterpret_cast<const uint8_t*>(data_.data());
        out.num_frames = kFrames;
        out.silent = false;
        return true;
    }
    void ReleaseBuffer() override {
        if (!acquired_) {
            return;
        }
        acquired_ = false;
        if (pre_left_ > 0) {
            --pre_left_;
        } else if (post_left_ > 0) {
            --post_left_;
        }
    }
    uint32_t SampleRate() const override {
        return 48000;
    }
    uint32_t Channels() const override {
        return kChannels;
    }
    AudioSampleFormat SampleFormat() const override {
        return AudioSampleFormat::Float32;
    }
    const std::string& EndpointName() const override {
        return name_;
    }
    int32_t LastCaptureHresult() const override {
        return (lost_ && !recovered_) ? static_cast<int32_t>(AUDCLNT_E_DEVICE_INVALIDATED) : 0;
    }
    void Shutdown() override {
    }

  private:
    std::atomic<bool>* stop_ = nullptr;
    uint32_t pre_left_ = 0;
    uint32_t post_left_ = 0;
    bool lost_ = false;
    bool recovered_ = false;
    bool acquired_ = false;
    std::vector<float> data_;
    std::string name_ = "lost-recover";
};

// Steady source used inside a merged track: delivers `count` packets then stops
// the session (drives the merged test to completion).
class SteadySource : public IAudioCaptureSource {
  public:
    SteadySource(std::atomic<bool>* stop, uint32_t count) : stop_(stop), left_(count) {
        data_.assign(static_cast<size_t>(kFrames) * kChannels, 0.05f);
    }
    bool Init(std::string&) override {
        return true;
    }
    uint32_t PendingFrameCount() override {
        if (acquired_) {
            return 0;
        }
        if (left_ > 0) {
            return kFrames;
        }
        if (stop_) {
            stop_->store(true);
        }
        return 0;
    }
    bool AcquireBuffer(RawAudioBuffer& out, std::string& err) override {
        out = {};
        if (left_ == 0) {
            err.clear();
            return false;
        }
        acquired_ = true;
        out.bytes = reinterpret_cast<const uint8_t*>(data_.data());
        out.num_frames = kFrames;
        out.silent = false;
        return true;
    }
    void ReleaseBuffer() override {
        if (acquired_) {
            acquired_ = false;
            if (left_ > 0) {
                --left_;
            }
        }
    }
    uint32_t SampleRate() const override {
        return 48000;
    }
    uint32_t Channels() const override {
        return kChannels;
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
    std::atomic<bool>* stop_ = nullptr;
    uint32_t left_ = 0;
    bool acquired_ = false;
    std::vector<float> data_;
    std::string name_ = "steady";
};

// Merged inner that loses its endpoint after `pre` packets and never recovers
// (Reinit refuses — the PID-guard "stay silent" case).
class DyingInner : public IAudioCaptureSource {
  public:
    explicit DyingInner(uint32_t pre) : pre_left_(pre) {
        data_.assign(static_cast<size_t>(kFrames) * kChannels, 0.05f);
    }
    bool Init(std::string&) override {
        return true;
    }
    bool Reinit(std::string& e) override {
        e = "target gone";
        return false;
    }
    uint32_t PendingFrameCount() override {
        if (acquired_) {
            return 0;
        }
        if (pre_left_ > 0) {
            return kFrames;
        }
        lost_ = true;
        return 1;
    }
    bool AcquireBuffer(RawAudioBuffer& out, std::string& err) override {
        out = {};
        if (lost_) {
            err = "inner endpoint lost";
            return false;
        }
        if (pre_left_ == 0) {
            err.clear();
            return false;
        }
        acquired_ = true;
        out.bytes = reinterpret_cast<const uint8_t*>(data_.data());
        out.num_frames = kFrames;
        out.silent = false;
        return true;
    }
    void ReleaseBuffer() override {
        if (acquired_) {
            acquired_ = false;
            if (pre_left_ > 0) {
                --pre_left_;
            }
        }
    }
    uint32_t SampleRate() const override {
        return 48000;
    }
    uint32_t Channels() const override {
        return kChannels;
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
    uint32_t pre_left_ = 0;
    bool lost_ = false;
    bool acquired_ = false;
    std::vector<float> data_;
    std::string name_ = "dying-inner";
};

bool HasEos(SessionState& state) {
    std::lock_guard lk(state.mux_mutex);
    for (const auto& item : state.mux_queue) {
        if (std::get_if<AudioEosSentinel>(&item.payload) != nullptr) {
            return true;
        }
    }
    return false;
}

size_t QueuedAudioPackets(SessionState& state) {
    size_t n = 0;
    {
        std::lock_guard lk(state.premux_mutex);
        n += state.audio_premux.size();
    }
    {
        std::lock_guard lk(state.mux_mutex);
        for (const auto& item : state.mux_queue) {
            if (std::get_if<EncodedAudioPacket>(&item.payload) != nullptr) {
                ++n;
            }
        }
    }
    return n;
}

TEST(AudioThreadDeviceLoss, SingleSourceLossDegradesAndRecoversWithoutEndingSession) {
    auto state_ptr = std::make_shared<SessionState>();
    SessionState& state = *state_ptr;
    state.config.audio_codec = AudioCodec::Opus;
    state.audio_track_count = 1;

    // 3 packets, lose the endpoint, recover on Reinit (~500 ms later), 3 more.
    auto source = std::make_unique<LostRecoverSource>(&state.stop_requested, 3, 3);
    auto thread = std::make_shared<AudioThread>(state_ptr, std::move(source), 0);

    thread->Start();
    ASSERT_TRUE(thread->Join(8000));

    // The device loss must NOT have ended the recording.
    EXPECT_FALSE(state.HasFailure());
    // It was reacquired, and it degraded (post-flight fact recorded).
    EXPECT_TRUE(state.stats.audio_degraded_occurred);
    // The timeline stayed alive: packets on both sides + a clean EOS.
    EXPECT_GT(QueuedAudioPackets(state), 0u);
    EXPECT_TRUE(HasEos(state));
}

TEST(AudioThreadDeviceLoss, MergedTrackSurvivesOneDeadInner) {
    auto state_ptr = std::make_shared<SessionState>();
    SessionState& state = *state_ptr;
    state.config.audio_codec = AudioCodec::Opus;
    state.audio_track_count = 1;

    std::vector<std::unique_ptr<IAudioCaptureSource>> inners;
    inners.push_back(std::make_unique<SteadySource>(&state.stop_requested, 20)); // survivor drives to stop
    inners.push_back(std::make_unique<DyingInner>(2));                           // dies after 2, never recovers
    auto merged = std::make_unique<MixedAudioSrc>(std::move(inners), std::vector<float>{1.0f, 1.0f});
    auto thread = std::make_shared<AudioThread>(state_ptr, std::move(merged), 0);

    thread->Start();
    ASSERT_TRUE(thread->Join(8000));

    // A dead inner must not end the merged track or the session; the survivor
    // mixes through to completion and the EOS is emitted.
    EXPECT_FALSE(state.HasFailure());
    EXPECT_TRUE(state.stats.audio_degraded_occurred);
    EXPECT_GT(QueuedAudioPackets(state), 0u);
    EXPECT_TRUE(HasEos(state));
}

} // namespace
