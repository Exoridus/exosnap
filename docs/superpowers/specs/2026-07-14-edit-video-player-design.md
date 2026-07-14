# Edit Video Player — Design

Status: approved (brainstorming), 2026-07-14. Not yet implemented.

## Problem

`EditExportPage.cpp:233` shows a static `QLabel("Video preview — coming in 0.11", ...)` instead of
a real video. The trim-handle/playhead/scrub UI (#169) is fully built and correct — draggable
keyframe+marker-snapped trim handles, a scrubbing playhead, drag feedback with a time label — but it
runs against a synthetic clock (`preview_position_ms_`, driven by a `QTimer`/`QElapsedTimer` pair)
with no decoder or renderer behind it. This was deferred to 0.11 in-code, but the user was explicit
(2026-07-14 session) that this must land in **0.9**, not deferred. Export itself
(`EditExportPage::runExport()`, `RemuxToMkv`/`RemuxToProgressiveMp4`) is real, correct, stream-copy
only, and entirely unaffected by this work.

## Non-goals

- No in-app audio output device picker. Playback uses the system default render endpoint
  (`eRender`/`eConsole`), matching the same "system default" framing already used for capture
  defaults elsewhere in the product.
- No hardware-accelerated decode (NVDEC/D3D11VA). Software decode via `avcodec` is the 0.9 approach;
  hardware decode is deferred as a possible follow-up if software decode proves too slow on real
  hardware.
- No changes to marker editing, waveform data, or the Details rail — all already correct.
- No changes to export/remux logic — stream-copy export is untouched and does not depend on the
  decoder at all.
- No chapter export. Out of scope per the product spec.

## Prerequisite: companion FFmpeg build needs decoders

`cmake/VendorFFmpeg.cmake` vendors a **mux-only** FFmpeg build
(`Exoridus/exosnap-ffmpeg-build`, currently release `r3`) built with `--disable-everything` and an
explicit whitelist. The whitelist covers `avformat`/`avcodec`/`avutil`/`swresample` as libraries plus
specific demuxers/muxers/parsers/bitstream-filters — but **zero `--enable-decoder=...` entries**.
`avcodec` is present as a library, but no codec inside it can actually decode anything yet. `swscale`
is also not vendored at all (`VendorFFmpeg.cmake:12-13`), so any pixel-format conversion has to
happen without it.

Before any ExoSnap-side decode code can run, a new release of the companion repo (`r4`) is needed,
adding:

```
--enable-decoder=h264
--enable-decoder=hevc
--enable-decoder=av1
--enable-decoder=opus
--enable-decoder=aac
--enable-decoder=flac
--enable-decoder=pcm_s16le
--enable-decoder=pcm_s24le
--enable-decoder=pcm_s32le
--enable-decoder=pcm_f32le
```

This mirrors every codec ExoSnap can itself produce (product spec §4 video, §5 audio incl. lossless
PCM/FLAC bit depths). `VendorFFmpeg.cmake`'s `EXOSNAP_FFMPEG_VERSION` pin and URL/SHA256 are updated
to the new tag, same mechanics as the existing r1→r2→r3 history documented in that file's header
comment. This PR happens in the companion repo, prepared as part of this work, merged/released by
the user; the ExoSnap-side decode module cannot be implemented before that release exists.

## Architecture

### Engine: new decode module in `libs/recorder_core`

A new, UI-agnostic component (no Qt types in its interface — same posture as `mp4_remuxer` and
`yuv_to_bgra`, unit-testable without a window or audio device) that:

1. Opens the MKV master file (`EditContext::mkv_master_path`) via `avformat_open_input`, locates the
   video and audio streams, opens one `AVCodecContext` per stream via `avcodec_open2`.
2. Runs a single decode thread that reads packets sequentially and dispatches them to the video or
   audio decoder as appropriate.
3. Video frames land in a small, bounded frame queue (a handful of frames — enough to smooth
   playback without unbounded memory growth or added latency).
4. Audio frames are resampled to the WASAPI render device's format via `swresample` (already
   vendored) and written into a ring buffer that the render callback consumes.
5. **Seeking** (scrub or trim-handle drag): given a target timestamp, seeks to the keyframe at or
   before it (`AVSEEK_FLAG_BACKWARD`, the same approach `mp4_remuxer`'s existing trim logic already
   uses) and decodes forward to the requested frame. A newer seek request supersedes and cancels an
   in-flight older one — this is the "live, throttled" scrub behavior: the decoder always works
   toward the most recent drag position, never queues up stale ones.

### Video pixel path: extend `yuv_to_bgra`, do not reach for `swscale`

