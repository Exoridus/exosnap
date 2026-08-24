// Audio-thread silent-stall integration: a capture source that is perfectly
// healthy but stops delivering packets (WASAPI loopback while nothing is
// playing) must not freeze the encoder timeline. The contract:
//   - the quiet stretch lands on the track as silence, so the track's total
//     length still matches wall time and everything after the quiet keeps its
//     real position instead of sliding earlier,
//   - a source that simply runs faster than real time is NOT padded,
//   - a discontinuity gap covering a stretch the wall clock already filled is
//     not filled a second time,
//   - the track's measured zero point is published even when the recording
//     opens on silence, and is never walked back over silence the timeline
//     does not actually contain.
//
// PCM makes the payload directly countable: one fed frame is exactly
// `channels * 2` bytes of int16 with no encoder framing.

#include <gtest/gtest.h>

#include "audio_thread.h"
#include "session_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <windows.h>

namespace {

// QPC now in nanoseconds -- the axis WASAPI reports its capture timestamps on
// and the one AudioThread publishes its measured epoch against.
uint64_t QpcNowNs() {
    LARGE_INTEGER freq{};
    LARGE_INTEGER counter{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    const auto c = static_cast<uint64_t>(counter.QuadPart);
    const auto f = static_cast<uint64_t>(freq.QuadPart);
    return (c / f) * 1000000000ULL + ((c % f) * 1000000000ULL) / f;
}

using exosnap::engine::AudioCodec;
using exosnap::engine::AudioEosSentinel;
using exosnap::engine::AudioSampleFormat;
using exosnap::engine::AudioThread;
using exosnap::engine::EncodedAudioPacket;
using exosnap::engine::IAudioCaptureSource;
using exosnap::engine::RawAudioBuffer;
using exosnap::engine::SessionState;

constexpr uint32_t kChannels = 2;
constexpr uint32_t kFrames = 960; // 20 ms at 48 kHz

// Delivers `pre` packets, then goes completely quiet for `quiet` of wall clock
// (reporting zero pending frames and never failing an acquire -- the endpoint
// is healthy, it just has nothing to hand over), then delivers `post` packets
// and stops the session. Optionally flags the resuming packet with a
// device-position gap covering the whole quiet stretch, the way WASAPI can
// report the same outage a second time.
struct QuietSourceOptions {
    uint32_t pre = 3;
    uint32_t post = 3;
    std::chrono::milliseconds quiet{800};
    bool report_gap_on_resume = false;
};

class GoesQuietSource : public IAudioCaptureSource {
  public:
    GoesQuietSource(QuietSourceOptions opts, std::atomic<bool>* stop)
        : stop_(stop), opts_(opts), pre_left_(opts.pre), post_left_(opts.post) {
        data_.assign(static_cast<size_t>(kFrames) * kChannels, 0.25f);
    }

    // Wall-clock quiet window, valid after the worker joined.
    std::chrono::steady_clock::time_point quiet_started{};
    std::chrono::steady_clock::time_point quiet_ended{};

    bool Init(std::string&) override {
        return true;
    }
    uint32_t PendingFrameCount() override {
        if (acquired_)
            return 0;
        if (pre_left_ > 0)
            return kFrames;
        if (!quiet_done_) {
            if (quiet_started.time_since_epoch().count() == 0)
                quiet_started = std::chrono::steady_clock::now();
            if (std::chrono::steady_clock::now() - quiet_started < opts_.quiet)
                return 0; // healthy, just nothing to deliver
            quiet_done_ = true;
            quiet_ended = std::chrono::steady_clock::now();
        }
        if (post_left_ > 0)
            return kFrames;
        if (stop_)
            stop_->store(true);
        return 0;
    }
    bool AcquireBuffer(RawAudioBuffer& out, std::string& err) override {
        out = {};
        err.clear();
        if (pre_left_ == 0 && !quiet_done_)
            return false; // benign: no data this tick
        if (pre_left_ == 0 && post_left_ == 0)
            return false;
        acquired_ = true;
        // Device timing, the way a real WASAPI endpoint attributes it: the QPC
        // instant this packet's first frame was recorded at, plus the running
        // device position.
        last_device_ns_ = (delivered_frames_ * 1000000000ULL) / 48000ULL;
        last_qpc_ns_ = QpcNowNs();
        out.bytes = reinterpret_cast<const uint8_t*>(data_.data());
        out.num_frames = kFrames;
        out.silent = false;
        if (opts_.report_gap_on_resume && pre_left_ == 0 && !gap_reported_) {
            gap_reported_ = true;
            out.data_discontinuity = true;
            const auto quiet_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(quiet_ended - quiet_started).count();
            out.gap_frames = static_cast<uint32_t>(quiet_ms * 48);
        }
        return true;
    }
    void ReleaseBuffer() override {
        if (!acquired_)
            return;
        acquired_ = false;
        delivered_frames_ += kFrames;
        if (pre_left_ > 0)
            --pre_left_;
        else if (post_left_ > 0)
            --post_left_;
    }
    bool LastBufferDeviceTiming(exosnap::engine::AudioDeviceTiming& out) const override {
        if (last_qpc_ns_ == 0)
            return false;
        out.device_position_ns = last_device_ns_;
        out.qpc_position_ns = last_qpc_ns_;
        return true;
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
    QuietSourceOptions opts_;
    uint32_t pre_left_ = 0;
    uint32_t post_left_ = 0;
    bool acquired_ = false;
    bool quiet_done_ = false;
    bool gap_reported_ = false;
    uint64_t delivered_frames_ = 0;
    uint64_t last_device_ns_ = 0;
    uint64_t last_qpc_ns_ = 0;
    std::vector<float> data_;
    std::string name_ = "goes-quiet";
};

// A source that delivers `count` packets as fast as the worker asks, far
// faster than real time. Nothing may be padded onto such a track.
class FastSource : public IAudioCaptureSource {
  public:
    FastSource(std::atomic<bool>* stop, uint32_t count) : stop_(stop), left_(count) {
        data_.assign(static_cast<size_t>(kFrames) * kChannels, 0.1f);
    }
    bool Init(std::string&) override {
        return true;
    }
    uint32_t PendingFrameCount() override {
        if (acquired_)
            return 0;
        if (left_ > 0)
            return kFrames;
        if (stop_)
            stop_->store(true);
        return 0;
    }
    bool AcquireBuffer(RawAudioBuffer& out, std::string& err) override {
        out = {};
        err.clear();
        if (left_ == 0)
            return false;
        acquired_ = true;
        out.bytes = reinterpret_cast<const uint8_t*>(data_.data());
        out.num_frames = kFrames;
        out.silent = false;
        return true;
    }
    void ReleaseBuffer() override {
        if (acquired_) {
            acquired_ = false;
            if (left_ > 0)
                --left_;
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
    std::string name_ = "fast";
};

void MarkVideoTrackReady(SessionState& state) {
    std::lock_guard lk(state.premux_mutex);
    state.codec_private.av1_ready = true;
    state.codec_private.h264_ready = true;
    state.codec_private.hevc_ready = true;
}

std::vector<EncodedAudioPacket> GatherAudioPacketsInOrder(SessionState& state) {
    std::vector<EncodedAudioPacket> packets;
    {
        std::lock_guard lk(state.premux_mutex);
        for (const auto& pkt : state.audio_premux)
            packets.push_back(pkt);
    }
    {
        std::lock_guard lk(state.mux_mutex);
        for (const auto& item : state.mux_queue) {
            if (const auto* pkt = std::get_if<EncodedAudioPacket>(&item.payload))
                packets.push_back(*pkt);
        }
    }
    return packets;
}

uint64_t TotalPcmFrames(const std::vector<EncodedAudioPacket>& packets) {
    uint64_t bytes = 0;
    for (const auto& pkt : packets)
        bytes += pkt.bytes.size();
    return bytes / (sizeof(int16_t) * kChannels);
}

bool HasEos(SessionState& state) {
    std::lock_guard lk(state.mux_mutex);
    for (const auto& item : state.mux_queue) {
        if (std::get_if<AudioEosSentinel>(&item.payload) != nullptr)
            return true;
    }
    return false;
}

struct QuietRun {
    std::vector<EncodedAudioPacket> packets;
    uint64_t real_frames = 0;
    uint64_t quiet_frames = 0;
    bool failed = false;
    bool has_eos = false;
    // Measured zero point the worker published for track 0 (100 ns QPC units;
    // 0 == never published) plus the wall-clock window the run occupied, which
    // is what any honest epoch has to fall inside.
    uint64_t audio_epoch_100ns = 0;
    uint64_t qpc_before_start_100ns = 0;
    uint64_t qpc_after_join_100ns = 0;
    // The epoch as it stood the moment this track's FIRST packet became visible
    // to the muxer. The muxer resolves a track's placement from that packet and
    // never revisits it, so a measurement published later is a measurement
    // thrown away. 0 == the first packet went out with no zero point behind it.
    uint64_t epoch_at_first_packet_100ns = 0;
    bool saw_first_packet = false;
};

QuietRun RunQuiet(QuietSourceOptions opts) {
    auto state_ptr = std::make_shared<SessionState>();
    SessionState& state = *state_ptr;
    state.config.audio_codec = AudioCodec::Pcm;
    state.config.audio_bit_depth = 16;
    state.audio_track_count = 1;
    MarkVideoTrackReady(state);

    auto source = std::make_unique<GoesQuietSource>(opts, &state.stop_requested);
    GoesQuietSource* raw_source = source.get();
    auto thread = std::make_shared<AudioThread>(state_ptr, std::move(source), 0);

    QuietRun run;
    run.qpc_before_start_100ns = QpcNowNs() / 100;

    // Stand in for the muxer's ordering: it latches a track's placement from
    // the first packet it can see, so sample the published epoch at exactly
    // that instant. The worker publishes before it routes, so a watcher can
    // never catch a packet with no epoch behind it — unless the ordering
    // regresses, in which case the gap is the whole detection threshold wide.
    std::atomic<bool> watching{true};
    std::atomic<uint64_t> epoch_at_first{0};
    std::atomic<bool> saw_first{false};
    std::thread watcher([&] {
        while (watching.load()) {
            bool any = false;
            {
                std::lock_guard lk(state.premux_mutex);
                any = !state.audio_premux.empty();
            }
            if (!any) {
                std::lock_guard lk(state.mux_mutex);
                for (const auto& item : state.mux_queue) {
                    if (std::get_if<EncodedAudioPacket>(&item.payload) != nullptr) {
                        any = true;
                        break;
                    }
                }
            }
            if (any) {
                epoch_at_first.store(state.audio_epoch_qpc_100ns[0].load());
                saw_first.store(true);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    thread->Start();
    EXPECT_TRUE(thread->Join(20000));
    watching.store(false);
    watcher.join();
    run.qpc_after_join_100ns = QpcNowNs() / 100;
    run.audio_epoch_100ns = state.audio_epoch_qpc_100ns[0].load();
    run.epoch_at_first_packet_100ns = epoch_at_first.load();
    run.saw_first_packet = saw_first.load();

    run.failed = state.HasFailure();
    run.has_eos = HasEos(state);
    run.packets = GatherAudioPacketsInOrder(state);
    run.real_frames = static_cast<uint64_t>(opts.pre + opts.post) * kFrames;
    const auto quiet_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(raw_source->quiet_ended - raw_source->quiet_started)
            .count();
    EXPECT_GT(quiet_ms, 0);
    run.quiet_frames = static_cast<uint64_t>(quiet_ms) * 48ull;
    return run;
}

// The stall is only noticed after the detection threshold, and the resume is
// noticed one poll late; both are bounded by a couple of poll iterations.
constexpr uint64_t kToleranceFrames = 48ull * 80; // 80 ms

TEST(AudioThreadSilenceFill, SoloSourceThatGoesQuietKeepsItsTimeline) {
    QuietSourceOptions opts;
    opts.quiet = std::chrono::milliseconds(900);
    const QuietRun run = RunQuiet(opts);

    EXPECT_FALSE(run.failed);
    EXPECT_TRUE(run.has_eos);
    ASSERT_FALSE(run.packets.empty());

    for (size_t i = 1; i < run.packets.size(); ++i)
        EXPECT_GE(run.packets[i].pts_ns, run.packets[i - 1].pts_ns) << "audio PTS went backwards at packet " << i;

    const uint64_t total_frames = TotalPcmFrames(run.packets);
    const uint64_t expected = run.real_frames + run.quiet_frames;
    EXPECT_GE(total_frames + kToleranceFrames, expected)
        << "the quiet stretch was lost: " << total_frames << " frames for " << run.real_frames << " real + "
        << run.quiet_frames << " quiet frames -- everything after the silence is stamped too early";
    EXPECT_LE(total_frames, expected + kToleranceFrames) << "more audio was produced than wall time: " << total_frames;
}

TEST(AudioThreadSilenceFill, QuietStretchIsNotFilledTwiceWhenTheDeviceAlsoReportsIt) {
    // The resuming packet carries a DATA_DISCONTINUITY gap covering the same
    // stretch the wall clock already filled. Honouring both would make the
    // track twice as long as wall time and push it permanently late.
    QuietSourceOptions opts;
    opts.quiet = std::chrono::milliseconds(900);
    opts.report_gap_on_resume = true;
    const QuietRun run = RunQuiet(opts);

    EXPECT_FALSE(run.failed);
    const uint64_t total_frames = TotalPcmFrames(run.packets);
    const uint64_t expected = run.real_frames + run.quiet_frames;
    EXPECT_LE(total_frames, expected + kToleranceFrames)
        << "the same outage was filled twice: " << total_frames << " frames for " << expected << " expected";
    EXPECT_GE(total_frames + kToleranceFrames, expected);
}

// 100 ns units per millisecond.
constexpr uint64_t kMs100ns = 10000ULL;

TEST(AudioThreadSilenceFill, RecordingThatOpensOnSilenceStillPublishesItsZeroPoint) {
    // The everyday loopback case: recording starts while nothing is playing, so
    // the track opens with clock-driven silence and no capture packet exists
    // yet. The muxer places a track from its FIRST packet, so a measurement
    // that waits for real audio loses that race and the whole track silently
    // falls back to the assumed start — exactly the offset the measurement is
    // there to remove. The fill itself defines the zero point, so it publishes.
    QuietSourceOptions opts;
    opts.pre = 0; // nothing is playing when the recording starts
    opts.post = 3;
    opts.quiet = std::chrono::milliseconds(700);
    const QuietRun run = RunQuiet(opts);

    EXPECT_FALSE(run.failed);
    ASSERT_NE(run.audio_epoch_100ns, 0u) << "no zero point was published for a track that opened on silence";
    EXPECT_GE(run.audio_epoch_100ns, run.qpc_before_start_100ns) << "the published zero point predates the recording";
    // The timeline starts as soon as the worker does, so the epoch belongs at
    // the very beginning of the run -- not after the quiet stretch.
    EXPECT_LE(run.audio_epoch_100ns, run.qpc_before_start_100ns + (300 * kMs100ns))
        << "the zero point landed after the silence instead of at the start of it";

    // The ordering, which is what actually decides whether the measurement is
    // used at all: the muxer places a track from its first packet, so a zero
    // point published after that packet is a zero point thrown away.
    ASSERT_TRUE(run.saw_first_packet) << "the track produced no packets at all";
    EXPECT_NE(run.epoch_at_first_packet_100ns, 0u)
        << "the first packet reached the muxer before any zero point was published -- "
           "the measurement loses the race and the track falls back to the assumed start";
}

TEST(AudioThreadSilenceFill, ZeroPointStaysInsideTheRunWhenTheDeviceAlsoReportsTheStall) {
    // End-to-end guard on the walk-back: whatever path publishes the zero
    // point, it must land inside the run's own wall-clock window. A walk-back
    // that counted the resuming packet's reported gap -- which is deliberately
    // NOT fed, because the wall clock already covered it -- would put the epoch
    // a whole quiet stretch before the recording began, and the muxer would
    // then trim that much real audio off the head of the track. (The arithmetic
    // itself is pinned by AudioEpochFromPacket.CountsOnlyFramesActuallyFed;
    // both call sites go through that one helper.)
    QuietSourceOptions opts;
    opts.pre = 2;
    opts.post = 3;
    opts.quiet = std::chrono::milliseconds(900);
    opts.report_gap_on_resume = true;
    const QuietRun run = RunQuiet(opts);

    EXPECT_FALSE(run.failed);
    ASSERT_NE(run.audio_epoch_100ns, 0u);
    EXPECT_GE(run.audio_epoch_100ns, run.qpc_before_start_100ns)
        << "the zero point was walked back before the recording even started";
    EXPECT_LE(run.audio_epoch_100ns, run.qpc_after_join_100ns);
}

TEST(AudioThreadSilenceFill, ShortGapsBetweenPacketsAreNotPadded) {
    // A source running far faster than real time must be passed through
    // untouched -- the fill is for genuine stalls, not ordinary cadence.
    auto state_ptr = std::make_shared<SessionState>();
    SessionState& state = *state_ptr;
    state.config.audio_codec = AudioCodec::Pcm;
    state.config.audio_bit_depth = 16;
    state.audio_track_count = 1;
    MarkVideoTrackReady(state);

    constexpr uint32_t kPackets = 40;
    auto thread =
        std::make_shared<AudioThread>(state_ptr, std::make_unique<FastSource>(&state.stop_requested, kPackets), 0);
    thread->Start();
    ASSERT_TRUE(thread->Join(15000));

    EXPECT_FALSE(state.HasFailure());
    EXPECT_EQ(TotalPcmFrames(GatherAudioPacketsInOrder(state)), static_cast<uint64_t>(kPackets) * kFrames);
}

} // namespace
