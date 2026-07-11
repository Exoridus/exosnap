// Audio discontinuity gap compensation.
//
// A WASAPI DATA_DISCONTINUITY means frames were lost at the device. Counting
// the event is not enough: every lost frame that is not replaced pulls the
// remaining audio track earlier by that duration — a permanent A/V offset that
// grows with every underrun. These tests pin:
//   1. the pure gap arithmetic (device-position jump -> gap frames, clamped),
//   2. the AudioThread behavior: a packet that reports a gap is preceded by
//      exactly that much synthesized silence, so packet PTS stays continuous.

#include <gtest/gtest.h>

#include "audio_thread.h"
#include "discontinuity_gap.h"
#include "session_internal.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace {

using recorder_core::AudioCodec;
using recorder_core::AudioSampleFormat;
using recorder_core::AudioThread;
using recorder_core::ComputeDiscontinuityGapFrames;
using recorder_core::EncodedAudioPacket;
using recorder_core::IAudioCaptureSource;
using recorder_core::kMaxDiscontinuityGapSeconds;
using recorder_core::RawAudioBuffer;
using recorder_core::ScaleDiscontinuityGapFrames;
using recorder_core::SessionState;

// ---------------------------------------------------------------------------
// Pure gap arithmetic
// ---------------------------------------------------------------------------

TEST(DiscontinuityGap, NoDiscontinuityMeansNoGap) {
    EXPECT_EQ(ComputeDiscontinuityGapFrames(false, true, 960, 1440, 48000), 0u);
}

TEST(DiscontinuityGap, FirstPacketHasNoExpectedPosition) {
    EXPECT_EQ(ComputeDiscontinuityGapFrames(true, false, 0, 1440, 48000), 0u);
}

TEST(DiscontinuityGap, ForwardJumpIsTheGapLength) {
    // Previous packet ended at frame 960; the flagged packet starts at 1440:
    // 480 frames were lost.
    EXPECT_EQ(ComputeDiscontinuityGapFrames(true, true, 960, 1440, 48000), 480u);
}

TEST(DiscontinuityGap, NonForwardPositionIsIgnored) {
    EXPECT_EQ(ComputeDiscontinuityGapFrames(true, true, 960, 960, 48000), 0u);
    EXPECT_EQ(ComputeDiscontinuityGapFrames(true, true, 960, 100, 48000), 0u);
}

TEST(DiscontinuityGap, PathologicalJumpIsClamped) {
    const uint32_t rate = 48000;
    const uint64_t huge_jump = 960ull + static_cast<uint64_t>(rate) * 3600ull; // one hour
    const uint32_t max_frames = static_cast<uint32_t>(kMaxDiscontinuityGapSeconds * rate);
    EXPECT_EQ(ComputeDiscontinuityGapFrames(true, true, 960, huge_jump, rate), max_frames);
}

TEST(DiscontinuityGap, ScaleAcrossSampleRates) {
    EXPECT_EQ(ScaleDiscontinuityGapFrames(0, 96000, 48000), 0u);
    EXPECT_EQ(ScaleDiscontinuityGapFrames(960, 48000, 48000), 960u);
    EXPECT_EQ(ScaleDiscontinuityGapFrames(960, 96000, 48000), 480u);
    EXPECT_EQ(ScaleDiscontinuityGapFrames(441, 44100, 48000), 480u);
}

// ---------------------------------------------------------------------------
// AudioThread gap fill
// ---------------------------------------------------------------------------

// Fake source that delivers a fixed sequence of Float32 packets, one of which
// reports a discontinuity with a known gap length, then requests stop.
class GapMockSource : public IAudioCaptureSource {
  public:
    struct Packet {
        uint32_t frames = 0;
        float value = 0.0f;
        uint32_t gap_frames = 0;
        bool discontinuity = false;
    };

    GapMockSource(std::atomic<bool>* stop_flag, std::vector<Packet> plan) : stop_flag_(stop_flag) {
        for (const auto& p : plan) {
            buffers_.emplace_back(static_cast<size_t>(p.frames) * kChannels, p.value);
            plan_.push_back(p);
        }
    }

    bool Init(std::string&) override {
        initialized_ = true;
        return true;
    }

    uint32_t PendingFrameCount() override {
        if (!initialized_ || acquired_)
            return 0;
        if (next_ < plan_.size())
            return plan_[next_].frames;
        if (stop_flag_)
            stop_flag_->store(true);
        return 0;
    }

    bool AcquireBuffer(RawAudioBuffer& out, std::string&) override {
        if (!initialized_ || acquired_ || next_ >= plan_.size())
            return false;
        acquired_ = true;
        out.bytes = reinterpret_cast<const uint8_t*>(buffers_[next_].data());
        out.num_frames = plan_[next_].frames;
        out.silent = false;
        out.data_discontinuity = plan_[next_].discontinuity;
        out.gap_frames = plan_[next_].gap_frames;
        return true;
    }

