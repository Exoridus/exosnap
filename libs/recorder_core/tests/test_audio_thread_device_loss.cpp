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
#include "pipeline_diagnostics_aggregator.h"
#include "session_internal.h"

#include <cmath>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
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

// Merged inner that delivers `pre` packets, loses its endpoint, refuses Reinit
// until `min_outage` of wall clock has passed, then recovers and delivers
// `post` packets. Records the wall-clock instants of the loss and the recovery
// so a test can compare the produced silence against the real outage window.
class OutageInner : public IAudioCaptureSource {
  public:
    OutageInner(uint32_t pre, std::chrono::milliseconds min_outage, uint32_t post, std::atomic<bool>* stop = nullptr)
        : stop_(stop), pre_left_(pre), post_left_(post), min_outage_(min_outage) {
        data_.assign(static_cast<size_t>(kFrames) * kChannels, 0.1f);
    }

    // Wall-clock outage window, valid after the worker joined.
    std::chrono::steady_clock::time_point lost_at{};
    std::chrono::steady_clock::time_point recovered_at{};
    int reinit_calls = 0;

    bool Init(std::string&) override {
        return true;
    }
    bool Reinit(std::string& e) override {
        ++reinit_calls;
        if (!lost_ || std::chrono::steady_clock::now() - lost_at < min_outage_) {
            e = "endpoint still gone";
            return false;
        }
        recovered_ = true;
        lost_ = false;
        recovered_at = std::chrono::steady_clock::now();
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
            if (!lost_) {
                lost_ = true;
                lost_at = std::chrono::steady_clock::now();
            }
            return 1; // a lost endpoint reports pending so the drain sees the error
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
            err = "inner endpoint invalidated";
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
    std::chrono::milliseconds min_outage_{0};
    bool lost_ = false;
    bool recovered_ = false;
    bool acquired_ = false;
    std::vector<float> data_;
    std::string name_ = "outage-inner";
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

// Every queued audio packet in production order (pre-mux buffer first, then the
// mux queue). PCM makes the payload directly countable: one fed frame is
// exactly `channels * 2` bytes of int16 with no encoder framing.
std::vector<EncodedAudioPacket> GatherAudioPacketsInOrder(SessionState& state) {
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

size_t QueuedAudioPackets(SessionState& state) {
    return GatherAudioPacketsInOrder(state).size();
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

// Concatenated int16 samples of every PCM packet, in order.
std::vector<int16_t> ConcatPcmSamples(const std::vector<EncodedAudioPacket>& packets) {
    std::vector<int16_t> samples;
    for (const auto& pkt : packets) {
        const size_t n = pkt.bytes.size() / sizeof(int16_t);
        const auto* p = reinterpret_cast<const int16_t*>(pkt.bytes.data());
        samples.insert(samples.end(), p, p + n);
    }
    return samples;
}

// Longest run of consecutive all-zero frames — the silence segment.
uint64_t LongestZeroRunFrames(const std::vector<int16_t>& samples, uint32_t channels) {
    uint64_t best = 0;
    uint64_t run = 0;
    for (size_t i = 0; i + channels <= samples.size(); i += channels) {
        bool zero = true;
        for (uint32_t c = 0; c < channels; ++c) {
            if (samples[i + c] != 0) {
                zero = false;
                break;
            }
        }
        if (zero) {
            ++run;
            best = std::max(best, run);
        } else {
            run = 0;
        }
    }
    return best;
}

// The mux thread does not run in these tests; publishing the video codec
// private up front lets the worker's packets go to the bounded mux queue
// instead of piling up against the 600-packet pre-mux limit.
void MarkVideoTrackReady(SessionState& state) {
    std::lock_guard lk(state.premux_mutex);
    state.codec_private.av1_ready = true;
    state.codec_private.h264_ready = true;
    state.codec_private.hevc_ready = true;
}

TEST(AudioThreadDeviceLoss, MergedTrackFullOutageFillsExactSilenceForTheWholeOutage) {
    // Every inner of a merged track loses its endpoint at once. The track must
    // keep producing exact silence on its own timeline for as long as the outage
    // lasts — the same wall-clock silence fill a bare source already gets — so
    // the merged track's total duration still matches wall time and audio does
    // not slide against video.
    auto state_ptr = std::make_shared<SessionState>();
    SessionState& state = *state_ptr;
    state.config.audio_codec = AudioCodec::Pcm; // countable payload: 1 frame == 4 bytes
    state.config.audio_bit_depth = 16;
    state.audio_track_count = 1;
    MarkVideoTrackReady(state);

    constexpr uint32_t kPre = 3;
    constexpr uint32_t kPost = 3;
    std::vector<std::unique_ptr<IAudioCaptureSource>> inners;
    auto a = std::make_unique<OutageInner>(kPre, std::chrono::milliseconds(200), kPost, &state.stop_requested);
    auto b = std::make_unique<OutageInner>(kPre, std::chrono::milliseconds(200), kPost, nullptr);
    OutageInner* a_raw = a.get();
    inners.push_back(std::move(a));
    inners.push_back(std::move(b));
    auto merged = std::make_unique<MixedAudioSrc>(std::move(inners), std::vector<float>{1.0f, 1.0f});
    auto thread = std::make_shared<AudioThread>(state_ptr, std::move(merged), 0);

    thread->Start();
    ASSERT_TRUE(thread->Join(15000));

    EXPECT_FALSE(state.HasFailure());
    EXPECT_TRUE(state.stats.audio_degraded_occurred);
    EXPECT_TRUE(HasEos(state));

    const auto packets = GatherAudioPacketsInOrder(state);
    ASSERT_FALSE(packets.empty());

    // Timestamps stay monotonic across the outage — no jump, no rewind.
    for (size_t i = 1; i < packets.size(); ++i) {
        EXPECT_GE(packets[i].pts_ns, packets[i - 1].pts_ns) << "audio PTS went backwards at packet " << i;
    }

    ASSERT_NE(a_raw->recovered_at.time_since_epoch().count(), 0) << "the inner never recovered";
    const auto outage =
        std::chrono::duration_cast<std::chrono::milliseconds>(a_raw->recovered_at - a_raw->lost_at).count();
    ASSERT_GT(outage, 0);
    const uint64_t outage_frames = static_cast<uint64_t>(outage) * 48ull;

    const auto samples = ConcatPcmSamples(packets);
    const uint64_t total_frames = static_cast<uint64_t>(samples.size()) / 2ull;
    const uint64_t real_frames = static_cast<uint64_t>(kPre + kPost) * 960ull;

    // Track duration == real audio + the outage, within a few ms (the fill is
    // rebased on the first recovered packet, so at most one poll iteration of
    // the outage tail is not filled).
    // Two boundaries are not filled: the tick before the loss is noticed and the
    // tick after the reacquire. Each is bounded by one poll iteration, so the
    // tolerance is a couple of Windows sleep quanta — nowhere near the ~500 ms
    // outage a missing fill would swallow whole.
    constexpr uint64_t kToleranceFrames = 48ull * 60; // 60 ms
    EXPECT_GE(total_frames + kToleranceFrames, real_frames + outage_frames)
        << "merged track lost time during the outage: " << total_frames << " frames for " << real_frames << " real + "
        << outage_frames << " outage frames";
    EXPECT_LE(total_frames, real_frames + outage_frames + kToleranceFrames)
        << "merged track produced more audio than wall time: " << total_frames << " frames";

    // The filled section is exact silence, and it is as long as the outage.
    const uint64_t zero_run = LongestZeroRunFrames(samples, 2);
    EXPECT_GE(zero_run + kToleranceFrames, outage_frames)
        << "silence segment too short: " << zero_run << " frames for a " << outage_frames << "-frame outage";
    EXPECT_LE(zero_run, outage_frames + kToleranceFrames) << "silence segment too long: " << zero_run << " frames";
}

TEST(AudioThreadDeviceLoss, MergedTrackFullOutageKeepsOpusCadenceContinuous) {
    // Same outage on the shipped default codec: the silence is fed through the
    // normal encoder path, so the packet cadence stays exactly one Opus frame
    // (20 ms) across the outage — no stall, no double audio on recovery.
    auto state_ptr = std::make_shared<SessionState>();
    SessionState& state = *state_ptr;
    state.config.audio_codec = AudioCodec::Opus;
    state.audio_track_count = 1;
    MarkVideoTrackReady(state);

    std::vector<std::unique_ptr<IAudioCaptureSource>> inners;
    inners.push_back(std::make_unique<OutageInner>(3, std::chrono::milliseconds(200), 3, &state.stop_requested));
    inners.push_back(std::make_unique<OutageInner>(3, std::chrono::milliseconds(200), 3, nullptr));
    auto merged = std::make_unique<MixedAudioSrc>(std::move(inners), std::vector<float>{1.0f, 1.0f});
    auto thread = std::make_shared<AudioThread>(state_ptr, std::move(merged), 0);

    thread->Start();
    ASSERT_TRUE(thread->Join(15000));

    EXPECT_FALSE(state.HasFailure());
    const auto packets = GatherAudioPacketsInOrder(state);
    // 6 real packets (120 ms) plus at least a 500 ms outage worth of silence.
    ASSERT_GT(packets.size(), 20u);

    constexpr uint64_t kStepNs = 20000000ULL;
    for (size_t i = 1; i < packets.size(); ++i) {
        EXPECT_EQ(packets[i].pts_ns - packets[i - 1].pts_ns, kStepNs)
            << "Opus PTS step broke at packet " << i << " (outage must not stall or skip the timeline)";
    }
}

TEST(AudioThreadDeviceLoss, MergedTrackWithOneSurvivingSourceGetsNoSilenceFill) {
    // Unchanged behaviour for a partial outage: one of two inners dies, the
    // survivor keeps mixing, and the track is driven by that survivor — the
    // clock-driven silence fill must not run and must not pad the timeline.
    auto state_ptr = std::make_shared<SessionState>();
    SessionState& state = *state_ptr;
    state.config.audio_codec = AudioCodec::Pcm;
    state.config.audio_bit_depth = 16;
    state.audio_track_count = 1;
    MarkVideoTrackReady(state);

    constexpr uint32_t kSurvivorPackets = 30;
    std::vector<std::unique_ptr<IAudioCaptureSource>> inners;
    inners.push_back(std::make_unique<SteadySource>(&state.stop_requested, kSurvivorPackets));
    inners.push_back(std::make_unique<DyingInner>(2)); // dies after 2 packets, never recovers
    auto merged = std::make_unique<MixedAudioSrc>(std::move(inners), std::vector<float>{1.0f, 1.0f});
    auto thread = std::make_shared<AudioThread>(state_ptr, std::move(merged), 0);

    thread->Start();
    ASSERT_TRUE(thread->Join(15000));

    EXPECT_FALSE(state.HasFailure());
    EXPECT_TRUE(state.stats.audio_degraded_occurred);
    EXPECT_TRUE(HasEos(state));

    const auto samples = ConcatPcmSamples(GatherAudioPacketsInOrder(state));
    const uint64_t total_frames = static_cast<uint64_t>(samples.size()) / 2ull;
    // Exactly the survivor's frames: the mix is source-driven while any inner
    // still delivers, so no wall-clock silence is added on top.
    EXPECT_EQ(total_frames, static_cast<uint64_t>(kSurvivorPackets) * 960ull);
    // No long silent stretch either — the survivor's audio is never zero.
    EXPECT_LT(LongestZeroRunFrames(samples, 2), 480ull);
}

// A gain-adjusted single source is wrapped in MixedAudioSrc in production, so it
// takes the merged path while still attributing one device clock (its timing is
// forwarded, and the track is clock-slaved). This mock paces its packets on the
// wall clock like a real endpoint, reports device position + QPC with every
// packet, and — like a reacquired WASAPI stream — restarts its device position
// near zero after the outage.
class TimedOutageInner : public IAudioCaptureSource {
  public:
    TimedOutageInner(uint32_t pre, std::chrono::milliseconds min_outage, uint32_t post, std::atomic<bool>* stop)
        : stop_(stop), pre_left_(pre), post_left_(post), min_outage_(min_outage) {
        data_.assign(static_cast<size_t>(kFrames) * kChannels, 0.1f);
    }

    bool Init(std::string&) override {
        next_packet_due_ = std::chrono::steady_clock::now();
        return true;
    }
    bool Reinit(std::string& e) override {
        if (!lost_ || std::chrono::steady_clock::now() - lost_at_ < min_outage_) {
            e = "endpoint still gone";
            return false;
        }
        recovered_ = true;
        lost_ = false;
        device_frames_ = 0; // a reacquired stream restarts its device position
        next_packet_due_ = std::chrono::steady_clock::now();
        return true;
    }
    uint32_t PendingFrameCount() override {
        if (acquired_) {
            return 0;
        }
        if (!recovered_ && pre_left_ == 0) {
            if (!lost_) {
                lost_ = true;
                lost_at_ = std::chrono::steady_clock::now();
            }
            return 1;
        }
        if (pre_left_ == 0 && post_left_ == 0) {
            if (stop_) {
                stop_->store(true);
            }
            return 0;
        }
        // Real-time pacing: one 20 ms packet every 20 ms of wall clock, so the
        // device timeline and the QPC timeline advance together (drift ~ 0).
        if (std::chrono::steady_clock::now() < next_packet_due_) {
            return 0;
        }
        return kFrames;
    }
    bool AcquireBuffer(RawAudioBuffer& out, std::string& err) override {
        out = {};
        if (lost_ && !recovered_) {
            err = "inner endpoint invalidated";
            return false;
        }
        if (pre_left_ == 0 && post_left_ == 0) {
            err.clear();
            return false;
        }
        acquired_ = true;
        last_device_ns_ = recorder_core::DeviceFramesToNs(device_frames_, 48000);
        last_qpc_ns_ = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
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
        device_frames_ += kFrames;
        next_packet_due_ += std::chrono::milliseconds(20);
        if (pre_left_ > 0) {
            --pre_left_;
        } else if (post_left_ > 0) {
            --post_left_;
        }
    }
    bool LastBufferDeviceTiming(recorder_core::AudioDeviceTiming& out) const override {
        out.device_position_ns = last_device_ns_;
        out.qpc_position_ns = last_qpc_ns_;
        return last_qpc_ns_ != 0;
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
    std::chrono::milliseconds min_outage_{0};
    bool lost_ = false;
    bool recovered_ = false;
    bool acquired_ = false;
    uint64_t device_frames_ = 0;
    uint64_t last_device_ns_ = 0;
    uint64_t last_qpc_ns_ = 0;
    std::chrono::steady_clock::time_point lost_at_{};
    std::chrono::steady_clock::time_point next_packet_due_{};
    std::vector<float> data_;
    std::string name_ = "timed-outage-inner";
};

TEST(AudioThreadDeviceLoss, MergedTrackReAlignsClockSlavingAfterAFullOutage) {
    // Clock slaving must not regulate against the outage: it stays frozen while
    // there is no source clock, and the reacquired stream — whose device
    // position restarts near zero — must re-establish the drift baseline
    // instead of being read as a huge drift and pinned to a bogus compensation.
    auto state_ptr = std::make_shared<SessionState>();
    SessionState& state = *state_ptr;
    state.config.audio_codec = AudioCodec::Opus;
    state.config.audio_clock_slaving_enabled = true;
    state.audio_track_count = 1;
    MarkVideoTrackReady(state);

    std::vector<std::unique_ptr<IAudioCaptureSource>> inner;
    inner.push_back(std::make_unique<TimedOutageInner>(10, std::chrono::milliseconds(200), 10, &state.stop_requested));
    // Gain != 1.0 is what wraps a single production source in MixedAudioSrc.
    auto merged = std::make_unique<MixedAudioSrc>(std::move(inner), std::vector<float>{0.5f});
    auto thread = std::make_shared<AudioThread>(state_ptr, std::move(merged), 0);

    thread->Start();
    ASSERT_TRUE(thread->Join(15000));

    EXPECT_FALSE(state.HasFailure());
    EXPECT_TRUE(state.stats.audio_degraded_occurred);

    const auto snapshot = state.diagnostics.BuildSnapshot(std::chrono::steady_clock::now(), state.stats,
                                                          recorder_core::DiagnosticsLifecycle::Completed, 1.0);
    // A stale baseline reports roughly the whole outage as drift (~250 ms here);
    // a re-baselined stream reports only the mock's delivery jitter.
    constexpr double kMaxDriftMs = 100.0;
    EXPECT_LT(std::abs(snapshot.av_drift_raw_ms), kMaxDriftMs)
        << "the reacquired stream was read as drift instead of re-baselined: " << snapshot.av_drift_raw_ms << " ms";
    EXPECT_LT(std::abs(snapshot.av_drift_ms), kMaxDriftMs)
        << "clock slaving kept regulating across the outage: residual " << snapshot.av_drift_ms << " ms";
}

} // namespace
