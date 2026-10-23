#include "exosnap/engine/edit_player_session.h"
#include <exosnap/engine/audio_media_manifest.h>

#include "edit_playback_pacing.h"
#include "exosnap/engine/edit_player_engine.h"
#include "exosnap/engine/wasapi_audio_render.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace exosnap::engine {

struct EditPlayerSession::Impl {
    EditPlayerEngine engine;
    WasapiAudioRenderer audio;
    bool has_audio = false;
    bool playing = false;
    std::function<int64_t()> external_clock;
    std::atomic<float> volume{1.0f};

    // WasapiAudioRenderer::FramesPlayed() (and therefore AudioClockMs())
    // resets to 0 on every Start() -- it measures time-since-resume, not
    // absolute clip position. Frame PTS values from the engine, and the
    // start_us a caller passes to Play(), are both absolute media time. This
    // offset bridges the two: set to the start_us of the current playback
    // run, added back into every clock read in CurrentPositionMs(). Only
    // ever written from the caller's own UI thread (Play(), same as the rest
    // of this class's public API contract), so no extra synchronization is
    // needed to read it from that same method. The decode thread reads it
    // too; Play() writes it before starting that thread and Pause() joins the
    // thread before the next Play() can write it again.
    int64_t playback_start_us = 0;
    int64_t paused_position_us = 0;
    std::chrono::steady_clock::time_point playback_started;
    std::atomic<bool> independent_clock{false};
    std::atomic<bool> presentation_stopped{true};
    std::mutex presentation_mutex;
    std::condition_variable presentation_cv;

    // GPU render path (2026-08-03 design) -- the callback Play()/SeekTo()
    // drive; see SetOnFrameReady's doc comment. Delivered directly from the
    // engine's own decode/seek thread, with no intermediate queue: pacing/
    // drop decisions live in EditPlayerRenderer::PresentFrame instead.
    std::function<void(RawDecodedVideoFrame, uint64_t)> on_frame;
    std::mutex callback_mutex; // guards on_frame against concurrent Set/invoke

    // Scrub seek: only ever one in flight. A new SeekTo() bumps the
    // generation counter; the worker thread checks it before delivering a
    // frame so a superseded seek's result is silently dropped -- the
    // "throttled, live" scrub behavior from the design.
    std::atomic<uint64_t> seek_generation{0};
    std::thread seek_thread;
    std::mutex seek_thread_mutex;
    std::condition_variable seek_cv;
    struct SeekRequest {
        int64_t target_us;
        uint64_t generation;
        uint64_t presentation_generation;
    };
    std::optional<SeekRequest> pending_seek;
    bool seek_stopped = false;
    std::mutex seek_performance_mutex;
    SeekPerformance seek_performance;

    void StopSeek() {
        {
            std::lock_guard lock(seek_thread_mutex);
            seek_stopped = true;
            pending_seek.reset();
            seek_generation.fetch_add(1);
        }
        seek_cv.notify_all();
        if (seek_thread.joinable())
            seek_thread.join();
        seek_stopped = false;
    }

    // Playback clock snapshot in absolute media time, or -1 when there is no
    // clock. Refreshed from the caller's own thread on every presentation
    // tick (CurrentPositionMs()) and read by the engine's video thread to
    // decide whether a decoded frame is still worth wrapping and delivering.
    //
    // A snapshot rather than a direct WasapiAudioRenderer::FramesPlayed() call
    // from the decode thread on purpose: that method documents a
    // single-caller-thread contract (it reads through a COM clock object that
    // Shutdown() releases). One presentation tick of staleness is irrelevant
    // to a "has this frame's time already passed" decision, and a stale
    // snapshot can only ever fail towards delivering a frame that a fresher
    // clock would have discarded -- never the other way round.
    //
    // The audio-clocked presentation hold also reads it, extrapolated from
    // media_clock_sampled_ns. That time is stored before the clock (release)
    // and read after it (acquire), so a reader never pairs a fresh clock with
    // an older sample time, which would release a frame early.
    std::atomic<int64_t> media_clock_us{-1};
    std::atomic<int64_t> media_clock_sampled_ns{0};

    void PublishClock(int64_t position_us) {
        media_clock_sampled_ns.store(SteadyNowNs(), std::memory_order_relaxed);
        media_clock_us.store(position_us, std::memory_order_release);
    }