    void ReleaseBuffer() override {
        if (!acquired_)
            return;
        acquired_ = false;
        ++next_;
        if (next_ >= plan_.size() && stop_flag_)
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

    static constexpr uint32_t kSampleRate = 48000;
    static constexpr uint32_t kChannels = 2;

  private:
    std::atomic<bool>* stop_flag_ = nullptr;
    bool initialized_ = false;
    bool acquired_ = false;
    size_t next_ = 0;
    std::vector<Packet> plan_;
    std::vector<std::vector<float>> buffers_;
    std::string name_ = "GapMock";
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

// PCM (passthrough encoding, 16-bit, one packet per feed) makes the sample
// timeline directly observable: bytes are the samples, PTS is the running frame
// counter. The plan delivers 960 clean frames, then a packet flagged with a
// 480-frame gap, then 960 more clean frames.
TEST(AudioThreadGapFill, ReportedGapBecomesExactSilenceAndPtsStaysContinuous) {
    auto state_ptr = std::make_shared<SessionState>();
    SessionState& state = *state_ptr;
    state.config.audio_codec = AudioCodec::Pcm;
    state.config.audio_bit_depth = 16;
    state.audio_track_count = 1;

    std::vector<GapMockSource::Packet> plan = {
        {960, 0.25f, 0, false},
        {960, 0.25f, 480, true}, // 480 frames lost before this packet
        {960, 0.25f, 0, false},
    };
    auto source = std::make_unique<GapMockSource>(&state.stop_requested, std::move(plan));
    auto thread = std::make_shared<AudioThread>(state_ptr, std::move(source), 0);
    thread->Start();
    ASSERT_TRUE(thread->Join(5000));

    EXPECT_FALSE(state.HasFailure());

    const auto packets = GatherQueuedAudioPackets(state);

    // Total delivered frames must include the synthesized gap: 960+480+960+960.
    constexpr uint32_t kChannels = GapMockSource::kChannels;
    constexpr size_t kBytesPerFrame = kChannels * sizeof(int16_t);
    size_t total_frames = 0;
    for (const auto& pkt : packets) {
        total_frames += pkt.bytes.size() / kBytesPerFrame;
    }
    EXPECT_EQ(total_frames, 960u + 480u + 960u + 960u);

    // PTS continuity: every packet must start exactly where the previous one
    // ended (PTS delta == previous packet's frame count).
    constexpr uint64_t kRate = GapMockSource::kSampleRate;
    for (size_t i = 1; i < packets.size(); ++i) {
        const uint64_t prev_frames = packets[i - 1].bytes.size() / kBytesPerFrame;
        const uint64_t expected_delta =
            (packets[i - 1].pts_ns + prev_frames * 1000000000ULL / kRate) - packets[i - 1].pts_ns;
        EXPECT_EQ(packets[i].pts_ns - packets[i - 1].pts_ns, expected_delta)
            << "PTS gap/overlap between packet " << (i - 1) << " and " << i;
    }

    // The synthesized packet is pure silence and sits between the flagged
    // packet's predecessor and the flagged packet itself.
    ASSERT_EQ(packets.size(), 4u);
    const auto& silence = packets[1];
    EXPECT_EQ(silence.bytes.size() / kBytesPerFrame, 480u);
    for (uint8_t b : silence.bytes) {
        ASSERT_EQ(b, 0u);
    }
    // And the non-gap packets are NOT silent (guards against zeroing real data).
    bool packet2_has_signal = false;
    for (uint8_t b : packets[2].bytes) {
        if (b != 0) {
            packet2_has_signal = true;
            break;
        }
    }
    EXPECT_TRUE(packet2_has_signal);

    // The diagnostics discontinuity counter is preserved.
    // (One flagged packet -> one counted discontinuity.)
    const auto snapshot =
        state.diagnostics.BuildSnapshot(std::chrono::steady_clock::now(), recorder_core::SessionStats{},
                                        recorder_core::DiagnosticsLifecycle::Completed, 1.0);
    EXPECT_EQ(snapshot.audio.discontinuities, 1u);
}

// A discontinuity with an unknown gap length (gap_frames == 0, e.g. a source
// that cannot measure it) must not synthesize anything.
TEST(AudioThreadGapFill, UnknownGapLengthInsertsNothing) {
    auto state_ptr = std::make_shared<SessionState>();
    SessionState& state = *state_ptr;
    state.config.audio_codec = AudioCodec::Pcm;
    state.config.audio_bit_depth = 16;
    state.audio_track_count = 1;

    std::vector<GapMockSource::Packet> plan = {
        {960, 0.25f, 0, false}, {960, 0.25f, 0, true}, // flagged, but no measurable gap
    };
    auto source = std::make_unique<GapMockSource>(&state.stop_requested, std::move(plan));
    auto thread = std::make_shared<AudioThread>(state_ptr, std::move(source), 0);
    thread->Start();
    ASSERT_TRUE(thread->Join(5000));

    EXPECT_FALSE(state.HasFailure());

    const auto packets = GatherQueuedAudioPackets(state);
    constexpr size_t kBytesPerFrame = GapMockSource::kChannels * sizeof(int16_t);
    size_t total_frames = 0;
    for (const auto& pkt : packets) {
        total_frames += pkt.bytes.size() / kBytesPerFrame;
    }
    EXPECT_EQ(total_frames, 1920u);
    EXPECT_EQ(packets.size(), 2u);
}

} // namespace
