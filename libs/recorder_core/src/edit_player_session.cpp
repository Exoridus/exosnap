#include "recorder_core/edit_player_session.h"

#include "playback_clock.h"
#include "recorder_core/wasapi_audio_render.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace recorder_core {

namespace {

// The audio ring's own capacity expressed in seconds -- the decode-ahead
// window the video queue has to be able to hold. 48 kHz is
// WasapiAudioRenderer's fixed engine rate. The sizing is unchanged from when
// one thread served both streams: it is still the right window, it is just no
// longer the audio ring that enforces it on the video side (the frame queue
// below does that itself now).
constexpr double kDecodeAheadSeconds = static_cast<double>(kDefaultRingCapacityFrames) / 48000.0;

// The sizing itself is pure math and lives (and is unit-tested) in
// playback_clock.h alongside the rest of the pacing arithmetic.
constexpr size_t kMinVideoQueueCapacity = 16;

// Bytes one queued frame occupies. DecodedVideoFrame carries BGRA, so this is
// the clip's coded size regardless of its source chroma. Returns 0 when the
// size is not known, which VideoQueueCapacityForFrameRate reads as "no byte
// information" rather than "no budget".
size_t QueuedFrameBytes(int width, int height) noexcept {
    if (width <= 0 || height <= 0)
        return 0;
    return static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
}

} // namespace

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
    // run, added back into every clock read in PollFrame()/
    // CurrentPositionMs(). Only ever written from the caller's own UI thread
    // (Play(), same as the rest of this class's public API contract), so no
    // extra synchronization is needed to read it from those same methods.
    int64_t playback_start_us = 0;

    std::function<void(DecodedVideoFrame)> on_frame;
    std::mutex callback_mutex; // guards on_frame against concurrent Set/invoke

    // Scrub seek: only ever one in flight. A new SeekTo() bumps the
    // generation counter; the worker thread checks it before delivering a
    // frame so a superseded seek's result is silently dropped -- the
    // "throttled, live" scrub behavior from the design.
    std::atomic<uint64_t> seek_generation{0};
    std::thread seek_thread;
    std::mutex seek_thread_mutex;

    // Bounded, BLOCKING queue between the engine's video decode thread
    // (writer) and PollFrame() (reader, called from the caller's UI thread) --
    // two different threads, so this needs its own mutex, distinct from
    // callback_mutex above.
    //
    // Blocking, not drop-oldest: presentation paces decode. Now that video and
    // audio decode on separate threads (docs/superpowers/specs/
    // 2026-08-01-edit-player-decoupled-decode-design.md), nothing else holds
    // the video thread back, and a drop-oldest queue would let it race through
    // the file converting frames nobody will ever see -- the conversion is the
    // dominant per-frame cost, so that is the one thing it must not do.
    // Blocking it here also cannot starve audio anymore, which is exactly what
    // made drop-oldest necessary before.
    //
    // Capacity must still cover the decode-ahead window the audio ring allows
    // -- kDefaultRingCapacityFrames is 200 ms of audio, so that window holds
    // (frame rate x 0.2 s) video frames. Derived from the OPENED CLIP's rate,
    // never a constant: a fixed 16 (the 60 fps case plus headroom) is far too
    // small at any higher rate -- 200 ms of a 144 fps clip is ~29 frames.
    size_t video_queue_capacity = kMinVideoQueueCapacity;
    std::deque<DecodedVideoFrame> video_queue;
    std::mutex video_queue_mutex;
    std::condition_variable video_queue_cv;
    // Releases a writer blocked on a full queue during teardown. Every wait
    // predicate below includes it: a decode thread still parked here when
    // StopPlaybackDecode() joins it would be a hang, not a delay.
    bool video_queue_release = false;

    // Playback clock snapshot in absolute media time, or -1 when there is no
    // clock. Refreshed from the caller's own thread on every presentation tick
    // (PollFrame/CurrentPositionMs) and read by the engine's video thread to
    // decide whether a decoded frame is still worth converting.
    //
    // A snapshot rather than a direct WasapiAudioRenderer::FramesPlayed() call
    // from the decode thread on purpose: that method documents a
    // single-caller-thread contract (it reads through a COM clock object that
    // Shutdown() releases). One presentation tick of staleness is irrelevant
    // to a "has this frame's time already passed" decision, and a stale
    // snapshot can only ever fail towards converting a frame that a fresher
    // clock would have discarded -- never the other way round.
    std::atomic<int64_t> media_clock_us{-1};

    void DeliverFrame(DecodedVideoFrame frame) {
        std::lock_guard<std::mutex> lock(callback_mutex);
        if (on_frame)
            on_frame(std::move(frame));
    }

    void EnqueueVideoFrame(DecodedVideoFrame frame) {
        std::unique_lock<std::mutex> lock(video_queue_mutex);
        video_queue_cv.wait(lock, [this] { return video_queue_release || video_queue.size() < video_queue_capacity; });
        if (video_queue_release)
            return; // tearing down: drop the frame rather than queue it
        video_queue.push_back(std::move(frame));
    }

    // Wakes a writer blocked in EnqueueVideoFrame so it can be joined. Must run
    // BEFORE EditPlayerEngine::StopPlaybackDecode() -- the engine can wake its
    // own waits, not this one.
    void ReleaseVideoQueueWriter() {
        {
            std::lock_guard<std::mutex> lock(video_queue_mutex);
            video_queue_release = true;
        }
        video_queue_cv.notify_all();
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

    impl_->video_queue_capacity = VideoQueueCapacityForFrameRate(
        impl_->engine.VideoFrameRate(), kDecodeAheadSeconds,
        QueuedFrameBytes(impl_->engine.VideoWidth(), impl_->engine.VideoHeight()), kDefaultMaxVideoQueueBytes);
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
    // Pause() already did this for a running playback, but Close() must also
    // hold for a session that was never playing (or whose decode threads ended
    // at EOF): nothing may be left parked in the frame queue.
    impl_->ReleaseVideoQueueWriter();
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

void EditPlayerSession::SetOnFrameReady(std::function<void(DecodedVideoFrame)> callback) {
    std::lock_guard<std::mutex> lock(impl_->callback_mutex);
    impl_->on_frame = std::move(callback);
}

void EditPlayerSession::Play(int64_t start_us) {
    if (impl_->playing)
        return;

    // Ensure any in-flight scrub seek (spawned by SeekTo) has fully finished
    // before playback starts. EditPlayerEngine's DecodeFrameAt (the scrub
    // path) and its continuous playback thread share the same demux/decode
    // contexts with no internal synchronization between the two -- that is
    // the engine's documented single-writer contract, enforced by callers,
    // not by the engine itself. SeekTo() already guarantees the other
    // direction (it calls Pause() before seeking), but without this join the
    // normal resume-on-release flow (scrub -> SeekTo spawns a background
    // seek -> user releases -> Play()) could start the playback thread while
    // that seek thread is still running DecodeFrameAt on the same contexts --
    // a genuine data race, not just a redundant frame delivery.
    {
        std::lock_guard<std::mutex> lock(impl_->seek_thread_mutex);
        if (impl_->seek_thread.joinable())
            impl_->seek_thread.join();
    }

    // A previous playback run (before a pause or a seek-away) may have left
    // frames queued; starting fresh from start_us must never show one of
    // those stale frames before the first newly-decoded one arrives. The
    // release flag from that run's teardown is cleared here too, so the new
    // run's decode thread can block on this queue again.
    {
        std::lock_guard<std::mutex> lock(impl_->video_queue_mutex);
        impl_->video_queue.clear();
        impl_->video_queue_release = false;
    }

    impl_->playing = true;
    impl_->playback_start_us = start_us;
    // Seed the clock the decode thread reads before it can produce anything:
    // the frames between the preceding keyframe and start_us are already in
    // the past and must not be paid a colour conversion for.
    impl_->media_clock_us.store(impl_->has_audio ? start_us : -1);

    if (impl_->has_audio)
        impl_->audio.Start();

    impl_->engine.StartPlaybackDecode(
        start_us, [this](DecodedVideoFrame frame) { impl_->EnqueueVideoFrame(std::move(frame)); },
        [this](DecodedAudioBlock block) {
            if (impl_->has_audio && block.interleaved_stereo)
                impl_->audio.PushSamples(block.interleaved_stereo->data(), block.frame_count);
        },
        [this]() -> int64_t { return impl_->media_clock_us.load(); });

    // The engine builds its playback resampler inside StartPlaybackDecode, and
    // that can fail on a file whose audio stream opened perfectly well. This
    // class paces video off the audio clock, so believing in audio that never
    // arrives would leave FramesPlayed() at 0 forever: the clock would stay
    // pinned to start_us, PollFrame would keep selecting the same frame, and
    // playback would present as a frozen picture with a stationary playhead.
    // Fall back to the same video-only path a file without an audio stream
    // takes (see PollFrame) instead.
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
    // Both releases must run BEFORE engine.StopPlaybackDecode(): its join()
    // cannot wake a decode thread parked in a wait that belongs to this class.
    // audio.Stop() wakes a PushSamples() blocked on a full ring (dropping its
    // remaining data instead of inserting it); ReleaseVideoQueueWriter() wakes
    // the video thread blocked on a full frame queue. Either one left blocked
    // turns the join into a deadlock, since nothing drains those buffers once
    // playback has stopped.
    if (impl_->has_audio)
        impl_->audio.Stop();
    impl_->ReleaseVideoQueueWriter();
    impl_->engine.StopPlaybackDecode();
    impl_->media_clock_us.store(-1);
}

void EditPlayerSession::SeekTo(int64_t target_us) {
    Pause(); // scrubbing pauses; resume-on-release is the caller's job (matches existing UI contract)

    const uint64_t my_generation = impl_->seek_generation.fetch_add(1) + 1;

    std::lock_guard<std::mutex> lock(impl_->seek_thread_mutex);
    if (impl_->seek_thread.joinable())
        impl_->seek_thread.join(); // the previous seek already saw a bumped generation and is winding down

    impl_->seek_thread = std::thread([this, target_us, my_generation]() {
        auto frame = impl_->engine.DecodeFrameAt(target_us);
        if (frame.has_value() && impl_->seek_generation.load() == my_generation)
            impl_->DeliverFrame(std::move(*frame));
    });
}

std::optional<DecodedVideoFrame> EditPlayerSession::PollFrame() {
    if (!impl_->has_audio)
        return std::nullopt; // caller must drive the no-audio fallback via SeekTo() instead

    const int64_t clock_ms =
        impl_->playback_start_us / 1000 + AudioClockMs(impl_->audio.FramesPlayed(), impl_->audio.SampleRate());
    impl_->media_clock_us.store(clock_ms * 1000);

    std::optional<DecodedVideoFrame> selected;
    {
        std::lock_guard<std::mutex> lock(impl_->video_queue_mutex);
        if (impl_->video_queue.empty())
            return std::nullopt;

        std::vector<int64_t> pts_ms;
        pts_ms.reserve(impl_->video_queue.size());
        for (const auto& frame : impl_->video_queue)
            pts_ms.push_back(frame.pts_us / 1000);

        const FrameSelection sel = SelectFrameForClock(pts_ms, clock_ms);
        if (!sel.index.has_value())
            return std::nullopt; // clock hasn't reached the first queued frame's timestamp yet

        // dropped_count is purely positional (SelectFrameForClock's own
        // contract, see playback_clock.h): after popping that many frames from
        // the front, the selected frame is now at the front of what remains.
        for (size_t i = 0; i < sel.dropped_count; ++i)
            impl_->video_queue.pop_front();

        selected = std::move(impl_->video_queue.front());
        impl_->video_queue.pop_front();
    }
    // Draining the queue is what un-blocks the video decode thread now that
    // this queue is the thing pacing it.
    impl_->video_queue_cv.notify_one();
    return selected;
}

int64_t EditPlayerSession::CurrentPositionMs() const noexcept {
    if (!impl_->has_audio)
        return 0;
    const int64_t position_ms =
        impl_->playback_start_us / 1000 + AudioClockMs(impl_->audio.FramesPlayed(), impl_->audio.SampleRate());
    // Same tick, same thread as PollFrame(): keep the decode thread's clock
    // snapshot fresh even on ticks where no frame is due.
    impl_->media_clock_us.store(position_ms * 1000);
    return position_ms;
}

} // namespace recorder_core
