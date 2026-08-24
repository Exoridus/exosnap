#include "exosnap/engine/edit_player_session.h"

#include "exosnap/engine/wasapi_audio_render.h"
#include "playback_clock.h"

#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

namespace exosnap::engine {

struct EditPlayerSession::Impl {
    EditPlayerEngine engine;
    WasapiAudioRenderer audio;
    bool has_audio = false;
    bool playing = false;

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

    // GPU render path (2026-08-03 design) -- the callback Play()/SeekTo()
    // drive; see SetOnFrameReady's doc comment. Delivered directly from the
    // engine's own decode/seek thread, with no intermediate queue: pacing/
    // drop decisions live in EditPlayerRenderer::PresentFrame instead.
    std::function<void(RawDecodedVideoFrame)> on_frame;
    std::mutex callback_mutex; // guards on_frame against concurrent Set/invoke

    // Scrub seek: only ever one in flight. A new SeekTo() bumps the
    // generation counter; the worker thread checks it before delivering a
    // frame so a superseded seek's result is silently dropped -- the
    // "throttled, live" scrub behavior from the design.
    std::atomic<uint64_t> seek_generation{0};
    std::thread seek_thread;
    std::mutex seek_thread_mutex;

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

    void DeliverFrame(RawDecodedVideoFrame frame) {
        std::lock_guard<std::mutex> lock(callback_mutex);
        if (on_frame)
            on_frame(std::move(frame));
    }
};

EditPlayerSession::EditPlayerSession() : impl_(std::make_unique<Impl>()) {
}

EditPlayerSession::~EditPlayerSession() {
    Close();
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
    {
        std::lock_guard<std::mutex> lock(impl_->seek_thread_mutex);
        impl_->seek_generation.fetch_add(1); // supersede any in-flight seek
        if (impl_->seek_thread.joinable())
            impl_->seek_thread.join();
    }
    impl_->audio.Shutdown();
    impl_->engine.Close();
    impl_->has_audio = false;
}

bool EditPlayerSession::HasAudioStream() const noexcept {
    return impl_->has_audio;
}

double EditPlayerSession::VideoFrameRate() const noexcept {
    return impl_->engine.VideoFrameRate();
}

void EditPlayerSession::SetOnFrameReady(std::function<void(RawDecodedVideoFrame)> callback) {
    std::lock_guard<std::mutex> lock(impl_->callback_mutex);
    impl_->on_frame = std::move(callback);
}

void EditPlayerSession::Play(int64_t start_us) {
    if (impl_->playing)
        return;

    // Ensure any in-flight scrub seek (spawned by SeekTo) has fully finished
    // before playback starts. EditPlayerEngine's DecodeFrameAtRaw (the scrub
    // path) and its continuous playback thread share the same demux/decode
    // contexts with no internal synchronization between the two -- that is
    // the engine's documented single-writer contract, enforced by callers,
    // not by the engine itself. SeekTo() already guarantees the other
    // direction (it calls Pause() before seeking), but without this join the
    // normal resume-on-release flow (scrub -> SeekTo spawns a background
    // seek -> user releases -> Play()) could start the playback thread while
    // that seek thread is still running DecodeFrameAtRaw on the same
    // contexts -- a genuine data race, not just a redundant frame delivery.
    {
        std::lock_guard<std::mutex> lock(impl_->seek_thread_mutex);
        if (impl_->seek_thread.joinable())
            impl_->seek_thread.join();
    }

    impl_->playing = true;
    impl_->playback_start_us = start_us;
    // Seed the clock the decode thread reads before it can produce anything:
    // the frames between the preceding keyframe and start_us are already in
    // the past and must not be paid a colour conversion for.
    impl_->media_clock_us.store(impl_->has_audio ? start_us : -1);

    if (impl_->has_audio)
        impl_->audio.Start();

    // GPU render path (see SetOnFrameReady's doc comment): delivered straight
    // to on_frame from the engine's own decode thread, no intermediate queue
    // -- EditPlayerRenderer::PresentFrame does the present-time clock gate
    // that used to live in a queue/poll path.
    impl_->engine.StartPlaybackDecode(
        start_us, [this](RawDecodedVideoFrame frame) { impl_->DeliverFrame(std::move(frame)); },
        [this](DecodedAudioBlock block) {
            if (impl_->has_audio && block.interleaved_stereo)
                impl_->audio.PushSamples(block.interleaved_stereo->data(), block.frame_count);
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
        impl_->media_clock_us.store(-1); // no clock: the decode thread stops discarding against it
    }
}

void EditPlayerSession::Pause() {
    if (!impl_->playing)
        return;
    impl_->playing = false;
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

void EditPlayerSession::SeekTo(int64_t target_us) {
    Pause(); // scrubbing pauses; resume-on-release is the caller's job (matches existing UI contract)

    const uint64_t my_generation = impl_->seek_generation.fetch_add(1) + 1;

    std::lock_guard<std::mutex> lock(impl_->seek_thread_mutex);
    if (impl_->seek_thread.joinable())
        impl_->seek_thread.join(); // the previous seek already saw a bumped generation and is winding down

    // GPU render path (see SetOnFrameReady's doc comment).
    impl_->seek_thread = std::thread([this, target_us, my_generation]() {
        auto frame = impl_->engine.DecodeFrameAtRaw(target_us);
        if (frame.has_value() && impl_->seek_generation.load() == my_generation)
            impl_->DeliverFrame(std::move(*frame));
    });
}

int64_t EditPlayerSession::ClockSnapshotUs() const noexcept {
    return impl_->media_clock_us.load(std::memory_order_relaxed);
}

int64_t EditPlayerSession::CurrentPositionMs() const noexcept {
    if (!impl_->has_audio)
        return 0;
    const int64_t position_ms =
        impl_->playback_start_us / 1000 + AudioClockMs(impl_->audio.FramesPlayed(), impl_->audio.SampleRate());
    // Also refreshes the decode thread's clock snapshot (see ClockSnapshotUs)
    // on every presentation tick, not just the ones where a new frame lands.
    impl_->media_clock_us.store(position_ms * 1000);
    return position_ms;
}

} // namespace exosnap::engine