    static int64_t SteadyNowNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // Blocks the decode thread until the audio clock reaches pts_us or
    // presentation stops. Blocking here is the backpressure the demuxer's
    // admission rule already allows for: the audio queue keeps filling.
    void HoldUntilAudioClockReaches(int64_t pts_us) {
        std::unique_lock lock(presentation_mutex);
        while (!presentation_stopped.load()) {
            const int64_t sampled_clock_us = media_clock_us.load(std::memory_order_acquire);
            const int64_t elapsed_us = (SteadyNowNs() - media_clock_sampled_ns.load(std::memory_order_relaxed)) / 1000;
            // Until the device has played anything the clock is not running, and
            // extrapolating it would show the first frames before their sound.
            const int64_t max_extrapolation_us =
                sampled_clock_us > playback_start_us ? kAudioClockMaxExtrapolationUs : 0;
            const int64_t hold_us =
                PresentationHoldUs(pts_us - kPresentationEarlyReleaseUs,
                                   ExtrapolatedClockUs(sampled_clock_us, elapsed_us, max_extrapolation_us));
            if (hold_us == 0)
                return;
            presentation_cv.wait_for(lock, std::chrono::microseconds(std::min(hold_us, kPresentationRecheckUs)),
                                     [this] { return presentation_stopped.load(); });
        }
    }

    // The tick samples the clock once per clip frame, down to 10 fps. The bound
    // must exceed that interval: a frame released only at the next sample is
    // already behind that sample, and the present gate drops it.
    static constexpr int64_t kAudioClockMaxExtrapolationUs = 100'000;
    // The clock only moves when the tick samples it; re-reading it more often
    // than this buys nothing.
    static constexpr int64_t kPresentationRecheckUs = 4'000;
    // A timed wait can wake one scheduler tick (15.6 ms by default) late. A
    // frame released that late is already behind the next clock sample and
    // the present gate drops it, so frames leave up to one tick early instead,
    // well inside the lead lip sync tolerates.
    static constexpr int64_t kPresentationEarlyReleaseUs = 16'000;

    void DeliverFrame(RawDecodedVideoFrame frame, uint64_t generation) {
        std::lock_guard<std::mutex> lock(callback_mutex);
        if (on_frame)
            on_frame(std::move(frame), generation);
    }
};

EditPlayerSession::EditPlayerSession() : impl_(std::make_unique<Impl>()) {
}

EditPlayerSession::~EditPlayerSession() {
    Close();
}

void EditPlayerSession::SetVolume(float volume) {
    impl_->volume.store(std::clamp(volume, 0.0f, 1.0f));
}

bool EditPlayerSession::Open(const std::filesystem::path& path, std::string& out_error,
                             const std::atomic<bool>* cancelled) {
    Close();

    if (!impl_->engine.Open(path, out_error, cancelled))
        return false;

    impl_->has_audio = impl_->engine.HasAudioStream();
    if (impl_->has_audio) {
        std::string audio_err;
        if (!impl_->audio.Init(audio_err)) {
            // Non-fatal: degrade to silent-video, matching the "no audio
            // stream" fallback contract rather than failing the whole open.
            impl_->has_audio = false;
        }
    }
    return true;
}

bool EditPlayerSession::Open(const std::shared_ptr<const MediaReadCapability>& source, std::string& out_error,
                             const std::atomic<bool>* cancelled) {
    Close();
    if (!impl_->engine.Open(source, out_error, cancelled))
        return false;
    impl_->has_audio = impl_->engine.HasAudioStream();
    if (impl_->has_audio) {
        std::string audio_error;
        if (!impl_->audio.Init(audio_error))
            impl_->has_audio = false;
    }
    return true;
}

void EditPlayerSession::Close() {
    Pause();
    impl_->StopSeek();
    impl_->audio.Shutdown();
    impl_->engine.Close();
    impl_->has_audio = false;
    impl_->paused_position_us = 0;
}

bool EditPlayerSession::HasAudioStream() const noexcept {
    return impl_->has_audio;
}

double EditPlayerSession::VideoFrameRate() const noexcept {
    return impl_->engine.VideoFrameRate();
}

void EditPlayerSession::SetOnFrameReady(std::function<void(RawDecodedVideoFrame)> callback) {
    if (!callback) {
        SetOnFrameReadyWithGeneration({});
        return;
    }
    SetOnFrameReadyWithGeneration(
        [callback = std::move(callback)](RawDecodedVideoFrame frame, uint64_t) { callback(std::move(frame)); });
}

void EditPlayerSession::SetOnFrameReadyWithGeneration(std::function<void(RawDecodedVideoFrame, uint64_t)> callback) {
    std::lock_guard<std::mutex> lock(impl_->callback_mutex);
    impl_->on_frame = std::move(callback);
}

