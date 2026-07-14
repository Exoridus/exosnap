# Edit Video Player — Pacing Fix — Design

Status: implemented, 2026-07-14.

## Problem

A whole-branch review of `feat/edit-video-player` (11 tasks, 15 commits) found that the audio-master-
clock pacing described in the original design
(`docs/superpowers/specs/2026-07-14-edit-video-player-design.md:92-105`) was never actually wired up.
`AudioClockMs`/`SelectFrameForClock` (`libs/recorder_core/src/playback_clock.{h,cpp}`) and
`WasapiAudioRenderer::FramesRendered()` are built and unit-tested but called nowhere outside their own
tests. `EditPlayerEngine::StartPlaybackDecode` (`edit_player_engine.cpp:456-588`) delivers every
decoded video frame immediately and unthrottled via its `on_video` callback;
`EditExportPage.cpp:727-737` forwards each one straight onto the UI thread via
`QMetaObject::invokeMethod(..., Qt::QueuedConnection)` with no pacing in between.

Effect during a real recording: at software-decode speeds (5x+ realtime for H.264/HEVC, more for AV1)
video races through the entire file while audio plays in realtime — visible A/V desync. The UI event
queue fills with an unbounded number of full-frame `QImage`s (GB-scale for a multi-minute clip), and
the audio ring buffer (`WasapiAudioRenderer`) grows without bound since nothing throttles
`PushSamples()`.

This is a gap in the original design's own execution — Task 8 never included a step that actually
wires the queue between decode and paint, even though the design document explicitly described that
mechanism. This document specs the fix.

## Non-goals

- No hardware-accelerated decode (NVDEC/D3D11VA). Still deferred per the original design's own
  non-goals, pending real performance numbers from software decode.
- No changes to scrub/seek behavior — `EditPlayerSession::SeekTo` and its generation-counter
  supersession logic are already correct and untouched by this fix.
- No changes to trim, markers, or export — all independent of the decode/pacing path.
- No UI-visible behavior change beyond correct A/V sync during playback (and, as a side effect, an
  honest playhead position — see "Timeline position" below).

## Architecture

### Audio ring buffer becomes the single pacing point

