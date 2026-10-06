// A stalled writer must become a mux failure through the real audio producer.

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>

#include "audio_thread.h"
#include "exosnap/engine/codec_types.h"
#include "exosnap/engine/error_types.h"
#include "exosnap/engine/interfaces/IAudioCaptureSource.h"
#include "session_internal.h"

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using exosnap::engine::AudioCodec;
using exosnap::engine::AudioSampleFormat;
using exosnap::engine::AudioThread;
using exosnap::engine::ErrorPhase;
using exosnap::engine::IAudioCaptureSource;
using exosnap::engine::RawAudioBuffer;
using exosnap::engine::SessionState;

// ---------------------------------------------------------------------------
// Producer-level behavior: AudioThread against a stalled consumer
// ---------------------------------------------------------------------------

// Delivers `packet_count` Float32 packets as fast as the thread drains them.
class FloodMockSource : public IAudioCaptureSource {
  public:
    FloodMockSource(SessionState& state, size_t packet_count) : state_(state) {
        packets_ = packet_count;
        data_.assign(static_cast<size_t>(kFramesPerPacket) * kChannels, 0.1f);
    }

    bool Init(std::string&) override {
        initialized_ = true;
        return true;
    }
    uint32_t PendingFrameCount() override {
        if (!initialized_ || acquired_)
            return 0;
        if (delivered_ < packets_)
            return kFramesPerPacket;
        state_.RequestCleanStop();
        return 0;
    }
    bool AcquireBuffer(RawAudioBuffer& out, std::string&) override {
        if (!initialized_ || acquired_ || delivered_ >= packets_)
            return false;
        acquired_ = true;
        out.bytes = reinterpret_cast<const uint8_t*>(data_.data());
        out.num_frames = kFramesPerPacket;
        out.silent = false;
        return true;
    }
    void ReleaseBuffer() override {
        if (!acquired_)
            return;
        acquired_ = false;
        ++delivered_;
        if (delivered_ >= packets_)
            state_.RequestCleanStop();
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

    static constexpr uint32_t kFramesPerPacket = 960;
    static constexpr uint32_t kChannels = 2;

  private:
    SessionState& state_;
    bool initialized_ = false;
    bool acquired_ = false;
    size_t packets_ = 0;
    size_t delivered_ = 0;
    std::vector<float> data_;
    std::string name_ = "FloodMock";
};

// With codec-private data already ready (bothReady == true) the producer pushes
// straight into mux_queue. Nothing consumes it — exactly the slow-destination
// scenario. The queue must stop at the bound and the session must fail with an
// ErrorPhase::Mux error instead of growing without limit.
TEST(MuxQueueBackpressure, AudioProducerFailsInsteadOfUnboundedGrowth) {
    auto state_ptr = std::make_shared<SessionState>();
    SessionState& state = *state_ptr;
    state.config.audio_codec = AudioCodec::Pcm;
    state.config.audio_bit_depth = 16;
    state.audio_track_count = 1;
    state.mux_queue.Configure({8, 256ull * 1024 * 1024, std::chrono::milliseconds(100)});

    // Video codec private already ready: packets route past the premux phase.
    state.premux.PublishVideo(exosnap::engine::VideoCodec::Av1, {});

    auto source = std::make_unique<FloodMockSource>(state, 100);
    auto thread = std::make_shared<AudioThread>(state_ptr, std::move(source), 0);
    thread->Start();
    ASSERT_TRUE(thread->Join(15000));

    ASSERT_TRUE(state.HasFailure());
    EXPECT_EQ(state.FailureSnapshot()->error_phase, ErrorPhase::Mux);

    // Bounded: at most the limit plus the EOS sentinel the drain still enqueues.
    EXPECT_LE(state.mux_queue.Size(), 8u + 2u);
}

} // namespace
