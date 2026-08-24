# Edit Video Player Pacing Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire up the audio-master-clock pacing that the original Edit-page video player design
specified but never actually connected — playback currently decodes and delivers every video frame
immediately and unthrottled, racing through the file at decode speed instead of real time.

**Architecture:** `WasapiAudioRenderer`'s internal ring buffer becomes a fixed-capacity, blocking
queue — the single pacing point for both audio and video, since both are read on the same playback
decode thread. `EditPlayerSession` gains a small bounded, non-blocking video queue (smoothing only)
and a new `PollFrame()` pull method that uses the already-tested `AudioClockMs`/`SelectFrameForClock`
to pick the right frame. `EditExportPage`'s existing 33 ms preview timer drives `PollFrame()` directly
(no audio stream) instead of the frame arriving unthrottled via a queued callback.

**Tech Stack:** C++20, Qt 6 Widgets, WASAPI (`Audioclient.h`), GoogleTest via `exosnap_add_gtest`.

## Global Constraints

- Engine code (`libs/engine`) must contain **no Qt types** — CLAUDE.md: "Keep the engine
  UI-agnostic." Only `app/` files may include Qt headers.
- **Never drive the running application** — no mouse/keyboard synthesis, no window automation.
  Starting `exosnap.exe` once to confirm no startup crash is allowed; nothing interactive.
- Live playback/A-V-sync correctness (does it actually look/sound right) is a **manual verification
  step for the user** — this project's existing `test_wasapi_audio_render.cpp` deliberately never
  calls `Init()` (no real WASAPI render device assumed available in CI); every test in this plan
  follows that same constraint.
- Follow existing `engine` conventions exactly: trailing-underscore private members,
  PascalCase free functions/methods, snake_case locals, `bool Method(..., std::string& out_error)`
  for fallible setup.
- Run `pwsh scripts/run-tests.ps1 -Filter <binary>` for focused verification during each task (not
  raw `ctest` — the script sets `EXOSNAP_CONFIG_DIR`, `QT_QPA_PLATFORM=offscreen`, and Qt/FFmpeg on
  `PATH`); the full gate (`pwsh scripts/run-tests.ps1` with no filter) runs once at the end (Task 4).

---

## Task 1: `WasapiAudioRenderer` — bounded, blocking ring buffer (the pacing point)

**Files:**
- Modify: `libs/engine/include/exosnap/engine/wasapi_audio_render.h`
- Modify: `libs/engine/src/wasapi_audio_render.cpp`
- Test: `libs/engine/tests/test_wasapi_audio_render.cpp`

