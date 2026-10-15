#include "exosnap/engine/edit_player_session.h"
#include <exosnap/engine/audio_media_manifest.h>

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
    std::atomic<float> volume{1.0f};

    // WasapiAudioRenderer::FramesPlayed() (and therefore AudioClockMs())
    // resets to 0 on every Start() -- it measures time-since-resume, not
    // absolute clip position. Frame PTS values from the engine, and the
    // start_us a caller passes to Play(), are both absolute media time. This
    // offset bridges the two: set to the start_us of the current playback
    // run, added back into every clock read in CurrentPositionMs(). Only
    // ever written from the caller's own UI thread (Play(), same as the rest
    // of this class's public API contract), so no extra synchronization is
    // needed to read it from that same method.
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
    std::atomic<int64_t> media_clock_us{-1};

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

bool EditPlayerSession::Open(const std::filesystem::path& path, std::string& out_error) {
    Close();

    if (!impl_->engine.Open(path, out_error))
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

bool EditPlayerSession::SetMonitorSelection(MonitorSelectionSnapshot selection, std::string& error) {
    if (impl_->playing) {
        error = "Pause playback before changing the monitoring selection.";
        return false;
    }
    return impl_->engine.SetMonitorSelection(std::move(selection), error);
}

bool EditPlayerSession::MonitoringConfirmationRequired() const {
    return impl_->engine.AudioMetadata().monitoring_confirmation_required;
}

void EditPlayerSession::Play(int64_t start_us, uint64_t generation) {
    if (impl_->playing || !impl_->engine.HasVideoStream() || MonitoringConfirmationRequired())
        return;

    impl_->StopSeek();

    impl_->playing = true;
    impl_->playback_start_us = start_us;
    impl_->paused_position_us = start_us;
    impl_->playback_started = std::chrono::steady_clock::now();
    impl_->independent_clock.store(!impl_->has_audio);
    impl_->presentation_stopped.store(false);
    // Seed the clock the decode thread reads before it can produce anything:
    // the frames between the preceding keyframe and start_us are already in
    // the past and must not be paid a colour conversion for.
    impl_->media_clock_us.store(start_us);

    if (impl_->has_audio)
        impl_->audio.Start();

    // GPU render path (see SetOnFrameReady's doc comment): delivered straight
    // to on_frame from the engine's own decode thread, no intermediate queue
    // -- EditPlayerRenderer::PresentFrame does the present-time clock gate
    // that used to live in a queue/poll path.
    impl_->engine.StartPlaybackDecode(
        start_us,
        [this, generation](RawDecodedVideoFrame frame) {
            // The engine publishes this before spawning workers. The owner
            // thread may not yet have observed audio setup failure.
            if (!impl_->engine.PlaybackDeliversAudio()) {
                const auto deadline =
                    impl_->playback_started + std::chrono::microseconds(frame.pts_us - impl_->playback_start_us);
                std::unique_lock lock(impl_->presentation_mutex);
                impl_->presentation_cv.wait_until(lock, deadline,
                                                  [this] { return impl_->presentation_stopped.load(); });
            }
            if (!impl_->presentation_stopped.load())
                impl_->DeliverFrame(std::move(frame), generation);
        },
        [this](DecodedAudioBlock block) {
            if (impl_->has_audio && block.interleaved_stereo) {
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
        [this]() -> int64_t { return impl_->media_clock_us.load(); });

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
    if (impl_->has_audio && !impl_->engine.PlaybackDeliversAudio()) {
        impl_->has_audio = false;
        impl_->audio.Stop();
        impl_->independent_clock.store(true);
        impl_->media_clock_us.store(start_us);
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
                auto frame =
                    impl_->engine.ReadFrameAtRaw(request.target_us, VideoReadMode::RandomAccess, request.generation);
                seek_lock.lock();
                if (!impl_->seek_stopped && frame && impl_->seek_generation.load() == request.generation)
                    impl_->DeliverFrame(std::move(*frame), request.presentation_generation);
            }
        });
    }
    impl_->seek_cv.notify_one();
}
int64_t EditPlayerSession::ClockSnapshotUs() const noexcept {
    return impl_->media_clock_us.load(std::memory_order_relaxed);
}

int64_t EditPlayerSession::CurrentPositionMs() const noexcept {
    return CurrentPositionUs() / 1000;
}

int64_t EditPlayerSession::CurrentPositionUs() const noexcept {
    if (!impl_->playing)
        return impl_->paused_position_us;
    const int64_t position_us =
        impl_->independent_clock.load()
            ? impl_->playback_start_us + std::chrono::duration_cast<std::chrono::microseconds>(
                                             std::chrono::steady_clock::now() - impl_->playback_started)
                                             .count()
            : impl_->playback_start_us + [&] {
                  const auto frames = impl_->audio.FramesPlayed();
                  const auto rate = impl_->audio.SampleRate();
                  return rate == 0 ? int64_t{0}
                                   : static_cast<int64_t>(frames / rate * 1'000'000 + frames % rate * 1'000'000 / rate);
              }();
    impl_->media_clock_us.store(position_us);
    return position_us;
}
} // namespace exosnap::engine
