# Editor Playback — Hardware-Accelerated Decode — Design

Status: proposed, 2026-08-03.

Builds on `2026-08-03-editor-playback-gpu-render-design.md` (GPU color-conversion path, ref-counted
`RawDecodedVideoFrame`, `EditFrameGpuConverter`, `EditPlayerRenderer`) and
`2026-07-11-editor-video-preview-spec.md` (originally scoped this as a deferred "Increment 2 —
D3D11VA + Echtzeit-Playback", never picked up since).

## Problem

The GPU-render-path design ruled out hardware decode for the case that motivated it (4:4:4 chroma
at 4K/240fps), stating "no vendor's D3D11VA decoder accepts 4:4:4." That statement was too broad:
it is true for H.264 (no hardware decoder in any vendor's current lineup, verified against
NVIDIA's own decode support tables), but **false for HEVC** — verified empirically on this
machine's RTX 5070 Ti (`ffmpeg -c:v hevc_cuvid` on a synthetic HEVC Main 4:4:4 8-bit clip decodes
cleanly; the same test against `h264_cuvid` on an equivalent AVC High 4:4:4 Predictive clip fails
every frame with `cuvid decode callback error`).

Separately, and more generally: today's editor playback decodes every clip on the CPU regardless
of chroma format — including the overwhelming majority of ordinary 4:2:0 clips that modern GPUs
decode natively. The GPU-render-path work already moved color conversion off the CPU; decode
itself, the more expensive of the two costs for typical H.264/HEVC material, has not.

## Decision

Add hardware-accelerated decode via FFmpeg's `AV_HWDEVICE_TYPE_D3D11VA` hwaccel, attempted
generically for every clip the editor opens (not narrowly gated to the HEVC/4:4:4 case), with a
clean, decided-once-at-open fallback to today's unmodified software decode path whenever hardware
doesn't support the stream's exact codec/profile/chroma/bit-depth combination.

### Why D3D11VA and not CUDA/NVDEC directly

`AV_HWDEVICE_TYPE_D3D11VA` reaches the same NVDEC silicon on NVIDIA GPUs as a direct CUDA/nvdec
hwaccel would, but through a vendor-generic Windows API this app already lives in end-to-end
(capture, GPU color conversion, encode, and the render host from the prior design are all
D3D11-based). It also works on Intel/AMD decoders where available, with no per-vendor gate — unlike
this codebase's current NVIDIA-only `VideoEncoderFactory` on the encode side. No reason to take on
a CUDA-specific path when the D3D11-generic one costs nothing extra and fits existing patterns
better.

### Why CPU-readback interop, not GPU-native texture sharing

Three interop depths were considered:

- **CPU readback** (chosen): hardware decodes, `av_hwframe_transfer_data` reads the frame back to
  system memory (NV12/P010), a small CPU-side step de-interleaves chroma into the three separate
  planes `RawDecodedVideoFrame` already expects, then the existing `WrapRawDecodedFrame`/
  `EditFrameGpuConverter`/`EditPlayerRenderer` pipeline runs completely unchanged.
- **GPU-native zero-copy**: decoder and `EditPlayerRenderer` share one video-capable, adapter-matched
  D3D11 device (mirroring `video_thread.cpp`'s existing capture/encode pattern); decoded frames stay
  as `AV_PIX_FMT_D3D11` textures end to end; requires a new NV12/P010-sampling shader path parallel
  to the existing three-plane one, plus a texture-backed variant of `RawDecodedVideoFrame`.
- **GPU copy + unswizzle pass**: same device-sharing requirement as zero-copy, but only a small new
  GPU pass splits NV12/P010 into the existing three-plane layout, leaving the conversion/tonemap
  shaders untouched.

Chosen CPU readback because the dominant cost being eliminated is decode itself (CPU-bound, and for
4K HEVC materially larger than the color-conversion cost the prior design already moved to the
GPU) — not the readback copy, which is one plane-sized memcpy-class operation, far cheaper than
software-decoding the frame would have been. This keeps the entire GPU-render-path implementation
(five tasks, two full review cycles) untouched, and defers the larger device-sharing/new-shader
surface to a later increment if the readback copy ever proves to be the bottleneck in practice.

### Capability check: let FFmpeg's own D3D11VA negotiation decide, not a hand-rolled DXVA table

Two ways to know if hardware supports a given stream before relying on it:

1. **(Chosen)** Set `hw_device_ctx` and a `get_format` callback preferring `AV_PIX_FMT_D3D11`, then
   let FFmpeg's own D3D11VA hwaccel initialization negotiate the exact decoder profile against the
   installed device (internally via `ID3D11VideoDevice::CheckVideoDecoderFormat`-equivalent calls)
   when the stream's parameters become known — this happens once, near stream start, before any
   picture data is actually decoded. Success or failure comes back as a single, clean signal — not
   the per-frame error storm this design's empirical test observed from the CUVID-specific decoder
   path (`h264_cuvid` accepted the open and then failed on every subsequent frame individually).
2. Query `ID3D11VideoDevice::CheckVideoDecoderFormat`/`GetVideoDecoderProfile` ourselves against a
   hand-built codec/profile/chroma → DXVA-GUID table, entirely before FFmpeg's decoder-open path
   runs. Purer (zero decoder-object creation), but duplicates a mapping FFmpeg already maintains
   internally, for no behavioral difference in this design.

Chosen (1): no new GUID-mapping table to build or keep in sync with FFmpeg's own, and the signal is
just as clean — one pass/fail, decided before any frame is decoded, not discovered mid-playback.

## Architecture

Hook point: `EditPlayerEngine::Open()`, `libs/recorder_core/src/edit_player_engine.cpp:553-576`
(video decoder open block), immediately before the existing `avcodec_open2(vctx, vcodec, nullptr)`
call, which itself is unchanged either way:

```cpp
AVCodecContext* vctx = avcodec_alloc_context3(vcodec);
bool vctx_ready = vctx != nullptr && avcodec_parameters_to_context(vctx, vst->codecpar) >= 0;
if (vctx_ready) {
    vctx->thread_count = 0;
    vctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    // New: attempt hardware decode. TryAttachD3D11VA sets vctx->hw_device_ctx and a get_format
    // callback preferring AV_PIX_FMT_D3D11 when it can create a D3D11VA device; returns false
    // (vctx left completely untouched) if no video-capable adapter is available at all -- the
    // per-stream profile/chroma negotiation itself happens lazily inside avcodec_open2/the first
    // avcodec_receive_frame, not here.
    TryAttachD3D11VA(vctx);
    vctx_ready = avcodec_open2(vctx, vcodec, nullptr) >= 0;
    if (!vctx_ready && vctx->hw_device_ctx != nullptr) {
        // Hardware negotiation rejected this stream's profile/chroma/bit-depth (or the device
        // vanished between creation and open). Fall back once, cleanly: drop hw_device_ctx and
        // retry with the exact call this codebase already makes today.
        av_buffer_unref(&vctx->hw_device_ctx);
        vctx_ready = avcodec_open2(vctx, vcodec, nullptr) >= 0;
    }
}
```

`TryAttachD3D11VA` is a new, self-contained helper (new file or alongside the decode-open logic in
`edit_player_engine.cpp`): creates an `AV_HWDEVICE_TYPE_D3D11VA` device via
`av_hwdevice_ctx_create`, installs a `get_format` callback returning `AV_PIX_FMT_D3D11` when
offered, `AV_PIX_FMT_NONE` fallback otherwise (the standard FFmpeg hwaccel pattern). No adapter
enumeration or vendor gating — `av_hwdevice_ctx_create`'s default behavior is sufficient; this
device is used only for decode capability negotiation and the readback below, not shared with
`EditPlayerRenderer`'s own device (see "Why CPU-readback" above).

### Data flow

Three call sites in `edit_player_engine.cpp` call `avcodec_receive_frame(vctx, frame.frame)` and
must handle a possible `AV_PIX_FMT_D3D11` result the same way:

- `DecodeForwardToTargetRaw` (`edit_player_engine.cpp:914`, scrub/seek path) — `avcodec_receive_frame`
  at line 955, `WrapRawDecodedFrame` call at line 932.
- The continuous-playback video thread (`edit_player_engine.cpp:~1274-1345`) — `avcodec_receive_frame`
  at line 1308, `WrapRawDecodedFrame` call at line 1342.
- `DecodeForwardToTarget` (line 835, the older BGRA thumbnail-strip path used by
  `TimelineThumbnailSource`, independently live per the GPU-render-path design's own finding).
  **Not optional** — `Open()` is shared code between the playback engine and the separate
  `EditPlayerEngine` instance `TimelineThumbnailSource` opens for itself, so once `Open()` can
  attach `hw_device_ctx`, ANY caller's decoder may hand back an `AV_PIX_FMT_D3D11` frame,
  including the thumbnail path. Without this same branch here, `ConvertToDecodedFrame` would read
  a hardware frame as if its `data[]` pointers were CPU-readable planes — a real correctness bug,
  not a missed optimization. Thumbnail generation gets hardware decode "for free" as a side effect
  of fixing this, not as separate scope.

At each site, a new `ReadBackD3D11Frame` helper runs first when `frame.frame->format ==
AV_PIX_FMT_D3D11`: `av_hwframe_transfer_data` into a freshly allocated CPU-side `AVFrame`, then
de-interleaves whatever semi-planar/packed chroma layout the hardware surface uses into three
separate planes matching
`DecodedPixelFormat::Yuv420P8`/`Yuv420P10`/`Yuv444P8` — i.e. it normalizes hardware output back
into exactly the shapes `RawDecodedVideoFrame` and `EditFrameGpuConverter` already handle. The
result replaces `frame.frame` for the rest of the existing call (`WrapRawDecodedFrame`/
`ConvertToDecodedFrame`, unchanged). `IsConvertibleFrame` (`edit_player_engine.cpp:696-700`) needs
no change — by the time it runs, the frame is always back in one of its three known software pixel
formats regardless of which path produced it.

**Not yet verified, deliberately not asserted here:** the exact DXGI surface layout
`av_hwframe_transfer_data` hands back for the HEVC RExt 4:4:4 case specifically. 4:2:0/4:2:2
(NV12/P010) surface *layout* is well-known; 4:4:4's D3D11VA surface layout is far less commonly
exercised in the wild and this design has not empirically confirmed it on real hardware, the same
way this session confirmed the CUVID pass/fail behavior directly instead of trusting
documentation. The implementation plan's first task should confirm this on real hardware before
`ReadBackD3D11Frame`'s de-interleave logic is written for the 4:4:4 case, the same way the prior
GPU-render-path plan pinned down exact struct layouts before parallel work started.

**Certain, not just unverified — the P010 numeric range needs an explicit fix, separate from the
4:4:4 layout question above.** `av_hwframe_transfer_data` hands 10-bit streams back as
`AV_PIX_FMT_P010LE`: FFmpeg/DXGI's documented convention left-justifies each 10-bit sample into a
16-bit word (`raw16 = sample << 6`). This codebase's `DecodedPixelFormat::Yuv420P10` is defined
against the opposite convention — plain values in `[0, 1023]`, "no P010 <<6 left-justification"
(`edit_frame_gpu_converter.cpp:33-34`) — and both consumers of it rely on that: the SDR path's
`y_scale`/`c_scale` divide by 1023/876/896 (`edit_frame_gpu_converter.cpp:222-223`), and the HDR
path's `DequantY10Limited`/`DequantC10Limited` (`hdr_pq.h:163-168`) expect codes in roughly
64-960. `ReadBackD3D11Frame` must therefore right-shift every 10-bit sample by 6 as part of
de-interleaving, not merely split the semi-planar plane into two — otherwise every
hardware-decoded 10-bit clip renders with badly wrong color (values ~64x too large), and any
HDR10 clip that reaches the PQ tonemap path reads garbage codes. Unlike the 4:4:4 layout, this
isn't something to verify empirically first; it's a known required conversion step and should be
implemented as such from the start.

## Blocker found during implementation: the shipped FFmpeg build has no hwaccel compiled in at all

Confirmed empirically 2026-08-03 while implementing the pieces above (`TryAttachD3D11VA`,
`SelectD3D11HwFormat`, the `Open()` wiring, `DeinterleaveHwReadbackFrame` with the P010 fix) and
exercising them against this machine's real HDR10 fixture clip (`hdr10_test_1080p60.mp4`, RTX 5070
Ti). `av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_D3D11VA, ...)` succeeds -- the device itself creates
fine -- and `avcodec_open2` succeeds with `hw_device_ctx` still attached. But the `get_format`
callback is invoked with **only** `AV_PIX_FMT_YUV420P10LE` in the offered list, never
`AV_PIX_FMT_D3D11` -- meaning libavcodec's HEVC decoder never advertises a hardware path for this
stream at all, so every frame decodes on the CPU exactly as before, regardless of anything in this
design.

`probe_edit_playback`'s Step F confirms the root cause directly:
`avcodec_get_hw_config()` returns null at index 0 for h264, hevc, AND av1 -- "no hw config
compiled in" -- while `av_hwdevice_ctx_create` for both D3D11VA and DXVA2 succeeds on their own
(the GPU/driver/OS side is fine). `avcodec_configuration()` confirms why: this project's own
minimal FFmpeg build ([[project_ffmpeg_build_repo]], `Exoridus/exosnap-ffmpeg-build`, currently
pinned to r5) is compiled with `--disable-everything` plus an explicit
`--enable-decoder=h264,hevc,av1,...` whitelist and **no** `--enable-d3d11va`, `--enable-dxva2`, or
`--enable-hwaccel=...` flags anywhere. The decoders themselves were never built with hwaccel
wiring, independent of what device or get_format logic this codebase supplies.

**This blocks the whole feature, not a corner case of it.** Every piece of code in this design --
`TryAttachD3D11VA`, the fallback retry, `DeinterleaveHwReadbackFrame` (including the P010 fix
above, now implemented and unit-tested against synthetic frames) -- is correct and behaves exactly
as designed: it degrades to today's software decode cleanly and safely, proven by the full
`test_edit_player_engine` suite staying green against real fixture clips including the HDR10 one.
But none of it can ever actually engage hardware decode on a machine running this FFmpeg build,
because the decoders it links against were never compiled with a hwaccel to offer.

**Fixing this is out of scope for this repository:** it requires a new release (`r7` or later) of
the separate `Exoridus/exosnap-ffmpeg-build` repo/CI pipeline with `--enable-d3d11va
--enable-dxva2` and the corresponding `--enable-hwaccel=...` entries for h264/hevc/av1 added to the
whitelist, a fresh cross-compiled artifact, and this repo's CMake pin bumped to it -- the same
release-and-verify workflow already used for r4 (decoders) through r6 (x264/x265, since reverted).
Until that lands, this design's C++ side is complete, tested, and safe to merge as dormant
scaffolding, but delivers zero performance benefit on any machine running the current r5 build.

## Fallback behavior

Decided once, at `Open()`, per opened stream — never mid-playback. If `TryAttachD3D11VA` cannot
create a device at all (no compatible adapter, driver issue), `vctx->hw_device_ctx` stays null and
the existing `avcodec_open2` call behaves exactly as it does today. If a device is created but the
stream's specific profile/chroma/bit-depth is rejected during hwaccel negotiation, `avcodec_open2`
fails once, is retried immediately without `hw_device_ctx`, and the whole clip plays back on
software decode — the same code path and behavior this codebase has today, for that one stream.
There is no mid-stream hardware-to-software (or software-to-hardware) switch; if hardware decode
starts failing unpredictably mid-playback despite passing this design's up-front negotiation, that
is a follow-up problem, not this design's.

## Testing

- A test proving software and hardware decode paths produce bit-identical `RawDecodedVideoFrame`
  planes for the same source clip (force each path, compare Y/U/V bytes) — the fallback must not
  silently change color-critical output.
- Hardware-path tests skip cleanly with no real D3D11-video-capable adapter present, same
  `GTEST_SKIP` convention the GPU-render-path work already established for its own
  hardware-required tests.
- `probe_edit_playback`'s existing Step F (`tools/probes/probe_edit_playback/src/main.cpp:473-539`,
  today diagnostic-only) already calls `av_hwdevice_ctx_create` for `AV_HWDEVICE_TYPE_D3D11VA` —
  the closest existing precedent for `TryAttachD3D11VA`'s device-creation call. The implementation
  should confirm whether the probe's device-creation code can be reused directly or only imitated;
  don't assume literal code sharing is possible without checking, since the probe is diagnostic-only
  and may not be structured as a reusable library function today.
- Live-verify (real playback, real hardware, real clips spanning both the HEVC-4:4:4-now-hardware
  and H.264-4:4:4-still-software cases) stays a human pass, per this project's CLAUDE.md rules
  around driving the running application — same handoff shape as the GPU-render-path design's own
  Step 7.

## Non-goals

- No GPU-native zero-copy texture interop (see "Why CPU-readback interop" above) — a later
  increment if the readback copy proves to be a real bottleneck.
- No mid-stream hardware/software fallback switching.
- No AMD/Intel-specific tuning or testing — D3D11VA is used generically, but this design is
  developed and verified against NVIDIA hardware only (matching this codebase's current
  NVIDIA-only stance on the encode side); other vendors get the same code path for free but are
  untested by this work.
- No separate scope for the thumbnail-strip path's hardware decode support — it is in scope by
  necessity (see "Data flow" above: `Open()` is shared, so `DecodeForwardToTarget` must handle
  `AV_PIX_FMT_D3D11` output for correctness, not as an added feature).