**Interfaces:**
- Produces: `WasapiAudioRenderer(uint32_t ring_capacity_frames = kDefaultRingCapacityFrames)` —
  constructor now takes an optional ring capacity (default 48,000 frames = 1 s @ 48 kHz). Consumed
  by Task 2 (production code uses the default; this task's own tests use a tiny capacity for
  determinism). `PushSamples()` now blocks until room is available or `Stop()` is called (documented
  behavior change consumed by Task 2's `Pause()` reordering).

- [ ] **Step 1: Write the failing test**

Add to the end of `libs/engine/tests/test_wasapi_audio_render.cpp`, before the closing
`} // namespace`:

```cpp
TEST(WasapiAudioRenderer, PushSamplesBlocksWhenRingIsFullAndStopWakesIt) {
    // A tiny capacity makes this deterministic without a real device: the
    // ring is a pure producer/consumer queue independent of whether a device
    // is open (Init() clears it via Shutdown() at the top of Init(), so
    // nothing pushed pre-Init can leak into playback) -- this test never
    // calls Init(), matching this file's existing no-real-device convention.
    exosnap::engine::WasapiAudioRenderer renderer(/*ring_capacity_frames=*/4);

    const std::vector<float> chunk(8, 0.0f); // 4 stereo frames == exactly the capacity
    renderer.PushSamples(chunk.data(), 4);   // fills the ring; must return immediately

    std::atomic<bool> push_returned{false};
    std::thread pusher([&] {
        renderer.PushSamples(chunk.data(), 4); // ring is full: must block
        push_returned.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(push_returned.load()) << "PushSamples returned before Stop() -- ring capacity is not enforced";

    renderer.Stop(); // must wake the blocked push (dropping its data) without needing Init()

    for (int i = 0; i < 50 && !push_returned.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_TRUE(push_returned.load()) << "Stop() did not wake a PushSamples() blocked on a full ring";
    pusher.join();
}
```

Add these includes to the top of the file (`std::vector` is already included):

```cpp
#include <atomic>
#include <chrono>
#include <thread>
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build --preset windows-x64-debug --target test_wasapi_audio_render
```

Expected: FAIL to compile — `WasapiAudioRenderer(uint32_t)` constructor does not exist yet (only the
default constructor does).

- [ ] **Step 3: Add the capacity constant and constructor to the header**

In `libs/engine/include/exosnap/engine/wasapi_audio_render.h`, add after the includes and
before `struct IMMDevice;`:

```cpp
#include <condition_variable>
```

Add inside `namespace exosnap::engine {`, before `class WasapiAudioRenderer {`:

```cpp
// Ring capacity used unless a caller overrides it. 1 second @ 48 kHz stereo
// is the fixed backpressure point that paces the playback decode thread --
// see docs/superpowers/specs/2026-07-14-edit-video-player-pacing-design.md.
inline constexpr uint32_t kDefaultRingCapacityFrames = 48000;
```

Replace the existing default constructor declaration:

```cpp
    WasapiAudioRenderer();
```

with:

```cpp
    explicit WasapiAudioRenderer(uint32_t ring_capacity_frames = kDefaultRingCapacityFrames);
```

Update the `PushSamples` doc comment (directly above its declaration) from "Safe to call from any
thread. No-op if not initialized." to:

```cpp
    // Appends `frame_count` stereo frames (frame_count * 2 floats,
    // interleaved L,R) to the internal ring buffer, blocking the caller
    // until enough room is available (this is the playback pacing point --
    // see the class doc comment above). A blocked call is woken early by
    // Stop(), which drops whatever had not yet been inserted rather than
    // writing it. Safe to call from any thread. No-op if frame_count is 0.
```

Add two new private members, next to the existing `ring_mutex_`/`ring_` declarations:

```cpp
    std::condition_variable ring_cv_; // paired with ring_mutex_: signaled when ring space frees up
                                       // or a Stop() is in progress
    bool stop_requested_ = false;     // guarded by ring_mutex_; wakes+drops any blocked PushSamples
    uint32_t ring_capacity_floats_;   // ring_ capacity in interleaved floats (frames * channels)
```

- [ ] **Step 4: Update the constructor, `PushSamples`, `RenderThreadMain`, and `Stop` in the .cpp**

In `libs/engine/src/wasapi_audio_render.cpp`, replace:

```cpp
WasapiAudioRenderer::WasapiAudioRenderer() = default;
```

with:

```cpp
WasapiAudioRenderer::WasapiAudioRenderer(uint32_t ring_capacity_frames)
    : ring_capacity_floats_(ring_capacity_frames * kEngineChannels) {
}
```

Replace the entire `PushSamples` body:

```cpp
void WasapiAudioRenderer::PushSamples(const float* interleaved_stereo, uint32_t frame_count) {
    if (!initialized_ || interleaved_stereo == nullptr || frame_count == 0)
        return;
    std::lock_guard<std::mutex> lock(ring_mutex_);
    ring_.insert(ring_.end(), interleaved_stereo,
                 interleaved_stereo + static_cast<size_t>(frame_count) * kEngineChannels);
}
```

with:

```cpp
void WasapiAudioRenderer::PushSamples(const float* interleaved_stereo, uint32_t frame_count) {
    if (interleaved_stereo == nullptr || frame_count == 0)
        return;
    // Not gated on initialized_: the ring is a pure producer/consumer queue,
    // independent of whether a real device is open. In production this is
    // only ever called after Init() succeeded (EditPlayerSession only pushes
    // when HasAudioStream() is true, which requires a successful Init()), and
    // Init() always clears any pre-existing ring contents via its own
    // Shutdown() call, so nothing pushed before Init() can leak into
    // playback -- this just makes the ring's blocking/capacity behavior
    // independently unit-testable without a real WASAPI render device.
    size_t remaining = static_cast<size_t>(frame_count) * kEngineChannels;
    const float* src = interleaved_stereo;
    std::unique_lock<std::mutex> lock(ring_mutex_);
    while (remaining > 0) {
        ring_cv_.wait(lock, [&] { return stop_requested_ || ring_.size() < ring_capacity_floats_; });
        if (stop_requested_)
            return; // renderer is stopping: drop whatever's left rather than insert it
        const size_t room = ring_capacity_floats_ - ring_.size();
        const size_t take = std::min<size_t>(room, remaining); // explicit template arg: windows.h's min() macro (no NOMINMAX here)
        ring_.insert(ring_.end(), src, src + take);
        src += take;
        remaining -= take;
    }
}
```

In `RenderThreadMain`, immediately after the block that erases consumed samples from the ring
(the `std::lock_guard<std::mutex> lock(ring_mutex_)` block that ends with `ring_.erase(...)`), add a
notify so a blocked `PushSamples()` can wake as soon as space frees up:

```cpp
        {
            std::lock_guard<std::mutex> lock(ring_mutex_);
            const size_t take = std::min<size_t>(want_floats, ring_.size());
            engine_buf.assign(ring_.begin(), ring_.begin() + static_cast<std::ptrdiff_t>(take));
            ring_.erase(ring_.begin(), ring_.begin() + static_cast<std::ptrdiff_t>(take));
        }
        ring_cv_.notify_all(); // wake any PushSamples() blocked waiting for room
        const uint32_t engine_frames = static_cast<uint32_t>(engine_buf.size() / kEngineChannels);
```

Replace the entire `Stop` body:

```cpp
void WasapiAudioRenderer::Stop() {
    if (!running_.load())
        return;
    running_.store(false);
    if (buffer_event_ != nullptr)
        SetEvent(static_cast<HANDLE>(buffer_event_)); // wake the render thread so it can observe running_==false
    if (render_thread_.joinable())
        render_thread_.join();
    if (audio_client_ != nullptr)
        audio_client_->Stop();
}
```

with:

```cpp
void WasapiAudioRenderer::Stop() {
    // Always wake anything blocked in PushSamples(), even if the render
    // thread was never started (Init() succeeded but Start() wasn't called
    // yet, or this is a second Stop() call) -- a caller further up the stack
    // (EditPlayerSession::Pause(), see the pacing design doc) relies on this
    // to make StopPlaybackDecode()'s join() unable to hang forever on a
    // decode thread stuck inside a full ring nothing is draining anymore.
    running_.store(false);
    {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        stop_requested_ = true;
    }
    ring_cv_.notify_all();
    if (buffer_event_ != nullptr)
        SetEvent(static_cast<HANDLE>(buffer_event_)); // wake the render thread so it can observe running_==false
    if (render_thread_.joinable())
        render_thread_.join();
    if (initialized_ && audio_client_ != nullptr)
        audio_client_->Stop();
    {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        stop_requested_ = false; // reset so a subsequent Start()/PushSamples() cycle blocks normally again
        ring_.clear();           // drop whatever was left queued; a fresh Play() starts clean
    }
}
```

- [ ] **Step 5: Run test to verify it passes**

```bash
cmake --build --preset windows-x64-debug --target test_wasapi_audio_render
pwsh scripts/run-tests.ps1 -Filter test_wasapi_audio_render
```

Expected: PASS, all 5 cases (4 existing + the new one). In particular confirm
`StopWithoutStartIsSafeNoOp` and `PushSamplesWithoutInitIsSafeNoOp` still pass unchanged — this
step's changes must not require `Init()` for either.

- [ ] **Step 6: Commit**

```bash
git add libs/engine/include/exosnap/engine/wasapi_audio_render.h libs/engine/src/wasapi_audio_render.cpp libs/engine/tests/test_wasapi_audio_render.cpp
git commit -m "WasapiAudioRenderer's ring buffer becomes a bounded, blocking pacing point"
```

---

## Task 2: `EditPlayerSession` — bounded video queue, `PollFrame()`, shutdown ordering

**Files:**
- Modify: `libs/engine/include/exosnap/engine/edit_player_session.h`
- Modify: `libs/engine/src/edit_player_session.cpp`
- Test: `libs/engine/tests/test_edit_player_session.cpp`

**Interfaces:**
- Consumes: `WasapiAudioRenderer::FramesRendered()`, `WasapiAudioRenderer::SampleRate()` (existing),
  `AudioClockMs(uint64_t, uint32_t)`, `SelectFrameForClock(std::span<const int64_t>, int64_t)` →
  `FrameSelection{std::optional<size_t> index; size_t dropped_count;}` (existing, `playback_clock.h`,
  unchanged by this task).
- Produces: `EditPlayerSession::PollFrame()` → `std::optional<DecodedVideoFrame>` and
  `EditPlayerSession::CurrentPositionMs()` → `int64_t`, both consumed by Task 3.

- [ ] **Step 1: Write the failing tests**

Add to the end of `libs/engine/tests/test_edit_player_session.cpp`, before the closing
`} // namespace`:

```cpp
TEST(EditPlayerSession, PollFrameWithoutAudioStreamReturnsNullopt) {
    EditPlayerSession session;
    // has_audio starts false on a never-opened session (see
    // ClosedSessionReportsNoAudioStream above) -- PollFrame() is only valid
    // while HasAudioStream() is true; the caller must use the wall-clock
    // SeekTo() fallback otherwise.
    EXPECT_FALSE(session.PollFrame().has_value());
}

TEST(EditPlayerSession, CurrentPositionMsWithoutAudioStreamIsZero) {
    EditPlayerSession session;
    EXPECT_EQ(session.CurrentPositionMs(), 0);
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build --preset windows-x64-debug --target test_edit_player_session
```

Expected: FAIL to compile — `PollFrame`/`CurrentPositionMs` are not declared on `EditPlayerSession`.

- [ ] **Step 3: Declare the new methods in the header**

In `libs/engine/include/exosnap/engine/edit_player_session.h`, add `#include <optional>` next
to the existing includes, then add these two public methods after `SeekTo`'s declaration (before the
closing `private:`):

