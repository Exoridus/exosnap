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

  That queue is bounded **twice**, and both bounds are necessary. The first is the clip's own frame
  rate: the depth has to cover the decode-ahead window, which is a fixed span of time and therefore
  a different number of frames at 60 fps than at 144. The second is a **memory budget**, because
  the queue holds decoded BGRA and a blocking producer makes the bound the steady state rather than
  a peak — the same 0.2 s window is ~235 MB at 1440p60 but over a gigabyte at 2160p120. Without the
  byte bound the depth is also only as trustworthy as the rate the container declares, and Matroska
  with a millisecond timebase routinely declares `r_frame_rate = 1000/1`. Sizing lives in
  `VideoQueueCapacityForFrameRate` (`playback_clock.h`) and is unit-tested there.
- **Demux** is paced by the **playback position**, not by queue occupancy: it reads until both
  streams are one second ahead of the clock, then waits for the clock to advance. With no clock
  available (throughput probes, video-only sessions) it never waits and runs at full speed.

  Pacing on occupancy instead was measured and rejected. A single demuxer reads in container
  interleave order, so a wait in front of a video packet also withholds the audio packets sitting
  right behind it. Once *any* occupancy bound saturates — and it saturates within tens of
  milliseconds, because the demuxer reads roughly a hundred times faster than a consumer drains —
  the demuxer's forward progress is gated 1:1 by the slowest consumer, which stamps that consumer's
  cadence onto the *other* stream's delivery. Raising the bound does not help: the extra headroom is
  spent in the first few tens of milliseconds and the coupling returns (measured: growing the bound
  from 1 s to 30 s raised the audio delivered during the probe's step H from 11.3 s to 41.5 s of
  media, and left the number of >50 ms delivery gaps at 74 of 74).

- **The packet queues** keep a **soft** capacity (1 s per stream, the normal buffer target) and a
  **hard** one (8 s / 8192 packets, a memory backstop that never binds in normal playback). At or
  above the soft capacity the demuxer keeps inserting as long as the *other* stream is below a
  250 ms low-water mark, and only genuinely waits at the hard capacity. The rule is symmetric —
  neither stream is privileged — and it is what lets the clock-paced read-ahead above actually
  govern while one consumer sits inside a long callback.

Together these buy the guarantee: audio packet delivery follows the clock, not the video
consumer's cadence, and a fully stalled video path still leaves audio the whole hard-capacity
read-ahead to keep going on rather than one second.

### Conversion follows the clock, not the file

The video thread converts a decoded frame to BGRA only when the frame is still in the future
relative to the audio clock. A frame whose presentation time has already passed is discarded before
the colour conversion runs — the conversion is the expensive part (2.9 ms per 1440p frame even
vectorised), and spending it on a frame that `PollFrame()` will drop anyway is pure waste.

This makes the expensive work scale with the **presentation rate** rather than the clip's frame
rate, which matters exactly where the old design hurt most: on a 144 Hz display, a 240 fps clip
needs 144 conversions per second, not 240.

The same rule has to apply to **audio**, for a different reason. A playback seek positions on the
keyframe at or before the requested start, so both streams begin decoding earlier than asked. Video
discards the difference by the rule above; audio has no clock comparison to make, because its
samples *are* the clock. Its preroll is therefore trimmed by timestamp instead — sample-accurately,
including the one block that straddles the boundary (`AudioPrerollFramesToDrop`). Handing the
untrimmed block over would start the sound at the keyframe while the clock is seeded to the
requested start, and video would lead audio by that gap for the entire run — up to a full keyframe
interval, 2 s at the product default, on every resume from a position mid-clip.

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

Two of the demuxer's waits depend on state that its own condition variable does not own — the
*other* queue's level, and the playback clock, which is a plain function with nothing to be
notified by. Both are therefore time-bounded (5 ms and 2 ms) and re-read the stop flag on every
iteration, so no notification or predicate mistake can turn either into a permanent stall. The
peer queue still notifies on every pop, so the timeout is a backstop and not the mechanism. The
queues are also peered through lock-free level mirrors rather than each other's mutexes, so the
two mutexes are never held at once and no lock cycle can exist.

