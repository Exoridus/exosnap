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

// Mock source that delivers kFramesPerPacket-sample chunks and signals stop after all packets.
// Used to verify PTS step size with sub-Opus-frame delivery (like a 10 ms WASAPI period).
class SmallChunkMockSource : public IAudioCaptureSource {
  public:
    SmallChunkMockSource(std::atomic<bool>* stop_flag, uint32_t frames_per_chunk, size_t chunk_count)
        : stop_flag_(stop_flag), frames_per_chunk_(frames_per_chunk) {
        chunks_.resize(chunk_count);
        for (auto& c : chunks_) {
            c.resize(frames_per_chunk * kChannels, 0.05f);
        }
    }

    bool Init(std::string&) override {
        initialized_ = true;
        return true;
    }

    uint32_t PendingFrameCount() override {
        if (!initialized_ || acquired_)
            return 0;
        if (next_ < chunks_.size())
            return frames_per_chunk_;
        if (stop_flag_)
            stop_flag_->store(true);
        return 0;
    }

    bool AcquireBuffer(RawAudioBuffer& out, std::string&) override {
        if (!initialized_ || acquired_ || next_ >= chunks_.size())
            return false;
        acquired_ = true;
        out.bytes = reinterpret_cast<const uint8_t*>(chunks_[next_].data());
        out.num_frames = frames_per_chunk_;
        out.silent = false;
        return true;
    }

    void ReleaseBuffer() override {
        if (!acquired_)
            return;
        acquired_ = false;
        ++next_;
        if (next_ >= chunks_.size() && stop_flag_)
            stop_flag_->store(true);
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
        return name_;
    }
    void Shutdown() override {
    }

  private:
    static constexpr uint32_t kSampleRate = 48000;
    static constexpr uint32_t kChannels = 2;

    std::atomic<bool>* stop_flag_ = nullptr;
    uint32_t frames_per_chunk_ = 480;
    bool initialized_ = false;
    bool acquired_ = false;
    size_t next_ = 0;
    std::vector<std::vector<float>> chunks_;
    std::string name_ = "SmallChunkMock";
};

TEST(AudioThreadSourceAgnosticTest, AudioThread_OpusPtsStepIs20ms_SmallChunks) {
    // Deliver audio in 480-sample chunks (10 ms WASAPI pattern).
    // 10 chunks × 480 samples = 4800 samples → 5 Opus packets.
    // Each Opus packet must have a PTS exactly 20 ms after the previous one.
    SessionState state{};
    state.config.audio_codec = AudioCodec::Opus;
    state.audio_track_count = 1;

    auto source = std::make_unique<SmallChunkMockSource>(&state.stop_requested, 480, 10);
    AudioThread thread(state, std::move(source), 0);
    thread.Start();
    ASSERT_TRUE(thread.Join(5000));

    EXPECT_FALSE(state.HasFailure());

    const auto packets = GatherQueuedAudioPackets(state);
    ASSERT_GE(packets.size(), 5u);

    constexpr uint64_t kStepNs = 20000000ULL; // 20 ms
    for (size_t i = 1; i < packets.size(); ++i) {
        const uint64_t step = packets[i].pts_ns - packets[i - 1].pts_ns;
        EXPECT_EQ(step, kStepNs) << "PTS step between packet " << (i - 1) << " and " << i << " should be 20 ms";
    }
}

// AAC-LC encodes 1024 PCM frames per output packet: 1024 / 48000 = 21.333 ms.
constexpr uint64_t kAacFrameDurNs = 1024ULL * 1000000000ULL / 48000ULL; // 21333333

// Mock source that interleaves real audio chunks with idle periods where
// PendingFrameCount() returns 0, forcing the AudioThread outer loop to Sleep.
// Before AAC-RC-FIX the QPC idle clock advanced the encoder timeline during
// these gaps; after the fix the audio PTS must depend only on delivered frames.
class IdleGapMockSource : public IAudioCaptureSource {
  public:
    IdleGapMockSource(std::atomic<bool>* stop_flag, uint32_t frames_per_chunk, size_t chunk_count,
                      int idle_polls_between_chunks)
        : stop_flag_(stop_flag), frames_per_chunk_(frames_per_chunk), idle_polls_(idle_polls_between_chunks) {
        chunks_.resize(chunk_count);
        for (auto& c : chunks_)
            c.resize(static_cast<size_t>(frames_per_chunk) * kChannels, 0.05f);
    }

    bool Init(std::string&) override {
        initialized_ = true;
        return true;
    }

    uint32_t PendingFrameCount() override {
        if (!initialized_ || acquired_)
            return 0;
        if (next_ >= chunks_.size()) {
            if (stop_flag_)
                stop_flag_->store(true);
            return 0;
        }
        if (idle_remaining_ > 0) {
            --idle_remaining_;
            return 0; // idle gap: no audio available yet
        }
        return frames_per_chunk_;
    }