```cpp
    // Pulls the next frame to display, paced by the audio master clock.
    // Valid only while HasAudioStream() is true -- returns nullopt
    // unconditionally otherwise (the caller must drive the no-audio
    // fallback by calling SeekTo() itself once per tick instead; see
    // docs/superpowers/specs/2026-07-14-edit-video-player-pacing-design.md).
    // Also returns nullopt if no frames are currently queued, or if the
    // clock hasn't advanced to the next queued frame's timestamp yet.
    // Intended to be polled from the caller's own UI-thread timer.
    [[nodiscard]] std::optional<DecodedVideoFrame> PollFrame();

    // Current playback position derived from the audio master clock (0 if no
    // audio stream, or not currently playing). Kept separate from
    // PollFrame() because the caller needs to advance the displayed
    // position on every tick, even the ones where PollFrame() itself
    // returns nullopt (clock hasn't reached the next frame yet).
    [[nodiscard]] int64_t CurrentPositionMs() const noexcept;
```

- [ ] **Step 4: Implement the video queue, `PollFrame`, `CurrentPositionMs`, and the shutdown-order fix in the .cpp**

In `libs/engine/src/edit_player_session.cpp`, add these includes at the top:

```cpp
#include "playback_clock.h"

#include <deque>
#include <vector>
```

