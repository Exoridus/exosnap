# Editor Playback — GPU Render Path — Design

Status: implemented, 2026-08-03.

Builds on `2026-08-01-edit-player-decoupled-decode-design.md` (thread topology, frame-queue
backpressure, clock-gated conversion) and the throughput/threading measurements in
`tools/probes/probe_edit_playback`. That document fixed audio hitching and took CPU conversion
throughput from 51.6 to ~190-212 fps at 1440p60 — comfortably above the 60 fps the product ships
by default. This document addresses what is left: a real, reachable combination the CPU path
cannot sustain, and a structural decode inefficiency found while measuring it.

## Problem

The Edit page's Expert options allow 4:4:4 chroma (H.264/HEVC, 8-bit) and a free frame-rate entry
up to 240 fps independently of each other and of resolution — nothing blocks a 4K/240fps/4:4:4
recording, and no hardware decoder accepts 4:4:4, so such a clip is on the software decode+convert
path permanently, unlike every other chroma/resolution combination the product offers.

Measured 2026-08-03 (Release build, `probe_edit_playback` extended with a 4K case; same machine as
the 2026-08-01 reference numbers, so the two are directly comparable):

| | 1440p | 4K |
|---|---|---|
| 4:2:0 SIMD (the common case) | 2.90 ms/frame | — |
| 4:4:4 scalar | 14.53 ms/frame | 32.84 ms/frame |
| **4:4:4 SIMD** | 3.65 ms/frame | **8.22 ms/frame** |

Budget at 240 fps is **4.1667 ms/frame for the entire pipeline** (demux + decode + convert +
allocate + present). The SIMD conversion alone already spends roughly double that budget at 4K —
before decode, the ~2.3 ms/frame (1440p) `make_shared` allocation (scales with frame byte count,
so materially larger at 4K), or presentation are counted at all. This combination is not a
near-miss to tune; the CPU path cannot reach it.

**Separate finding, same measurement session:** step E shows the H.264 software decoder opened
with `ctx->thread_count=1`, `active_thread_type=0` on a 16-logical-core machine — FFmpeg's own
frame/slice threading is never enabled for the editor's decode. This is independent of the
conversion cost above (it is upstream of it) but is a plain, low-risk gap on the same code path,
so it is fixed alongside this design rather than filed separately.

## Decision

Move color conversion (and, for HDR10 clips, tone-mapping) off the CPU entirely, for every clip —
not a special case gated on resolution/chroma. This also removes the per-frame heap allocation the
CPU path pays today, since the GPU path never materializes a full CPU-side BGRA buffer.

Enable FFmpeg's decoder threading (`AVCodecContext::thread_count` = hardware concurrency, default
threading type) as a small, independent fix on the same decode path.

### Why GPU conversion and not hardware decode

Hardware decode (D3D11VA) was evaluated and set aside: it cannot help the case that motivates this
work, because no vendor's D3D11VA decoder accepts 4:4:4 — that source is CPU-decoded regardless of
this design. GPU-side *conversion*, by contrast, benefits every clip: it takes CPU-decoded YUV
planes (whether decode ever gains hardware acceleration or not) and moves the expensive,
bandwidth-bound part of the pipeline to the GPU, where this class of work already lives in this
codebase (see below).

### Architecture

`EditPlayerSurface` (`app/ui/widgets/EditPlayerSurface.{h,cpp}`) is today a plain `QWidget` with a
`paintEvent` that draws a `QImage`. It becomes a native child HWND, following the pattern already
proven by `PreviewSurface`/`DxgiPreviewRenderer` for the Record page's live preview: a native child
window with the Qt widget as host only. `DxgiPreviewRenderer.cpp` places its child HWND at
`HWND_BOTTOM` so that Qt siblings which own their own HWND still composite correctly above it — the
only condition on reusing this pattern, to be re-checked against the Edit page's actual widget tree
during implementation.