AudioMetadataReadResult EditPlayerSession::AudioMetadata() const {
    return impl_->engine.AudioMetadata();
}

bool EditPlayerSession::SetMonitorSelection(MonitorSelectionSnapshot selection, std::string& error,
                                            const std::atomic<bool>* cancelled) {
    if (impl_->playing) {
        error = "Pause playback before changing the monitoring selection.";
        return false;
    }
    return impl_->engine.SetMonitorSelection(std::move(selection), error, cancelled);
}

bool EditPlayerSession::MonitoringConfirmationRequired() const {
    return impl_->engine.AudioMetadata().monitoring_confirmation_required;
}

void EditPlayerSession::Play(int64_t start_us, uint64_t generation) {
    StartPlayback(start_us, generation, {});
}

void EditPlayerSession::PlayWithExternalClock(int64_t start_us, uint64_t generation, std::function<int64_t()> clock) {
    if (clock)
        StartPlayback(start_us, generation, std::move(clock));
}

void EditPlayerSession::StartPlayback(int64_t start_us, uint64_t generation, std::function<int64_t()> clock) {
    if (impl_->playing || !impl_->engine.HasVideoStream() || (!clock && MonitoringConfirmationRequired()))
        return;

    impl_->StopSeek();

    impl_->external_clock = std::move(clock);
    impl_->playing = true;
    impl_->playback_start_us = start_us;
    impl_->paused_position_us = start_us;
    impl_->playback_started = std::chrono::steady_clock::now();
    impl_->independent_clock.store(!impl_->has_audio && !impl_->external_clock);
    impl_->presentation_stopped.store(false);
    // Seed the clock the decode thread reads before it can produce anything:
    // the frames between the preceding keyframe and start_us are already in
    // the past and must not be paid a colour conversion for.
    impl_->PublishClock(start_us);

    if (impl_->has_audio && !impl_->external_clock)
        impl_->audio.Start();

    // GPU render path (see SetOnFrameReady's doc comment): delivered straight
    // to on_frame from the engine's own decode thread, no intermediate queue.
    // The consumer's present gate only drops frames the clock has passed, so
    // this thread holds each frame until it is due; delivering on decode would
    // show video ahead of its sound by the demuxer's read-ahead.
    impl_->engine.StartPlaybackDecode(
        start_us,
        [this, generation](RawDecodedVideoFrame frame) {
            // The engine publishes this before spawning workers. The owner
            // thread may not yet have observed audio setup failure.
            if (!impl_->external_clock && !impl_->engine.PlaybackDeliversAudio()) {
                const auto deadline =
                    impl_->playback_started + std::chrono::microseconds(frame.pts_us - impl_->playback_start_us);
                std::unique_lock lock(impl_->presentation_mutex);
                impl_->presentation_cv.wait_until(lock, deadline,
                                                  [this] { return impl_->presentation_stopped.load(); });
            } else {
                impl_->HoldUntilAudioClockReaches(frame.pts_us);
            }
            if (!impl_->presentation_stopped.load())
                impl_->DeliverFrame(std::move(frame), generation);
        },
        [this](DecodedAudioBlock block) {
            if (!impl_->external_clock && impl_->has_audio && block.interleaved_stereo) {
                const float volume = impl_->volume.load();
                if (volume == 1.0f) {
                    impl_->audio.PushSamples(block.interleaved_stereo->data(), block.frame_count);
                } else {
                    auto samples = *block.interleaved_stereo;
                    for (auto& sample : samples)
                        sample *= volume;
                    impl_->audio.PushSamples(samples.data(), block.frame_count);
                }
            }
        },
        [this]() -> int64_t { return impl_->media_clock_us.load(); },
        impl_->external_clock ? PlaybackAudioMode::Disabled : PlaybackAudioMode::MonitorSelection);

    // The engine builds a playback resampler per audio track inside
    // StartPlaybackDecode, and those can fail on a file whose audio streams
    // opened perfectly well. This class paces video off the audio clock, so
    // believing in audio that never arrives would leave FramesPlayed() at 0
    // forever: the clock would stay pinned to start_us and playback would
    // present as a frozen picture with a stationary playhead. Fall back to
    // the same video-only path a file without an audio stream takes instead.
    //
    // PlaybackDeliversAudio() answers "at least one track", which is exactly
    // the question this needs: one surviving track advances the clock just as
    // well as three, and the blocks arriving here are their mix either way.
    if (!impl_->external_clock && impl_->has_audio && !impl_->engine.PlaybackDeliversAudio()) {
        impl_->has_audio = false;
        impl_->audio.Stop();
        impl_->independent_clock.store(true);
        impl_->PublishClock(start_us);
    }
}