Replace the `Impl` struct's body (everything between `struct EditPlayerSession::Impl {` and its
closing `};`) with:

```cpp
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
```

Replace the `Play` method body:

```cpp
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
```

Replace the `Pause` method body:

```cpp
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
```

Add the two new methods after `SeekTo`'s implementation, before the closing
`} // namespace exosnap::engine`:

```cpp
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
```

- [ ] **Step 5: Run test to verify it passes**

```bash
cmake --build --preset windows-x64-debug --target test_edit_player_session
pwsh scripts/run-tests.ps1 -Filter test_edit_player_session
```

Expected: PASS, all 6 cases (4 existing + 2 new).

- [ ] **Step 6: Full engine rebuild**

```bash
cmake --build --preset windows-x64-debug --target engine
```

Expected: exit 0 (confirms `edit_player_engine.cpp`'s existing callers of `EditPlayerSession` still
link — none exist yet outside `EditExportPage`, checked in Task 3).

- [ ] **Step 7: Commit**

```bash
git add libs/engine/include/exosnap/engine/edit_player_session.h libs/engine/src/edit_player_session.cpp libs/engine/tests/test_edit_player_session.cpp
git commit -m "EditPlayerSession gains a bounded video queue and a clock-paced PollFrame()"
```

---

## Task 3: `EditExportPage` — drive playback from `PollFrame()`, gate `Play()` on audio presence

