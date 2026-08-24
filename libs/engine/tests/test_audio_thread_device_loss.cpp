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
#include <thread>
#include <variant>
#include <vector>

namespace {

using exosnap::engine::AudioCodec;
using exosnap::engine::AudioEosSentinel;
using exosnap::engine::AudioSampleFormat;
using exosnap::engine::AudioThread;
using exosnap::engine::EncodedAudioPacket;
using exosnap::engine::IAudioCaptureSource;
using exosnap::engine::MixedAudioSrc;
using exosnap::engine::RawAudioBuffer;
using exosnap::engine::SessionState;

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

// Source that delivers `pre` packets, loses its endpoint, refuses Reinit until
// `min_outage` of wall clock has passed, then recovers and delivers `post`
// packets. Usable bare (it fails its acquire with a device-loss HRESULT, so the
// worker's bare_degraded branch owns it) and as a merged inner. Records the
// wall-clock instants of the loss and the recovery so a test can compare the
// produced silence against the real outage window.
//
// Two knobs model what the default mock cannot:
//   reinit_delay        — a reacquire that takes real time (opening a WASAPI
//                         endpoint is not instant). With the default 0 the
//                         reacquire is O(1) and that part of the outage is
//                         invisible.
//   report_device_timing — attribute a device clock (position + QPC) to every
//                         packet and pace delivery on the wall clock, like a
//                         real endpoint. This is what makes the track
//                         clock-slaved; without it the estimator is never fed.
struct OutageSourceOptions {
    uint32_t pre = 3;
    uint32_t post = 3;
    std::chrono::milliseconds min_outage{200};
    std::chrono::milliseconds reinit_delay{0};
    bool report_device_timing = false;
};

class OutageSource : public IAudioCaptureSource {
  public:
    OutageSource(OutageSourceOptions opts, std::atomic<bool>* stop)
        : stop_(stop), opts_(opts), pre_left_(opts.pre), post_left_(opts.post) {
        data_.assign(static_cast<size_t>(kFrames) * kChannels, 0.1f);
    }

    // Wall-clock outage window, valid after the worker joined. recovered_at is
    // taken after the (optionally slow) reacquire returns, so the window covers
    // the reacquire itself.
    std::chrono::steady_clock::time_point lost_at{};
    std::chrono::steady_clock::time_point recovered_at{};