    bool AcquireBuffer(RawAudioBuffer& out, std::string&) override {
        if (!initialized_ || acquired_ || next_ >= chunks_.size())
            return false;
        acquired_ = true;
        out.bytes = reinterpret_cast<const uint8_t*>(chunks_[next_].data());
        out.num_frames = frames_per_chunk_;
        out.silent = false;
        return true;
    }

    void ReleaseBuffer() override {
        if (!acquired_)
            return;
        acquired_ = false;
        ++next_;
        idle_remaining_ = idle_polls_; // open a fresh idle gap before the next chunk
        if (next_ >= chunks_.size() && stop_flag_)
            stop_flag_->store(true);
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
        return name_;
    }
    void Shutdown() override {
    }

  private:
    static constexpr uint32_t kSampleRate = 48000;
    static constexpr uint32_t kChannels = 2;

    std::atomic<bool>* stop_flag_ = nullptr;
    uint32_t frames_per_chunk_ = 1024;
    int idle_polls_ = 0;
    int idle_remaining_ = 0; // first chunk is delivered immediately
    bool initialized_ = false;
    bool acquired_ = false;
    size_t next_ = 0;
    std::vector<std::vector<float>> chunks_;
    std::string name_ = "IdleGapMock";
};

TEST(AudioThreadSourceAgnosticTest, AudioThread_AacPtsStepMatchesFrameDuration_SmallChunks) {
    // Deliver back-to-back 480-sample chunks through the real MF AAC encoder.
    // 100 chunks x 480 = 48000 frames (~1 s) -> ~46 AAC packets, each spaced by
    // one AAC frame duration (21.333 ms).
    SessionState state{};
    state.config.audio_codec = AudioCodec::AacMf;
    state.audio_track_count = 1;

    auto source = std::make_unique<SmallChunkMockSource>(&state.stop_requested, 480, 100);
    AudioThread thread(state, std::move(source), 0);
    thread.Start();
    ASSERT_TRUE(thread.Join(10000));

    if (state.HasFailure()) {
        GTEST_SKIP() << "MF AAC encoder unavailable on this host";
    }

    const auto packets = GatherQueuedAudioPackets(state);
    if (packets.size() < 5) {
        GTEST_SKIP() << "MF AAC encoder produced too few packets (" << packets.size() << ")";
    }

    for (size_t i = 1; i < packets.size(); ++i) {
        EXPECT_GE(packets[i].pts_ns, packets[i - 1].pts_ns) << "AAC PTS must not go backwards at packet " << i;
    }

    const uint64_t span = packets.back().pts_ns - packets[1].pts_ns;
    const double avgStep = static_cast<double>(span) / static_cast<double>(packets.size() - 2);
    EXPECT_GE(avgStep, kAacFrameDurNs * 0.9) << "Average AAC PTS step collapsed: " << avgStep << " ns";
    EXPECT_LE(avgStep, kAacFrameDurNs * 1.1) << "Average AAC PTS step inflated: " << avgStep << " ns";
}

TEST(AudioThreadSourceAgnosticTest, AudioThread_AacPtsNotInflatedByIdleGaps) {
    // Regression for AAC-RC-FIX: idle gaps between deliveries must not inflate
    // the audio timeline. 16 chunks of exactly one AAC frame, separated by ~25
    // idle polls (~25 ms of real Sleep). Pre-fix the QPC idle clock would push
    // the average step toward ~46 ms; after the fix it must stay ~21.333 ms.
    SessionState state{};
    state.config.audio_codec = AudioCodec::AacMf;
    state.audio_track_count = 1;

    auto source = std::make_unique<IdleGapMockSource>(&state.stop_requested, 1024, 16, 25);
    AudioThread thread(state, std::move(source), 0);
    thread.Start();
    ASSERT_TRUE(thread.Join(15000));

    if (state.HasFailure()) {
        GTEST_SKIP() << "MF AAC encoder unavailable on this host";
    }

    const auto packets = GatherQueuedAudioPackets(state);
    if (packets.size() < 5) {
        GTEST_SKIP() << "MF AAC encoder produced too few packets (" << packets.size() << ")";
    }

    for (size_t i = 1; i < packets.size(); ++i) {
        EXPECT_GE(packets[i].pts_ns, packets[i - 1].pts_ns) << "AAC PTS must not go backwards at packet " << i;
    }

    const uint64_t span = packets.back().pts_ns - packets[1].pts_ns;
    const double avgStep = static_cast<double>(span) / static_cast<double>(packets.size() - 2);
    // The upper bound is the regression guard: idle inflation would roughly
    // double the step. 15% headroom keeps the test stable under the fix.
    EXPECT_LE(avgStep, kAacFrameDurNs * 1.15)
        << "AAC PTS inflated by idle gaps: avg step " << avgStep << " ns (frame dur " << kAacFrameDurNs << " ns)";
    EXPECT_GE(avgStep, kAacFrameDurNs * 0.85) << "AAC PTS step collapsed: " << avgStep << " ns";
}

} // namespace