**Files:**
- Modify: `app/pages/EditExportPage.h`
- Modify: `app/pages/EditExportPage.cpp`
- Test: `app/tests/test_edit_export_page.cpp`

**Interfaces:**
- Consumes: `EditPlayerSession::PollFrame()`, `EditPlayerSession::CurrentPositionMs()`,
  `EditPlayerSession::HasAudioStream()` (existing) — all from Task 2.

- [ ] **Step 1: Write the failing test**

The existing widget tests in `app/tests/test_edit_export_page.cpp` use `MakeContext(...)` with no
`mkv_master_path`, so `player_session_` is constructed but never `Open()`'d and `HasAudioStream()`
stays `false` — meaning they already exercise exactly the no-audio branch this task changes.
`ScrubPausesAndResumesOnlyIfPreviouslyPlaying` (existing, ~line 427) already asserts the no-audio
wall-clock position tracking still works after a scrub/resume cycle. Add one new test after it that
locks down the new no-audio `SeekTo()`-per-tick behavior added in this task doesn't break playing
across the end of the clip (a case not currently covered):

```cpp
TEST_F(EditExportPageTest, PreviewStopsAtEndOfClipWithNoAudioStream) {
    EditExportPage page;
    page.resize(900, 700);
    page.show();
    page.setEditContext(MakeContext(1.0)); // 1-second clip: reaches the end in a couple of ticks
    page.setPhase(EditExportPage::Phase::Edit);
    SettleLayout();

    page.setPreviewPlaying(true);
    ASSERT_TRUE(page.isPreviewPlaying());

    // Let the preview timer (33 ms) run past the 1-second duration. This
    // binary links gtest, not Qt Test (see the SendMouse comment above), so
    // QTest::qWait is unavailable -- WaitMs pumps a real Qt event loop for
    // real wall-clock time instead, which is what the 33 ms QTimer needs to
    // actually fire repeatedly.
    WaitMs(1200);

    EXPECT_FALSE(page.isPreviewPlaying());
    EXPECT_EQ(page.previewPositionMs(), 1000);
}
```

Add a `WaitMs` helper next to `SettleLayout` (same anonymous namespace, top of the file):

```cpp
// Pumps a real Qt event loop for `ms` wall-clock milliseconds -- unlike
// SettleLayout's processEvents() loop, this actually lets QTimer-driven
// code (e.g. EditExportPage's 33 ms preview_timer_) fire repeatedly.
void WaitMs(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}
```

Add `#include <QEventLoop>` and `#include <QTimer>` to the file's existing include block if not
already present (check first — `EditExportPage.h`, which this file includes, only forward-declares
`QTimer`, so the test `.cpp` needs its own include to call `QTimer::singleShot`).

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build --preset windows-x64-debug --target edit_export_page_tests
pwsh scripts/run-tests.ps1 -Filter edit_export_page_tests
```

Expected: this specific case may already pass against the CURRENT code (today's wall-clock-only
`onPreviewTick` already stops at the end) — run it now to confirm it's green before this task's
changes, then re-run after Step 3 to confirm it's still green (a regression guard, not a RED/GREEN
pair, since this task's change to the no-audio path is additive/parallel to the existing wall-clock
logic, not a replacement of it).

- [ ] **Step 3: Add the `DecodedFrameToQImage` helper and declare it**

In `app/pages/EditExportPage.h`, add this private static method declaration next to
`refreshPlayButton()`/`updatePlayerHeight()`:

```cpp
    static QImage DecodedFrameToQImage(const exosnap::engine::DecodedVideoFrame& frame);
```

In `app/pages/EditExportPage.cpp`, add the implementation just above `onPreviewTick()` (in the
"Preview playback clock" section):

```cpp
QImage EditExportPage::DecodedFrameToQImage(const exosnap::engine::DecodedVideoFrame& frame) {
    const QImage img(frame.bgra->data(), static_cast<int>(frame.width), static_cast<int>(frame.height),
                     static_cast<int>(frame.stride_bytes), QImage::Format_ARGB32);
    return img.copy(); // detach: frame.bgra's buffer lifetime is not guaranteed beyond this call
}
```

Replace the existing `SetOnFrameReady` registration (in `setEditContext`, around line 727):

```cpp
            player_session_->SetOnFrameReady([this](exosnap::engine::DecodedVideoFrame frame) {
                // Invoked from the session's internal decode/seek threads --
                // never touch player_surface_ here. The QImage below is a
                // zero-copy view over frame.bgra; .copy() detaches it while
                // this lambda still keeps the shared buffer alive, and the
                // queued call carries the detached copy (by value) onto the
                // UI thread's event queue.
                const QImage img(frame.bgra->data(), static_cast<int>(frame.width), static_cast<int>(frame.height),
                                 static_cast<int>(frame.stride_bytes), QImage::Format_ARGB32);
                QMetaObject::invokeMethod(this, "onDecodedFrameReady", Qt::QueuedConnection, Q_ARG(QImage, img.copy()));
            });
