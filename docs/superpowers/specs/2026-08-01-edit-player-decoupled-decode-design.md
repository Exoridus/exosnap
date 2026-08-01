# Edit Video Player — Decoupled Decode — Design

Status: implemented, 2026-08-01.

Supersedes the threading model described in
`2026-07-14-edit-video-player-pacing-design.md` (that document's audio-ring pacing, clock and
frame-selection math all stay exactly as they are — only "one decode thread serves both streams"
changes). The measurements that motivated this are in
`tools/probes/probe_edit_playback`.

## Problem

Audio in the editor drops out in short, repeated hitches — not a tonal defect, a stream of small
gaps. The cause is structural, not a shortage of speed.

`EditPlayerEngine::StartPlaybackDecode` runs demux, video decode, colour conversion, audio decode
and audio resampling on **one thread**. The WASAPI ring is only refilled when that thread gets
around to it, and `WasapiAudioRenderer` renders silence into any gap
(`wasapi_audio_render.cpp:403-407`) while letting the clock run on. So *any* video-side hitch — an
unusually large keyframe, a page fault, another process taking the core — becomes an audible hole
plus a visible jump, because the clock moved while no frames were produced.

Throughput work has already taken this path from 51.6 to 212.6 fps on a 1440p60 clip, which makes
hitches rarer. It cannot make them impossible: one slow frame is still one hole. Two cases make
that unavoidable rather than unlikely:

- **4:4:4 recordings.** No hardware decoder accepts 4:4:4, so those clips are on the software path
  permanently.
- **High frame rates.** 120 fps is now offered in Settings, and the product's frame rate is a free
  value in the model. At 240 fps the per-second decode+convert workload is four times the 60 fps
  case that the current design was sized against.

## Decision

**Audio playback must not depend on video keeping up.** That is a property of the thread topology,
not of how fast any single stage runs, so it is fixed there.

### Thread topology

```
        ┌─────────────┐   video packets   ┌──────────────┐  frames  ┌────────────┐
        │             ├──────────────────►│ video decode ├─────────►│ frame queue│──► PollFrame()
av_read │   demux     │                   │  + convert   │          └────────────┘
────────►   thread    │                   └──────────────┘
        │             │   audio packets   ┌──────────────┐
        │             ├──────────────────►│ audio decode ├─────────►  WASAPI ring
        └─────────────┘                   │  + resample  │
                                          └──────────────┘
```

Three threads replace one. The demuxer owns the `AVFormatContext` exclusively; each decode thread
owns its own `AVCodecContext` exclusively. No context is touched by two threads, which keeps the
engine's existing single-writer contract intact rather than bolting locks onto it.

### What paces what

- **Audio** is paced by the WASAPI ring exactly as today: `PushSamples()` blocks when the ring is
  full. That now blocks *only the audio decode thread*, which is the whole point.
- **Video** is paced by the frame queue, which changes from drop-oldest-never-blocks to
  **bounded-and-blocking**. The video decode thread waits when the queue is full and is woken by
  `PollFrame()` draining it, or by stop. Presentation therefore paces decode, and a video thread
  that falls behind cannot race through the file converting frames nobody will see.
- **Demux** is paced by both packet queues, each bounded by buffered media duration
  (target: 1 s per stream). The demuxer waits when the queue it must push into is full.

The one-second packet buffer is what buys the guarantee: if video decode stalls entirely, audio
still has roughly a second of packets queued ahead of it. Every hitch this design is meant to
absorb is orders of magnitude shorter than that.

### Conversion follows the clock, not the file

The video thread converts a decoded frame to BGRA only when the frame is still in the future
relative to the audio clock. A frame whose presentation time has already passed is discarded before
the colour conversion runs — the conversion is the expensive part (2.9 ms per 1440p frame even
vectorised), and spending it on a frame that `PollFrame()` will drop anyway is pure waste.

This makes the expensive work scale with the **presentation rate** rather than the clip's frame
rate, which matters exactly where the old design hurt most: on a 144 Hz display, a 240 fps clip
needs 144 conversions per second, not 240.

Under sustained overload the decoder itself is allowed to skip work: `AVCodecContext::skip_frame`
is raised to `AVDISCARD_NONREF` while the video thread is behind and lowered again once it catches
up. Non-reference frames are the ones nothing else depends on, so skipping them degrades smoothness
without corrupting later frames.

### Shutdown ordering

Stopping tears down in producer-to-consumer order — demux, then video, then audio — with every
blocking wait woken explicitly before its thread is joined: packet-queue waits by a stop flag plus
condition variable, the frame-queue wait the same way, and the audio ring by
`WasapiAudioRenderer::Stop()` (already the case, and the reason `EditPlayerSession::Pause()` calls
`audio.Stop()` before stopping decode). Any thread still blocked when its join runs is a hang, so
each wait predicate must include the stop flag rather than only the space/data condition.

## Non-goals

- No change to the clock, frame selection, or drop accounting (`playback_clock.h`) — all of it is
  independent of how many threads produce the frames.
- No change to scrub/seek. `DecodeFrameAt` stays synchronous on a caller-owned worker, and
  `EditPlayerSession::Play()` keeps joining that worker before starting playback.
- No hardware decode here. It is the next slice and lands behind the same interface.
- No 4:4:4 support here. It is a separate, smaller change (`IsConvertibleFrame` plus a converter
  for the layout) that this design makes worth doing.

## Testing

- The queue bound and the "is this frame still worth converting" predicate are pure functions and
  are unit-tested as such, alongside the existing pacing math.
- Thread topology itself has no unit-test seam without a real file and a real device — the same
  boundary `test_edit_player_engine.cpp` already draws. It is verified with
  `probe_edit_playback`, which drives the real engine on a real recording, and by live playback.
- The regression this exists to prevent — audio hitching when video is slow — is reproducible by
  measurement: with the video thread artificially throttled, audio block delivery must stay
  continuous. That is the probe's job, not a unit test's.

## Open question

Packet-queue capacity is stated as 1 second per stream. That is a starting value chosen to be
comfortably longer than any hitch this addresses, not a measured one; the probe can report the
high-water mark actually reached and it can be tuned from that. It is shipped unmeasured.

## Cost, measured

Maximum decode throughput on the 1440p60 reference clip drops from ~209 to ~190 fps (medians of
five `probe_edit_playback` runs each, back to back on the same machine). The decoder already asks
libavcodec for one frame thread per logical core, so two further busy threads oversubscribe a
16-core machine, and the colour conversion is memory-bound rather than arithmetic-bound — the
added concurrent traffic costs it bandwidth.

That is the price of the property this exists to buy, and it is paid where it does not matter:
playback needs 60 presentations per second, not 190, and the clock-following conversion gate above
removes far more work than 9% in exactly the overloaded case where throughput would matter.
