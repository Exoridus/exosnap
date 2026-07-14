#include "recorder_core/edit_player_session.h"

#include "playback_clock.h"
#include "recorder_core/wasapi_audio_render.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace recorder_core {

struct EditPlayerSession::Impl {
    EditPlayerEngine engine;
    WasapiAudioRenderer audio;
    bool has_audio = false;
    bool playing = false;

    std::function<void(DecodedVideoFrame)> on_frame;
    std::mutex callback_mutex; // guards on_frame against concurrent Set/invoke

    // Scrub seek: only ever one in flight. A new SeekTo() bumps the
    // generation counter; the worker thread checks it before delivering a
    // frame so a superseded seek's result is silently dropped -- the
    // "throttled, live" scrub behavior from the design.
    std::atomic<uint64_t> seek_generation{0};
    std::thread seek_thread;
    std::mutex seek_thread_mutex;

    // Bounded, non-blocking smoothing queue between the playback decode
    // thread (writer) and PollFrame() (reader, called from the caller's UI
    // thread) -- two different threads, so this needs its own mutex,
    // distinct from callback_mutex above. Pacing itself is entirely the
    // audio ring's job (WasapiAudioRenderer); this queue only smooths
    // delivery and never blocks the decode thread -- see
    // docs/superpowers/specs/2026-07-14-edit-video-player-pacing-design.md.
    static constexpr size_t kVideoQueueCapacity = 4;
    std::deque<DecodedVideoFrame> video_queue;
    std::mutex video_queue_mutex;

    void DeliverFrame(DecodedVideoFrame frame) {
        std::lock_guard<std::mutex> lock(callback_mutex);
        if (on_frame)
            on_frame(std::move(frame));
    }

    void EnqueueVideoFrame(DecodedVideoFrame frame) {
        std::lock_guard<std::mutex> lock(video_queue_mutex);
        if (video_queue.size() >= kVideoQueueCapacity)
            video_queue.pop_front(); // drop-oldest: smoothing only, never blocks
        video_queue.push_back(std::move(frame));
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
    // those stale frames before the first newly-decoded one arrives.
    {
        std::lock_guard<std::mutex> lock(impl_->video_queue_mutex);
        impl_->video_queue.clear();
    }

    impl_->playing = true;

    if (impl_->has_audio)
        impl_->audio.Start();

    impl_->engine.StartPlaybackDecode(
        start_us, [this](DecodedVideoFrame frame) { impl_->EnqueueVideoFrame(std::move(frame)); },
        [this](DecodedAudioBlock block) {
            if (impl_->has_audio && block.interleaved_stereo)
                impl_->audio.PushSamples(block.interleaved_stereo->data(), block.frame_count);
        });
}

void EditPlayerSession::Pause() {
    if (!impl_->playing)
        return;
    impl_->playing = false;
    // audio.Stop() must run BEFORE engine.StopPlaybackDecode(): Stop() wakes
    // any PushSamples() call currently blocked on a full ring (dropping its
    // remaining data instead of inserting it), which StopPlaybackDecode()'s
    // join() below depends on -- otherwise the playback thread could be
    // blocked forever inside a full ring that nothing is draining anymore.
    if (impl_->has_audio)
        impl_->audio.Stop();
    impl_->engine.StopPlaybackDecode();
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

    const int64_t clock_ms = AudioClockMs(impl_->audio.FramesRendered(), impl_->audio.SampleRate());

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

    DecodedVideoFrame frame = std::move(impl_->video_queue.front());
    impl_->video_queue.pop_front();
    return frame;
}

int64_t EditPlayerSession::CurrentPositionMs() const noexcept {
    if (!impl_->has_audio)
        return 0;
    return AudioClockMs(impl_->audio.FramesRendered(), impl_->audio.SampleRate());
}

} // namespace recorder_core