```

with:

```cpp
            player_session_->SetOnFrameReady([this](exosnap::engine::DecodedVideoFrame frame) {
                // Invoked from the session's internal seek-worker thread
                // (scrub/trim-drag path only -- continuous playback frames
                // now go through PollFrame() in onPreviewTick() instead, see
                // below). Never touch player_surface_ here; marshal onto the
                // UI thread.
                QMetaObject::invokeMethod(this, "onDecodedFrameReady", Qt::QueuedConnection,
                                          Q_ARG(QImage, DecodedFrameToQImage(frame)));
            });
```

- [ ] **Step 4: Gate `Play()` on audio presence in `setPreviewPlaying`**

Replace `setPreviewPlaying`'s body:

```cpp
void EditExportPage::setPreviewPlaying(bool playing) {
    if (playing == preview_playing_)
        return;
    if (playing && durationMs() <= 0)
        return; // unknown duration: nothing to play against
    preview_playing_ = playing;
    if (preview_playing_) {
        // preview_timer_/preview_elapsed_ keep driving the playhead position
        // via onPreviewTick exactly as before; the session decodes/presents
        // frames for that same span (its internal audio clock, when present,
        // drives the actual frame pacing).
        preview_elapsed_->restart();
        preview_timer_->start();
        if (player_session_)
            player_session_->Play(preview_position_ms_ * 1000); // ms -> us: resume from where the
                                                                // playhead actually is (a pause or
                                                                // a prior scrub), not the beginning
    } else {
        preview_timer_->stop();
        if (player_session_)
            player_session_->Pause();
    }
    refreshPlayButton();
}
```

with:

```cpp
void EditExportPage::setPreviewPlaying(bool playing) {
    if (playing == preview_playing_)
        return;
    if (playing && durationMs() <= 0)
        return; // unknown duration: nothing to play against
    preview_playing_ = playing;
    if (preview_playing_) {
        preview_elapsed_->restart();
        preview_timer_->start();
        // Continuous decode (EditPlayerSession::Play()) is only engaged when
        // there's an audio stream to pace it against -- see onPreviewTick().
        // A clip with no audio stream is driven entirely by the per-tick
        // SeekTo() fallback there instead; starting continuous decode here
        // for a no-audio clip would just race through the file unthrottled
        // for frames nothing ever consumes (see
        // docs/superpowers/specs/2026-07-14-edit-video-player-pacing-design.md).
        if (player_session_ && player_session_->HasAudioStream())
            player_session_->Play(preview_position_ms_ * 1000); // ms -> us: resume from where the
                                                                // playhead actually is (a pause or
                                                                // a prior scrub), not the beginning
    } else {
        preview_timer_->stop();
        if (player_session_)
            player_session_->Pause();
    }
    refreshPlayButton();
}
```

- [ ] **Step 5: Rewrite `onPreviewTick` to pull from `PollFrame()` when audio is present**

Replace `onPreviewTick`'s body:

```cpp
void EditExportPage::onPreviewTick() {
    preview_position_ms_ += preview_elapsed_->restart();
    const qint64 total = durationMs();
    if (preview_position_ms_ >= total) {
        preview_position_ms_ = total;
        setPreviewPlaying(false); // reached the end: pause there
    }
    if (timeline_)
        timeline_->setPositionMs(preview_position_ms_);
}
```

with:

```cpp
void EditExportPage::onPreviewTick() {
    const bool paced_by_audio = player_session_ && player_session_->HasAudioStream();
    if (paced_by_audio) {
        // Audio is the pacing AND position source of truth while it exists
        // -- no independent wall-clock estimate to keep in sync with it.
        preview_position_ms_ = ClampPlayheadMs(player_session_->CurrentPositionMs(), durationMs());
        if (auto frame = player_session_->PollFrame())
            onDecodedFrameReady(DecodedFrameToQImage(*frame)); // already on the UI thread: direct call
    } else {
        preview_elapsed_->restart();
        preview_position_ms_ += preview_elapsed_->restart();
        if (player_session_)
            player_session_->SeekTo(preview_position_ms_ * 1000); // ms -> us: no-audio pacing fallback
                                                                   // (safe no-op if not open, matching
                                                                   // EditPlayerSession's own contract)
    }

    const qint64 total = durationMs();
    if (preview_position_ms_ >= total) {
        preview_position_ms_ = total;
        setPreviewPlaying(false); // reached the end: pause there
    }
    if (timeline_)
        timeline_->setPositionMs(preview_position_ms_);
}
```

**Correctness note for the implementer:** the line `preview_elapsed_->restart();` appears twice in
that replacement by mistake if copied verbatim from a naive edit — the no-audio branch must call
`preview_elapsed_->restart()` exactly **once** per tick (it both resets the elapsed timer for the
next tick and returns the milliseconds since the last call, which is added to
`preview_position_ms_`). Write the no-audio branch as:

```cpp
    } else {
        preview_position_ms_ += preview_elapsed_->restart();
        if (player_session_)
            player_session_->SeekTo(preview_position_ms_ * 1000); // ms -> us: no-audio pacing fallback
                                                                   // (safe no-op if not open, matching
                                                                   // EditPlayerSession's own contract)
    }