void EditPlayerSession::Pause() {
    if (!impl_->playing)
        return;
    impl_->paused_position_us = CurrentPositionUs();
    impl_->playing = false;
    impl_->presentation_stopped.store(true);
    {
        std::lock_guard lock(impl_->presentation_mutex);
        impl_->presentation_cv.notify_all();
    }
    // Must run BEFORE engine.StopPlaybackDecode(): its join() cannot wake a
    // decode thread parked in a wait that belongs to this class. audio.Stop()
    // wakes a PushSamples() blocked on a full ring (dropping its remaining
    // data instead of inserting it) -- left blocked, it would turn the join
    // into a deadlock, since nothing drains that ring once playback has
    // stopped.
    if (impl_->has_audio)
        impl_->audio.Stop();
    impl_->engine.StopPlaybackDecode();
    impl_->external_clock = {};
    impl_->media_clock_us.store(-1);
}

void EditPlayerSession::SeekTo(int64_t target_us, uint64_t generation) {
    Pause();
    if (!impl_->engine.HasVideoStream())
        return;
    impl_->paused_position_us = std::max<int64_t>(target_us, 0);
    std::lock_guard lock(impl_->seek_thread_mutex);
    const auto seek_generation = impl_->seek_generation.fetch_add(1) + 1;
    impl_->pending_seek = Impl::SeekRequest{impl_->paused_position_us, seek_generation, generation};
    if (!impl_->seek_thread.joinable()) {
        impl_->seek_thread = std::thread([this] {
            std::unique_lock seek_lock(impl_->seek_thread_mutex);
            for (;;) {
                impl_->seek_cv.wait(seek_lock,
                                    [this] { return impl_->seek_stopped || impl_->pending_seek.has_value(); });
                if (impl_->seek_stopped)
                    return;
                const auto request = *impl_->pending_seek;
                impl_->pending_seek.reset();
                seek_lock.unlock();
                const auto started = std::chrono::steady_clock::now();
                auto frame =
                    impl_->engine.ReadFrameAtRaw(request.target_us, VideoReadMode::RandomAccess, request.generation);
                {
                    std::lock_guard performance_lock(impl_->seek_performance_mutex);
                    ++impl_->seek_performance.read_count;
                    impl_->seek_performance.read_ns +=
                        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                            .count();
                }
                seek_lock.lock();
                if (!impl_->seek_stopped && frame && impl_->seek_generation.load() == request.generation)
                    impl_->DeliverFrame(std::move(*frame), request.presentation_generation);
            }
        });
    }
    impl_->seek_cv.notify_one();
}
EditPlayerSession::SeekPerformance EditPlayerSession::SeekPerformanceSnapshot() const {
    std::lock_guard lock(impl_->seek_performance_mutex);
    return impl_->seek_performance;
}
void EditPlayerSession::CancelPendingSeek() {
    impl_->StopSeek();
}
int64_t EditPlayerSession::ClockSnapshotUs() const noexcept {
    return impl_->media_clock_us.load(std::memory_order_relaxed);
}

uint64_t EditPlayerSession::AudioShortfallFrames() const noexcept {
    return impl_->has_audio ? impl_->audio.ShortfallFrames() : 0;
}

int64_t EditPlayerSession::CurrentPositionMs() const noexcept {
    return CurrentPositionUs() / 1000;
}

int64_t EditPlayerSession::CurrentPositionUs() const noexcept {
    if (!impl_->playing)
        return impl_->paused_position_us;
    const int64_t position_us =
        impl_->external_clock ? impl_->external_clock()
        : impl_->independent_clock.load()
            ? impl_->playback_start_us + std::chrono::duration_cast<std::chrono::microseconds>(
                                             std::chrono::steady_clock::now() - impl_->playback_started)
                                             .count()
            : impl_->playback_start_us + [&] {
                  const auto frames = impl_->audio.FramesPlayed();
                  const auto rate = impl_->audio.SampleRate();
                  return rate == 0 ? int64_t{0}
                                   : static_cast<int64_t>(frames / rate * 1'000'000 + frames % rate * 1'000'000 / rate);
              }();
    impl_->PublishClock(position_us);
    return position_us;
}
} // namespace exosnap::engine