Software decoders for H.264/HEVC/AV1 produce fully-planar `YUV420P` (separate Y/U/V planes), not the
semi-planar NV12/P010 layout `recorder_core::ConvertYuv420ToBgra` currently expects (that helper was
written for the DXGI capture/encode surfaces, which are semi-planar). Since `swscale` is not
vendored and adding it means another companion-repo change, `yuv_to_bgra.h`/`.cpp` gets a sibling
struct + function for fully-planar YUV420 (separate Y/U/V pointers and strides) sharing the exact
same BT.709 matrix/range math as the existing semi-planar path — a variant of existing, tested code,
not a new parallel implementation.

### Audio playback: new WASAPI render client, audio-master clock

No WASAPI **render** path exists anywhere in the codebase today (only `IAudioCaptureClient` for
recording; `eRender` is used solely to resolve the *loopback capture* endpoint in
`wasapi_loopback.cpp`). This is genuinely new: an event-driven `IAudioRenderClient` on the system
default render endpoint, pulling from the decode module's audio ring buffer. The count of samples
actually rendered so far **is** the playback clock — this makes audio the master clock, matching
mpv/ffplay/VLC convention and the product's own "honest, measured, real drops" posture (product spec
§6: the recorder itself skips frames it cannot encode in time rather than silently drifting).

A UI-thread timer (same shape as the existing `preview_timer_`) polls the audio clock, pulls the
best-matching frame from the video queue, converts it via the extended `yuv_to_bgra` path, and paints
it. Frames older than the current audio position are dropped as real drops — no catch-up blending,
no silent resync.

**No audio stream in the file** (possible: a user can mute/disable every audio source while
recording, per product spec §7 "if every audio source is lost at once, the recording continues
video-only"): no render client is opened at all, and the video clock falls back to the **existing**
`preview_elapsed_`/`onPreviewTick` wall-clock logic — today's synthetic clock becomes the real
fallback path rather than being discarded.

### Threading & lifecycle

- `setEditContext()` tears down any previous decode session before opening the new file (same place
  that already resets `keyframe_timestamps_` and markers today).
- `hideEvent()` stops and joins the decode thread and the render client, mirroring the existing
  `export_thread_` join pattern used for export.
- `setPreviewPlaying(bool)` starts/stops both the decode thread's continuous-feed mode and the
  render client's `Start()`/`Stop()`. Scrubbing's existing pause/resume-if-was-playing behavior
  (`onScrubStarted`/`onScrubFinished`) is unchanged — it now pauses/resumes the real engine instead
  of the synthetic clock.

### UI integration

New `app/ui/widgets/EditPlayerSurface.{h,cpp}`, modeled directly on the existing `CameraPreview`
widget: takes `QImage` frames via `setFrame()`, paints them letterboxed inside the same rounded dark
panel aesthetic, shows a "Preview unavailable" placeholder when there is no frame. Replaces
`player_sub_` (the "coming in 0.11" label) in `EditExportPage.cpp:233`; `play_pause_btn_` and the
player frame's existing 16:9 aspect-ratio handling are unchanged, now driving a real engine instead
of only the clock.

### Error handling

If the file cannot be decoded (unexpected/corrupt codec — unlikely since ExoSnap only ever plays
back files it wrote itself, but not impossible), the player falls back to the "Preview unavailable"
placeholder rather than crashing. Trim, scrub-handle dragging, markers, and export are all
independent of decode success — export is pure stream-copy and never touches this new module.

## Testing

- Unit tests (no hardware, no Qt) for: keyframe-seek target resolution, the new planar-YUV420→BGRA
  conversion (extending the existing `yuv_to_bgra` test coverage), and audio-clock-from-sample-count
  derivation.
- A new `--visual-test` scenario exercises `EditPlayerSurface` with an injected still image (no real
  decoder in the harness, consistent with how other visual scenarios work) to pixel-verify letterbox
  layout and the placeholder state.
- No test depends on a real WASAPI render device or real video decode — live playback verification
  (does it actually look/sound right) is a manual check for the user, same as every other live-audio
  or live-recording verification in this project.

## Open questions (for the implementation plan, not blocking this spec)

1. Exact bounded queue depths (video frame queue size, audio ring buffer size) — an implementation
   tuning detail, not a design blocker.
2. Forward-decode cost cap during scrub if a file has an unusually large keyframe interval (the
   Advanced → Video keyframe-interval setting allows up to 2 s; at 60 fps that's up to ~120 frames to
   decode forward from the nearest keyframe for a single scrub position). Likely fine for
   H.264/HEVC software decode; AV1 software decode may be visibly slower — worth a real-hardware
   timing check once implemented, not something to pre-optimize speculatively.
3. WASAPI render buffer size/latency parameters — picked from Windows' recommended defaults during
   implementation, not pinned in advance.