A new class, `EditPlayerRenderer`, owns a D3D11 device/context/swap chain and a dedicated render
thread. It is deliberately much smaller than `DxgiPreviewRenderer`: no Windows Graphics Capture
graph, no webcam PiP compositing, no cursor sprite, no snapshot-to-file path — just texture upload,
one conversion/tonemap shader pass, and `Present()`. `HdrToneMapper` (`gpu_hdr_tonemap.h`) and
`gpu_hdr_pq.h` already show the shape a standalone D3D11 shader-pass class takes in this codebase
(borrowed device/context, `Init`/`Convert`, lazily-cached SRV/RTV) and are the template for this
class, not code to share directly — the input here is planar YUV textures, not an already-linear
FP16 surface.

### Data flow change

`EditPlayerEngine`'s decode thread stops converting. Today it produces a `DecodedVideoFrame`
holding an owned, heap-allocated BGRA buffer (`edit_player_engine.cpp:702-758`,
`ConvertToDecodedFrame`); instead it hands off the decoded `AVFrame` itself, ref-counted via
FFmpeg's own `av_frame_ref`/`av_frame_unref` — no new allocation, since the frame already lives in
the decoder's own frame pool. This flows through the existing bounded-and-blocking frame queue from
the 2026-08-01 design unchanged in mechanism, only in payload: a ref-counted `AVFrame` handle is
smaller than a full BGRA buffer for every format below (4:2:0 8-bit: half the bytes; 4:2:0 10-bit:
about the same; 4:4:4 8-bit: three-quarters), so the existing byte-budget queue sizing
(`VideoQueueCapacityForFrameRate`, `playback_clock.h`) gets more headroom at the same memory bound,
not less.

The render thread pulls a queued frame, uploads its planes as GPU textures (one texture per plane;
reused/resized only when the clip's format or dimensions change, not per frame), and runs the
matching shader variant to write directly into the swap chain's back buffer.

### Pixel formats in scope

Exactly the three formats `edit_player_engine.cpp` already discriminates on today
(`IsConvertibleFrame`, `edit_player_engine.cpp:691-695`):

- `AV_PIX_FMT_YUV420P` (8-bit 4:2:0) — the common case.
- `AV_PIX_FMT_YUV420P10LE` (10-bit 4:2:0) — HDR10/PQ source when tagged as such
  (`IsPqTonemapSource`), ordinary BT.709/limited-range otherwise.
- `AV_PIX_FMT_YUV444P` (8-bit 4:4:4, Expert chroma option) — the case that motivates this design.

The color math itself (matrix/range handling, PQ tone-map curve) is not new: it already exists as
GPU shader code for the HDR10 case (`gpu_hdr_pq.h`/`gpu_hdr_tonemap.h`, currently used on the
recording/preview side) and as pinned CPU reference math for all three formats
(`yuv_to_bgra.cpp`, `hdr_tonemap.h`). This design ports that logic into shaders that read planar
textures instead of an already-composited FP16 surface; it does not re-derive any color science.

### Presentation cadence

The render thread replaces the current 33 ms UI-thread timer / `PollFrame()` polling
(the documented cause of the ~30 fps effective cap and 1/2-frame judder on 60 fps material) with
its own present loop, gated the same way the 2026-08-01 design gates conversion: a queued frame is
only uploaded and drawn if it is still at or before the playback clock; a frame the clock has
already passed is dropped before it touches the GPU. This is the same clock-comparison rule moved
one stage later in the pipeline (present-gated instead of convert-gated).

### FFmpeg decoder threading

`avcodec_open2` for the editor's decoders is changed to set `thread_count` to
`std::thread::hardware_concurrency()` (falling back to a small fixed value if that reports 0, as
the standard library allows) before opening, matching the threading the shipped decoders already
support (`ctx->codec->capabilities & AV_CODEC_CAP_FRAME_THREADS`/`SLICE_THREADS`, unexploited
today). This is decode-side only and independent of the render-path change above; it reduces decode
latency generally and narrows (without, on its own, closing) the 4:4:4/4K/240fps gap.

## Non-goals

- No hardware-accelerated *decode* (D3D11VA/NVDEC). Established above as not applicable to the
  motivating case; software decode stays as-is apart from the threading fix.
- No change to scrub/seek semantics, trim, markers, export, or the audio path — all independent of
  how a frame is converted or presented.
- No change to `TimelineThumbnailSource`'s decode path — it runs its own `EditPlayerEngine`
  instance for keyframe tiles, off the interactive playback path, and is not affected by this
  design.
- Does not itself guarantee real-time 4:4:4/4K/240fps playback — the numbers above show it removes
  the two biggest known costs (conversion, allocation), not that the resulting total necessarily
  clears the 4.17 ms/frame budget once GPU upload/present and decode-with-threading costs are
  counted. That is what the live-verify step below is for.

## Risks / things this touches that today's `QWidget` path handles for free

- **Placeholder text and letterboxing**: `EditPlayerSurface` currently draws a placeholder message
  and letterboxes via ordinary Qt painting when there is no frame. A native child HWND occludes Qt
  painting the same way `PreviewSurface`'s does, so both need to move into the renderer (solid-fill
  clear + a pre-rendered text texture, or similar) — the same problem `DxgiPreviewRenderer`'s OSD
  sprites already solve, reusable as a pattern.