`WasapiAudioRenderer`'s internal ring (`ring_`, `wasapi_audio_render.cpp`) gets a fixed capacity of one
second of audio (48,000 stereo frames @ 48 kHz — ~384 KB, negligible). `PushSamples()` blocks on a
`std::condition_variable` when the ring is full, until `RenderThreadMain()`'s consume loop drains
enough of it and signals. Because the playback decode thread interleaves video and audio packet
handling on a single thread (`StartPlaybackDecode`'s read loop), a blocking audio push naturally
throttles video decoding to real time too — one pacing mechanism serves both streams, no separate
video-side rate limiter is needed.

### Video queue: bounded, non-blocking, smoothing only

`EditPlayerSession::Impl` gains a small bounded `std::deque<DecodedVideoFrame>` (capacity 4), guarded
by its own mutex distinct from `callback_mutex` (the decode thread writes on every video callback, the
UI thread reads on every `PollFrame()` call — two different threads, so this is a genuine new
cross-thread hazard, not an existing one). The video callback passed into `StartPlaybackDecode` pushes
into this queue instead of calling `DeliverFrame()` directly. On overflow, the oldest queued frame is
dropped (drop-oldest, never blocks) — this queue exists purely to smooth delivery, not to pace it;
pacing is entirely the audio ring's job (previous section). The queue is cleared at the start of every
`Play()` call so a resumed or re-seeked session never has stale frames left over from a previous
playback run available for selection.

### New pull method: `EditPlayerSession::PollFrame()`

```cpp
// Valid only while HasAudioStream() == true. Reads FramesRendered() as the
// pacing clock, uses SelectFrameForClock to pick the best-matching frame
// from the video queue, and drops any older frames as real, honest drops.
// Returns nullopt if the clock hasn't reached the next queued frame yet.
std::optional<DecodedVideoFrame> PollFrame();
```

Called synchronously from the existing `preview_timer_` (33 ms tick, already on the UI thread) in
`EditExportPage` — no new `QueuedConnection` marshalling needed for the playback path.
`SetOnFrameReady()` keeps its existing, sole responsibility: immediate single-frame delivery for the
scrub/seek path (`SeekTo`), unchanged.

### No audio stream in the clip: reuse the existing seek path

When `HasAudioStream() == false` (every audio source was muted/disabled during recording), there is no
audio ring to pace against and no continuous playback queue is used at all. Instead, each
`preview_timer_` tick calls `SeekTo()` at the current wall-clock position (`preview_elapsed_`-driven,
same as today) — reusing the already-correct, already-thread-safe single-frame decode path (generation
counter included) instead of building a second continuous-decode/pacing mechanism for a rare case.

### Shutdown ordering fix

`EditPlayerSession::Pause()` must call `impl_->audio.Stop()` **before** `impl_->engine.
StopPlaybackDecode()`, reversed from a naive read of the current code. `WasapiAudioRenderer::Stop()`
sets `running_ = false` and must also wake any thread blocked inside `PushSamples()` (via the same
condition variable, causing that call to drop its samples and return rather than insert once woken).
If `StopPlaybackDecode()` ran first, its internal `join()` on the playback thread could hang forever
waiting on a thread that is itself blocked forever inside a full ring that no one is draining anymore.

### Timeline position follows the real clock

While `HasAudioStream() == true`, `EditExportPage::preview_position_ms_` during playback is derived
from `AudioClockMs()` (via the session) rather than the independent wall-clock estimate
`preview_elapsed_` currently produces. This keeps the displayed playhead and the frame-selection clock
identical — no drift between "what's shown as the position" and "what's actually driving which frame
is on screen," matching 0.9's "Reliability & Truthful" framing. The wall-clock path remains exactly as
it is today, but only as the no-audio fallback (previous section).

### Known residual gap (documented, not fixed here)

If `BuildPlaybackResampler` fails for a specific playback session despite `HasAudioStream()` being
true (audio stream present but not resamplable for some reason), that session's audio callback never
calls `PushSamples()`, so the audio-ring backpressure does not engage for it even though
`HasAudioStream()` still reports true. This is a narrow edge case of an already-open audio stream; it
is documented here as a known limitation rather than solved with a second blocking mechanism. A
follow-up would need `EditPlayerEngine` to expose whether the resampler is actually active per-session,
which does not exist today.

## Testing

- `WasapiAudioRenderer`: a new test confirms `PushSamples()` blocks when the ring is at capacity and
  that `Stop()` wakes a blocked push without hanging (bounded with a test-side deadline so a pacing
  regression fails the test instead of hanging the suite). This works without calling `Init()` (the
  ring is a producer/consumer queue independent of a real device), matching this file's existing
  convention of never depending on real WASAPI hardware in tests.
- `EditPlayerSession`: `video_queue` lives inside the class's private `Impl` (pimpl) with no seam to
  inject fake frames into it from a test, and the only way to make real frames flow through it is a
  real opened file feeding a real continuous decode thread — the same "no real file/hardware in unit
  tests" boundary `test_edit_player_engine.cpp` already draws (it covers only the nonexistent-file
  path today). `PollFrame()`'s actual selection/drop math is not new logic — it is a thin wrapper
  around the already-tested `SelectFrameForClock`, which fully covers the selection and positional-
  drop-count behavior on its own. What *is* independently testable without a file or device: `nullopt`
  handling when `HasAudioStream() == false` (both `PollFrame()` and `CurrentPositionMs()`), since that
  path is reachable on a never-opened session, matching this file's existing
  `ClosedSessionReportsNoAudioStream`-style tests.
- `AudioClockMs`/`SelectFrameForClock` themselves are already covered (Task 4, unchanged by this fix).
- No test depends on a real WASAPI render device or real video decode — live playback verification
  (does it actually look/sound right, is A/V sync correct) is a manual check for the user, same as
  every other live-audio verification in this project.

## Open questions (for the implementation plan, not blocking this spec)

1. Exact video-queue capacity (4 suggested above) and audio-ring capacity (1 second suggested above)
   are starting points, not pinned — fine to tune during implementation if unit tests or a live check
   show a better number.