## Non-goals

- No change to the clock itself, to frame selection, or to drop accounting — all of it is
  independent of how many threads produce the frames. `playback_clock.h` did gain two sizing/
  trimming helpers that the topology needs (`VideoQueueCapacityForFrameRate`,
  `AudioPrerollFramesToDrop`), but the clock arithmetic is untouched.
- No change to scrub/seek. `DecodeFrameAt` stays synchronous on a caller-owned worker, and
  `EditPlayerSession::Play()` keeps joining that worker before starting playback.
- No hardware decode here. It is the next slice and lands behind the same interface.

## Testing

- The three decisions the demuxer and the video thread make — "is this frame still worth
  converting", "may I read another packet yet", "may I insert past the soft capacity" — are pure
  functions in `edit_playback_pacing.h` and are unit-tested as such (`test_edit_player_engine.cpp`),
  including the boundary cases: soft capacity reached with a fed peer waits, soft capacity reached
  with a starving peer keeps going, hard capacity always waits, abort beats everything.
- Queue sizing and audio preroll trimming are likewise pure (`playback_clock.h`,
  `test_playback_clock.cpp`), covering the cases that only show up on other people's hardware:
  a 4K clip at a high frame rate staying inside the memory budget, a container declaring a
  nonsensical rate, and the audio block that straddles the requested start being cut on the
  right sample rather than dropped or kept whole.
- Thread topology itself has no unit-test seam without a real file and a real device — the same
  boundary `test_edit_player_engine.cpp` already draws. It is verified with
  `probe_edit_playback`, which drives the real engine on a real recording, and by live playback.
  Two paths inside it are therefore asserted only by construction and not by a test: the
  video-only fallback when the playback resampler fails to build (`PlaybackDeliversAudio`, which
  the session must consult instead of `HasAudioStream` before pacing on the audio clock), and the
  allocation-failure guards around both decode-thread bodies.
- The regression this exists to prevent — audio hitching when video is slow — is reproducible by
  measurement, which is the probe's step H job and not a unit test's: with the video callback
  throttled to 120 ms/frame for 10 s, audio block delivery must stay continuous.

  Measured on the 1440p60 reference clip:

  | | gaps > 50 ms | max gap | p50 gap | verdict |
  |---|---|---|---|---|
  | occupancy-paced demux (1 s bound) | 71 | 142.6 ms | 0.09 ms | FAIL |
  | occupancy-paced demux (30 s bound) | 74 | 141.4 ms | 0.02 ms | FAIL |
  | clock-paced demux + soft/hard queues | 0 (8 of 8 runs) | 34.2–37.7 ms | 16–21 ms | PASS |

  The shape of the failure is the tell: 71 gaps against 72 delivered video frames, i.e. one audio
  gap per video callback. After the change the median gap is roughly one Opus packet — audio
  arrives a packet at a time, paced by the clock, with the largest gap comfortably under the
  threshold and no run producing a single gap over it.

  Step B (maximum decode throughput) did not regress: 181.6 fps before, median 197.5 fps over the
  eight runs after (spread 121–218 fps — that spread is machine load, not the change; the low
  readings all came from runs immediately following a rebuild). Step B2's start/stop cycles kept
  terminating in ~320 ms each, every run.

## Cost, measured

Maximum decode throughput on the 1440p60 reference clip drops from ~209 to ~190 fps (medians of
five `probe_edit_playback` runs each, back to back on the same machine). The decoder already asks
libavcodec for one frame thread per logical core, so two further busy threads oversubscribe a
16-core machine, and the colour conversion is memory-bound rather than arithmetic-bound — the
added concurrent traffic costs it bandwidth.

That is the price of the property this exists to buy, and it is paid where it does not matter:
playback needs 60 presentations per second, not 190, and the clock-following conversion gate above
removes far more work than 9% in exactly the overloaded case where throughput would matter.