- **Resize/DPI**: the child HWND and swap chain must track the widget's size and the window's DPI
  the way `PreviewSurface` already does; this is solved-but-not-shared code, not new ground.
- **Single-frame scrub display** (`DecodeFrameAt`, synchronous path): still produces one frame per
  scrub step, now via the same GPU upload/present path instead of a CPU convert — no separate code
  path needed, but worth calling out since scrub is latency-sensitive in a different way than
  continuous playback.

## Testing

- **Shader correctness**: this codebase already has headless, CI-safe GPU shader tests using a WARP
  (software) D3D11 device (`test_gpu_hdr_tonemap.cpp`, `test_gpu_rgb_to_ayuv.cpp`,
  `test_gpu_compositor.cpp`) — no real GPU required, so this is a pattern to reuse, not a new
  testing capability to build. The new conversion/tonemap shaders get the same treatment: render a
  synthetic planar-YUV texture through each shader variant on a WARP device and compare the output
  pixel-for-pixel (small tolerance for rounding, as the existing SIMD-vs-scalar tests already do)
  against the existing CPU reference functions (`ConvertFullPlanarYuv420ToBgra`,
  `ConvertFullPlanar444ToBgra`, `P010PqMonitorConverter`) they replace on the render path. The CPU
  functions stay in the tree as the pinned reference, not dead code.
- **Queue payload change**: `test_edit_player_engine.cpp`'s existing pure-function tests
  (`edit_playback_pacing.h`) are unaffected — they reason about queue occupancy and timing, not
  payload type. A new test confirms `AVFrame` ref-counting across the queue boundary (no leak, no
  use-after-unref) using a real short clip, the same boundary `probe_edit_playback` already
  exercises for the existing queue.
- **Decoder threading**: a probe-level assertion (extending step E) that `ctx->thread_count > 1`
  and `active_thread_type != 0` after the fix, plus confirming step B's throughput does not
  regress.
- **What cannot be unit- or probe-tested**: whether the finished pipeline actually sustains
  real-time playback of an actual 4:4:4/4K/240fps recording. No such file exists today (it is an
  extreme, likely rare Expert combination) and creating one needs real capture hardware capable of
  240 Hz capture — out of scope to fabricate for this design. The plan should end with the user
  producing (or attempting to produce) exactly such a recording and judging playback live, the same
  live-verify pattern already used for other perf-sensitive changes in this project. Ordinary
  60fps/4:2:0 playback smoothness is also worth a live check post-implementation, as a regression
  guard on the far more common case, even though the CPU path already cleared that bar.

## Cost / trade-offs, acknowledged up front

This is a materially larger change than a CPU-side tuning pass: a new native-window lifecycle
(create/resize/destroy child HWND, D3D device loss handling — the same class of failure
`DxgiPreviewRenderer` already has to handle, e.g. `DXGI_ERROR_DEVICE_REMOVED`), a new render thread,
and three new shader variants to validate. It replaces, rather than extends, the current
CPU-convert-then-`QImage`-paint path end to end, so the changeover is effectively a full swap of the
Edit page's video presentation layer, not an additive optimization sitting behind a flag.