    bool Init(std::string&) override {
        next_packet_due_ = std::chrono::steady_clock::now();
        return true;
    }
    bool Reinit(std::string& e) override {
        if (!lost_ || std::chrono::steady_clock::now() - lost_at < opts_.min_outage) {
            e = "endpoint still gone";
            return false;
        }
        if (opts_.reinit_delay.count() > 0) {
            std::this_thread::sleep_for(opts_.reinit_delay);
        }
        recovered_ = true;
        lost_ = false;
        device_frames_ = 0; // a reacquired stream restarts its device position
        next_packet_due_ = std::chrono::steady_clock::now();
        recovered_at = std::chrono::steady_clock::now();
        return true;
    }
    uint32_t PendingFrameCount() override {
        if (acquired_) {
            return 0;
        }
        if (!recovered_ && pre_left_ == 0) {
            if (!lost_) {
                lost_ = true;
                lost_at = std::chrono::steady_clock::now();
            }
            return 1; // a lost endpoint reports pending so the drain sees the error
        }
        if (pre_left_ == 0 && post_left_ == 0) {
            if (stop_) {
                stop_->store(true);
            }
            return 0;
        }
        // Real-time pacing: one 20 ms packet every 20 ms of wall clock, so the
        // device timeline and the QPC timeline advance together (drift ~ 0).
        if (opts_.report_device_timing && std::chrono::steady_clock::now() < next_packet_due_) {
            return 0;
        }
        return kFrames;
    }
    bool AcquireBuffer(RawAudioBuffer& out, std::string& err) override {
        out = {};
        if (lost_ && !recovered_) {
            err = "endpoint invalidated";
            return false;
        }
        if (pre_left_ == 0 && post_left_ == 0) {
            err.clear();
            return false;
        }
        acquired_ = true;
        if (opts_.report_device_timing) {
            last_device_ns_ = exosnap::engine::DeviceFramesToNs(device_frames_, 48000);
            last_qpc_ns_ = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                     std::chrono::steady_clock::now().time_since_epoch())
                                                     .count());
        }
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
    bool LastBufferDeviceTiming(exosnap::engine::AudioDeviceTiming& out) const override {
        if (!opts_.report_device_timing || last_qpc_ns_ == 0) {
            return false;
        }
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
    int32_t LastCaptureHresult() const override {
        return (lost_ && !recovered_) ? static_cast<int32_t>(AUDCLNT_E_DEVICE_INVALIDATED) : 0;
    }
    void Shutdown() override {
    }

  private:
    std::atomic<bool>* stop_ = nullptr;
    OutageSourceOptions opts_;
    uint32_t pre_left_ = 0;
    uint32_t post_left_ = 0;
    bool lost_ = false;
    bool recovered_ = false;
    bool acquired_ = false;
    uint64_t device_frames_ = 0;
    uint64_t last_device_ns_ = 0;
    uint64_t last_qpc_ns_ = 0;
    std::chrono::steady_clock::time_point next_packet_due_{};
    std::vector<float> data_;
    std::string name_ = "outage-source";
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

// One recording driven through a full outage: every source of the track loses
// its endpoint at once and comes back after `reinit_delay` of reacquire work.
// Returns what landed on the track plus the real wall-clock outage window —
// from the first source going down to the last one being back — which is what
// the produced silence has to match.
struct FullOutageRun {
    std::vector<EncodedAudioPacket> packets;
    uint64_t outage_frames = 0;
    uint64_t real_frames = 0;
    bool failed = false;
    bool degraded_occurred = false;
    bool has_eos = false;
};

FullOutageRun RunFullOutage(AudioCodec codec, bool merged, std::chrono::milliseconds reinit_delay) {
    auto state_ptr = std::make_shared<SessionState>();
    SessionState& state = *state_ptr;
    state.config.audio_codec = codec;
    state.config.audio_bit_depth = 16;
    state.audio_track_count = 1;
    MarkVideoTrackReady(state);

    OutageSourceOptions opts;
    opts.pre = 3;
    opts.post = 3;
    opts.min_outage = std::chrono::milliseconds(200); // recovers on the first 500 ms poll
    opts.reinit_delay = reinit_delay;

    std::vector<OutageSource*> sources;
    auto first = std::make_unique<OutageSource>(opts, &state.stop_requested);
    sources.push_back(first.get());

    std::unique_ptr<IAudioCaptureSource> track_source;
    if (merged) {
        auto second = std::make_unique<OutageSource>(opts, nullptr);
        sources.push_back(second.get());
        std::vector<std::unique_ptr<IAudioCaptureSource>> inners;
        inners.push_back(std::move(first));
        inners.push_back(std::move(second));
        track_source = std::make_unique<MixedAudioSrc>(std::move(inners), std::vector<float>{1.0f, 1.0f});
    } else {
        track_source = std::move(first);
    }

    auto thread = std::make_shared<AudioThread>(state_ptr, std::move(track_source), 0);
    thread->Start();
    EXPECT_TRUE(thread->Join(15000));

    FullOutageRun run;
    run.failed = state.HasFailure();
    run.degraded_occurred = state.stats.audio_degraded_occurred;
    run.has_eos = HasEos(state);
    run.packets = GatherAudioPacketsInOrder(state);
    run.real_frames = static_cast<uint64_t>(opts.pre + opts.post) * kFrames;

    // The track is silent from the first source going down until the last one is
    // back — a merged track only resumes once the whole Reinit pass returned.
    auto lost = sources.front()->lost_at;
    auto back = sources.front()->recovered_at;
    for (const OutageSource* s : sources) {
        EXPECT_NE(s->recovered_at.time_since_epoch().count(), 0) << "a source never recovered";
        lost = std::min(lost, s->lost_at);
        back = std::max(back, s->recovered_at);
    }
    const auto outage_ms = std::chrono::duration_cast<std::chrono::milliseconds>(back - lost).count();
    EXPECT_GT(outage_ms, 0);
    run.outage_frames = static_cast<uint64_t>(outage_ms) * 48ull;
    return run;
}

// Two boundaries of the outage are inherently unfilled: the tick before the loss
// is noticed and the tick after the reacquire is noticed. Each is bounded by one
// poll iteration, so the tolerance is a couple of Windows sleep quanta — nowhere
// near the outage a missing fill would swallow whole.
constexpr uint64_t kOutageToleranceFrames = 48ull * 60; // 60 ms

TEST(AudioThreadDeviceLoss, MergedTrackFullOutageFillsExactSilenceForTheWholeOutage) {
    // Every inner of a merged track loses its endpoint at once. The track must
    // keep producing exact silence on its own timeline for as long as the outage
    // lasts — the same wall-clock silence fill a bare source already gets — so
    // the merged track's total duration still matches wall time and audio does
    // not slide against video.
    const FullOutageRun run = RunFullOutage(AudioCodec::Pcm, /*merged=*/true, std::chrono::milliseconds(0));

    EXPECT_FALSE(run.failed);
    EXPECT_TRUE(run.degraded_occurred);
    EXPECT_TRUE(run.has_eos);
    ASSERT_FALSE(run.packets.empty());

    // Timestamps stay monotonic across the outage — no jump, no rewind.
    for (size_t i = 1; i < run.packets.size(); ++i) {
        EXPECT_GE(run.packets[i].pts_ns, run.packets[i - 1].pts_ns) << "audio PTS went backwards at packet " << i;
    }

    const auto samples = ConcatPcmSamples(run.packets);
    const uint64_t total_frames = static_cast<uint64_t>(samples.size()) / 2ull;
    const uint64_t expected = run.real_frames + run.outage_frames;

    EXPECT_GE(total_frames + kOutageToleranceFrames, expected)
        << "merged track lost time during the outage: " << total_frames << " frames for " << run.real_frames
        << " real + " << run.outage_frames << " outage frames";
    EXPECT_LE(total_frames, expected + kOutageToleranceFrames)
        << "merged track produced more audio than wall time: " << total_frames << " frames";

    // The filled section is exact silence, and it is as long as the outage.
    const uint64_t zero_run = LongestZeroRunFrames(samples, 2);
    EXPECT_GE(zero_run + kOutageToleranceFrames, run.outage_frames)
        << "silence segment too short: " << zero_run << " frames for a " << run.outage_frames << "-frame outage";
    EXPECT_LE(zero_run, run.outage_frames + kOutageToleranceFrames)
        << "silence segment too long: " << zero_run << " frames";
}

TEST(AudioThreadDeviceLoss, MergedTrackOutageIncludesTheReacquireItself) {
    // Reopening an endpoint is not instant, and no audio is captured while it
    // happens — so the reacquire is part of the outage and has to end up in the
    // timeline as silence. Rebasing the silence clock over it instead would make
    // the track short by that much on every outage: a permanent A/V offset.
    const FullOutageRun run = RunFullOutage(AudioCodec::Pcm, /*merged=*/true, std::chrono::milliseconds(100));

    EXPECT_FALSE(run.failed);
    const auto samples = ConcatPcmSamples(run.packets);
    const uint64_t total_frames = static_cast<uint64_t>(samples.size()) / 2ull;
    const uint64_t expected = run.real_frames + run.outage_frames;

    EXPECT_GE(total_frames + kOutageToleranceFrames, expected)
        << "the reacquire was skipped instead of filled: " << total_frames << " frames for " << run.real_frames
        << " real + " << run.outage_frames << " outage frames";
    EXPECT_LE(total_frames, expected + kOutageToleranceFrames);
}

TEST(AudioThreadDeviceLoss, BareSourceOutageIncludesTheReacquireItself) {
    // Same contract on the bare (non-merged) path, which owns its own silence
    // fill and reactivation.
    const FullOutageRun run = RunFullOutage(AudioCodec::Pcm, /*merged=*/false, std::chrono::milliseconds(100));

    EXPECT_FALSE(run.failed);
    EXPECT_TRUE(run.degraded_occurred);
    const auto samples = ConcatPcmSamples(run.packets);
    const uint64_t total_frames = static_cast<uint64_t>(samples.size()) / 2ull;
    const uint64_t expected = run.real_frames + run.outage_frames;

    EXPECT_GE(total_frames + kOutageToleranceFrames, expected)
        << "the reacquire was skipped instead of filled: " << total_frames << " frames for " << run.real_frames
        << " real + " << run.outage_frames << " outage frames";
    EXPECT_LE(total_frames, expected + kOutageToleranceFrames);
}

TEST(AudioThreadDeviceLoss, MergedTrackFullOutageKeepsOpusCadenceContinuous) {
    // Same outage on the shipped default codec: the silence goes through the
    // normal encoder path, so the track ends up with one 20 ms Opus packet per
    // 20 ms of wall clock — real audio and outage alike. A stalled timeline
    // would produce only the 6 real packets.
    const FullOutageRun run = RunFullOutage(AudioCodec::Opus, /*merged=*/true, std::chrono::milliseconds(0));

    EXPECT_FALSE(run.failed);
    constexpr uint64_t kOpusFrame = 960;
    const uint64_t expected_packets = (run.real_frames + run.outage_frames) / kOpusFrame;
    const uint64_t tolerance_packets = kOutageToleranceFrames / kOpusFrame;
    ASSERT_GT(expected_packets, tolerance_packets);
    EXPECT_GE(run.packets.size() + tolerance_packets, expected_packets)
        << "Opus track is short: " << run.packets.size() << " packets, expected about " << expected_packets;
    EXPECT_LE(run.packets.size(), expected_packets + tolerance_packets)
        << "Opus track is long: " << run.packets.size() << " packets, expected about " << expected_packets;
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
    EXPECT_EQ(total_frames, static_cast<uint64_t>(kSurvivorPackets) * kFrames);
    // No long silent stretch either — the survivor's audio is never zero.
    EXPECT_LT(LongestZeroRunFrames(samples, 2), 480ull);
}

TEST(AudioThreadDeviceLoss, MergedTrackReAlignsClockSlavingAfterAFullOutage) {
    // A gain-adjusted single source is wrapped in MixedAudioSrc in production, so
    // it takes the merged path while still attributing one device clock — the
    // merged shape that is actually clock-slaved. Slaving must not regulate
    // against the outage: it stays frozen while there is no source clock, and the
    // reacquired stream — whose device position restarts near zero — must
    // re-establish the drift baseline instead of being read as a huge drift and
    // pinned to a bogus compensation.
    auto state_ptr = std::make_shared<SessionState>();
    SessionState& state = *state_ptr;
    state.config.audio_codec = AudioCodec::Opus;
    state.config.audio_clock_slaving_enabled = true;
    state.audio_track_count = 1;
    MarkVideoTrackReady(state);

    OutageSourceOptions opts;
    opts.pre = 10;
    opts.post = 10;
    opts.min_outage = std::chrono::milliseconds(200);
    opts.report_device_timing = true; // device position + QPC, wall-clock paced

    std::vector<std::unique_ptr<IAudioCaptureSource>> inner;
    inner.push_back(std::make_unique<OutageSource>(opts, &state.stop_requested));
    // Gain != 1.0 is what wraps a single production source in MixedAudioSrc.
    auto merged = std::make_unique<MixedAudioSrc>(std::move(inner), std::vector<float>{0.5f});
    auto thread = std::make_shared<AudioThread>(state_ptr, std::move(merged), 0);

    thread->Start();
    ASSERT_TRUE(thread->Join(15000));

    EXPECT_FALSE(state.HasFailure());
    EXPECT_TRUE(state.stats.audio_degraded_occurred);

    const auto snapshot = state.diagnostics.BuildSnapshot(std::chrono::steady_clock::now(), state.stats,
                                                          exosnap::engine::DiagnosticsLifecycle::Completed, 1.0);
    // A stale baseline reports roughly the whole outage as drift (~250 ms here);
    // a re-baselined stream reports only the mock's delivery jitter.
    constexpr double kMaxDriftMs = 100.0;
    EXPECT_LT(std::abs(snapshot.av_drift_raw_ms), kMaxDriftMs)
        << "the reacquired stream was read as drift instead of re-baselined: " << snapshot.av_drift_raw_ms << " ms";
    EXPECT_LT(std::abs(snapshot.av_drift_ms), kMaxDriftMs)
        << "clock slaving kept regulating across the outage: residual " << snapshot.av_drift_ms << " ms";
}

} // namespace