```

(This matches the original single-call behavior exactly — only the added `SeekTo()` call is new.)

- [ ] **Step 6: Run test to verify it passes**

```bash
cmake --build --preset windows-x64-debug --target edit_export_page_tests
pwsh scripts/run-tests.ps1 -Filter edit_export_page_tests
```

Expected: PASS, the full existing suite plus the new `PreviewStopsAtEndOfClipWithNoAudioStream` case.
Pay particular attention to `ScrubPausesAndResumesOnlyIfPreviouslyPlaying` and
`ScrubWhilePausedStaysPaused` (existing, no-audio path) — these must be unaffected since
`player_session_->HasAudioStream()` stays `false` throughout (no `mkv_master_path` in `MakeContext`).

- [ ] **Step 7: Commit**

```bash
git add app/pages/EditExportPage.h app/pages/EditExportPage.cpp app/tests/test_edit_export_page.cpp
git commit -m "EditExportPage plays back paced by the audio clock when a clip has audio"
```

---

## Task 4: Full regression + startup check

**Files:** none (verification only).

- [ ] **Step 1: Full engine + app rebuild**

```bash
cmake --build --preset windows-x64-debug-exosnap
```

Expected: exit 0.

- [ ] **Step 2: Full test suite**

```bash
pwsh scripts/run-tests.ps1
```

Expected: PASS, no regressions anywhere (not just the three binaries touched above — `Pause()`'s
reordering and `PushSamples()`'s new blocking contract are both used by `EditPlayerSession`, which
nothing outside `EditExportPage` currently calls, so no other suite should be affected, but this
confirms it).

- [ ] **Step 3: Startup check**

Per CLAUDE.md: starting the app once to confirm no startup crash is allowed and required after a
change that touches widget code. Launch `exosnap.exe` with no arguments, confirm the process starts
and stays alive for a few seconds without crashing, then close it. No interaction beyond that.

- [ ] **Step 4: Update the pacing design doc's status line**

In `docs/superpowers/specs/2026-07-14-edit-video-player-pacing-design.md`, change:

```
Status: approved (brainstorming), 2026-07-14. Not yet implemented.
```

to:

```
Status: implemented, 2026-07-14.
```

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/specs/2026-07-14-edit-video-player-pacing-design.md
git commit -m "Mark the Edit-page video-player pacing fix as implemented"
```

**Remaining limitation to flag to the user (not fixed by this plan, documented in the design doc):**
if `BuildPlaybackResampler` fails for a specific playback session despite `HasAudioStream()` being
true, that session's audio callback never calls `PushSamples()`, so the audio-ring backpressure does
not engage for it even though `HasAudioStream()` still reports true — a narrow edge case of an
already-open audio stream, not fixed here. Live A/V-sync verification (does it actually look/sound
right on a real recording) remains a manual check for the user — no test in this plan exercises a
real WASAPI device or real compressed video decode.
