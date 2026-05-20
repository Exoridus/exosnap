#include <gtest/gtest.h>

#include "audio_thread.h"
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
using recorder_core::ErrorPhase;
using recorder_core::IAudioCaptureSource;
using recorder_core::RawAudioBuffer;
using recorder_core::SessionState;

class MockAudioCaptureSource : public IAudioCaptureSource {
  public:
    MockAudioCaptureSource(std::atomic<bool>* stop_requested, size_t packet_count, bool init_ok = true,
                           std::string init_error = "mock init failure")
        : stop_requested_(stop_requested), init_ok_(init_ok), init_error_(std::move(init_error)) {
        packets_.resize(packet_count);
        for (auto& packet : packets_) {
            packet.resize(kFramesPerPacket * kChannels, 0.1f);
        }
    }

    bool Init(std::string& out_error) override {
        if (!init_ok_) {
            out_error = init_error_;
            return false;
        }
        initialized_ = true;
        out_error.clear();
        return true;
    }

    uint32_t PendingFrameCount() override {
        if (!initialized_ || packet_acquired_) {
            return 0;
        }
        if (next_packet_ < packets_.size()) {
            return kFramesPerPacket;
        }
        if (stop_requested_) {
            stop_requested_->store(true);
        }
        return 0;
    }

    bool AcquireBuffer(RawAudioBuffer& out_buf, std::string& out_error) override {
        out_buf = {};
        if (!initialized_) {
            out_error = "MockAudioCaptureSource not initialized";
            return false;
        }
        if (packet_acquired_) {
            out_error = "MockAudioCaptureSource AcquireBuffer while packet is held";
            return false;
        }
        if (next_packet_ >= packets_.size()) {
            out_error.clear();
            return false;
        }

        packet_acquired_ = true;
        out_buf.bytes = reinterpret_cast<const uint8_t*>(packets_[next_packet_].data());
        out_buf.num_frames = kFramesPerPacket;
        out_buf.silent = false;
        out_error.clear();
        return true;
    }

    void ReleaseBuffer() override {
        if (!packet_acquired_) {
            return;
        }
        packet_acquired_ = false;
        ++next_packet_;
        if (next_packet_ >= packets_.size() && stop_requested_) {
            stop_requested_->store(true);
        }
    }

    uint32_t SampleRate() const override {
        return kSampleRate;
    }

    uint32_t Channels() const override {
        return kChannels;
    }

    AudioSampleFormat SampleFormat() const override {
        return AudioSampleFormat::Float32;
    }

    const std::string& EndpointName() const override {
        return endpoint_;
    }

    void Shutdown() override {
    }

  private:
    static constexpr uint32_t kSampleRate = 48000;
    static constexpr uint32_t kChannels = 2;
    static constexpr uint32_t kFramesPerPacket = 960;

    std::atomic<bool>* stop_requested_ = nullptr;
    bool init_ok_ = true;
    std::string init_error_;
    bool initialized_ = false;
    bool packet_acquired_ = false;
    size_t next_packet_ = 0;
    std::vector<std::vector<float>> packets_;
    std::string endpoint_ = "Mock Mic";
};

std::vector<EncodedAudioPacket> GatherQueuedAudioPackets(SessionState& state) {
    std::vector<EncodedAudioPacket> packets;
    {
        std::lock_guard lk(state.premux_mutex);
        for (const auto& pkt : state.audio_premux) {
            packets.push_back(pkt);
        }
    }
    {
        std::lock_guard lk(state.mux_mutex);
        for (const auto& item : state.mux_queue) {
            if (const auto* pkt = std::get_if<EncodedAudioPacket>(&item.payload)) {
                packets.push_back(*pkt);
            }
        }
    }
    return packets;
}

TEST(AudioThreadSourceAgnosticTest, AudioThread_StampsPacketsWithTrackId) {
    SessionState state{};
    state.config.audio_codec = AudioCodec::Opus;
    state.audio_track_count = 2;

    auto source = std::make_unique<MockAudioCaptureSource>(&state.stop_requested, 3);
    AudioThread thread(state, std::move(source), 1);

    thread.Start();
    ASSERT_TRUE(thread.Join(5000));

    EXPECT_FALSE(state.HasFailure());

    const auto packets = GatherQueuedAudioPackets(state);
    ASSERT_FALSE(packets.empty());
    for (const auto& pkt : packets) {
        EXPECT_EQ(pkt.track_id, 1u);
    }
}

TEST(AudioThreadSourceAgnosticTest, AudioThread_SendsEosWithTrackId) {
    SessionState state{};
    state.config.audio_codec = AudioCodec::Opus;
    state.audio_track_count = 2;

    auto source = std::make_unique<MockAudioCaptureSource>(&state.stop_requested, 3);
    AudioThread thread(state, std::move(source), 1);

    thread.Start();
    ASSERT_TRUE(thread.Join(5000));

    bool foundEos = false;
    {
        std::lock_guard lk(state.mux_mutex);
        for (const auto& item : state.mux_queue) {
            if (const auto* eos = std::get_if<AudioEosSentinel>(&item.payload)) {
                foundEos = true;
                EXPECT_EQ(eos->track_id, 1u);
            }
        }
    }

    EXPECT_TRUE(foundEos);
}

TEST(AudioThreadSourceAgnosticTest, AudioThread_SourceInitFailureRecordsFailure) {
    SessionState state{};
    state.config.audio_codec = AudioCodec::Opus;
    state.audio_track_count = 1;

    auto source = std::make_unique<MockAudioCaptureSource>(&state.stop_requested, 0, false, "Mock source init failure");
    AudioThread thread(state, std::move(source), 1);

    thread.Start();
    ASSERT_TRUE(thread.Join(5000));

    EXPECT_TRUE(state.stop_requested.load());
    EXPECT_TRUE(state.HasFailure());

    {
        std::lock_guard lk(state.failure_mutex);
        EXPECT_TRUE(state.failure_recorded);
        EXPECT_EQ(state.failure.error_phase, ErrorPhase::AudioCapture);
        EXPECT_NE(state.failure.error_detail.find("Mock source init failure"), std::string::npos);
    }

    bool foundEos = false;
    {
        std::lock_guard lk(state.mux_mutex);
        for (const auto& item : state.mux_queue) {
            if (std::get_if<AudioEosSentinel>(&item.payload) != nullptr) {
                foundEos = true;
                break;
            }
        }
    }
    EXPECT_FALSE(foundEos);
}

} // namespace
