# ADR 0013: DXGI Output Duplication for Monitor Capture

## Status

Accepted — implemented in DXGI-OD-MONITOR-CAPTURE work (see project memory).

## Context

Monitor capture via WGC (`GraphicsCaptureSession`) causes two observable problems:

1. **VRR/G-Sync interference** — WGC synchronizes its frame-acquisition events with DWM, which
   forces periodic hard-VSync events visible as screen dimming/pulsing at the capture frequency.
   The effect is most pronounced on high-refresh-rate displays with VRR enabled.
2. **Yellow capture indicator** — WGC displays an OS-level orange/yellow border around the
   captured monitor. While `IsBorderRequired(false)` suppresses this on supported Windows builds,
   the underlying sync events remain.

Both issues stem from WGC being built for general compositor-aware capture rather than passive
GPU-buffer read.

## Decision

For `CaptureTarget::Kind::Monitor` targets, use `IDXGIOutputDuplication` (DXGI Output Duplication)
as the capture backend instead of WGC.

For `CaptureTarget::Kind::Window` targets, keep WGC (`GraphicsCaptureSession`) — it is the only
supported API for window and application capture.

### Why DXGI OD solves the problems

| | DXGI OD | WGC |
|---|---|---|
| VRR interference | None — reads GPU output buffer passively | Yes — DWM sync events at capture rate |
| Capture indicator | None | Yes (suppressed via `IsBorderRequired=false` on supported builds) |
| Performance | Direct `ID3D11Texture2D`, zero copy | Extra WGC pipeline overhead |
| Window/app capture | Not supported | Supported |
| Cursor in frame | Manual compositing required | Built-in via `IsCursorCaptureEnabled` |

### Cursor compositing

DXGI OD does not composite the cursor into the frame. When `RecorderConfig.capture_cursor = true`,
`VideoThread` manually composites the cursor using:

- `DXGI_OUTDUPL_FRAME_INFO.PointerPosition` for cursor position
- `IDXGIOutputDuplication::GetFramePointerShape()` for the cursor bitmap
- CPU alpha-blend over the captured frame region via `CopySubresourceRegion` + `Map` +
  `UpdateSubresource`

Supported cursor types: `DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR`,
`DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR`.
Unsupported: `DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME` (rare legacy cursor; silently skipped).

## Implementation

- **New files**: `dxgi_od_capture_src.h/.cpp` — `DxgiOdCaptureSrc` class wrapping
  `IDXGIOutputDuplication`
- **Modified**: `video_thread.cpp` — `useOdCapture` flag branches between DXGI OD and WGC paths
- **Modified**: `video_thread.cpp` — adapter-matched `D3D11CreateDevice` for Monitor targets
  (multi-GPU correctness)
- **Modified**: `CMakeLists.txt` — registers new source file

Both backends deliver `ID3D11Texture2D` to the same VideoProcessorBlt → NVENC pipeline.
From NVENC's perspective the source is identical.

### Capture-format negotiation (fix/od-10bit-desktop)

The desktop framebuffer duplicated by OD is **not always BGRA8**: a 10 bpc SDR
desktop (e.g. NVIDIA "Output color depth: 10 bpc") composites in 10-bit /
FP16, and an HDR/Advanced-Color desktop in FP16 scRGB. Measured behavior on a
10 bpc SDR system (Windows 11, NVIDIA, both displays SDR with
`BitsPerColor=10`):

- `DXGI_OUTDUPL_DESC.ModeDesc.Format` reports `R16G16B16A16_FLOAT` while the
  frames the legacy `DuplicateOutput` actually delivers are a **BGRA8 stream
  with occasional interleaved FP16 frames**. `ModeDesc` is therefore *not*
  trusted for format decisions.
- `IDXGIOutput5::DuplicateOutput1` with `{B8G8R8A8_UNORM, R10G10B10A2_UNORM}`
  returns `DXGI_ERROR_UNSUPPORTED` on the FP16-composited desktop; on a
  natively R10G10B10A2-composited desktop it delivers 10-bit frames directly.
- The D3D11 VideoProcessor converts `R10G10B10A2 (G22 full) → NV12/P010
  (studio BT.709)` natively (verified via
  `CheckVideoProcessorFormatConversion`, see `probe_hdr`); FP16 scRGB → NV12
  is NOT supported by the driver.

Decisions:

1. **Open**: prefer `IDXGIOutput5::DuplicateOutput1` with
   `{B8G8R8A8_UNORM, R10G10B10A2_UNORM}`; fall back to legacy
   `DuplicateOutput` (BGRA8 compatibility stream) when it fails.
2. **Negotiate from frames, not ModeDesc**: the session capture format (and
   `odCapturedTex`, the pacing ring, and the GPU compositor render target) is
   created lazily from the FIRST acquired frame's texture desc. Supported:
   BGRA8 and R10G10B10A2. The VideoProcessor input color space stays
   `RGB_FULL_G22_NONE_P709` for both (10 bpc SDR desktops are G22-encoded).
3. **Honest errors instead of silent starvation**: a first frame in an
   unsupported format (e.g. FP16 on a true HDR desktop) records an explicit
   `ErrorPhase::VideoCapture` failure naming the format. Previously a format
   mismatch made `CopyResource` a silent no-op, the encoder starved, and the
   session died at stop with the opaque
   "Codec private data not available at mux start" mux error.
4. **Foreign-format frames are skipped, not fatal**: mid-session frames whose
   format differs from the negotiated one (the measured Windows quirk above)
   are released and counted as coalesced drops (one structured warn log per
   session); the negotiated stream plus CFR duplication keeps the recording
   healthy.
5. **First-frame seeding**: the first captured frame (OD *and* WGC) is seeded
   into the encode loop. Both backends only deliver frames when the source
   repaints; without the seed, a static monitor (phase-correct pacing ignored
   the wait-loop frame in `odCapturedTex`) or a static window (the first WGC
   frame was discarded by the wait loop) encoded zero frames for the whole
   session and produced the same opaque mux error.

HDR capture (FP16 scRGB, PQ/BT.2020) remains out of scope — a separate
pipeline (ADR 0032 scope note).

## Consequences

- Monitor recordings no longer interfere with VRR/G-Sync.
- No OS capture indicator for monitor recording.
- Direct GPU pipeline: `IDXGIOutputDuplication` → `ID3D11Texture2D` → VideoProcessorBlt →
  NVENC NV12.
- Access loss (`DXGI_ERROR_ACCESS_LOST`) on a mode/topology change (desktop lock/session switch,
  refresh/HDR switch, or a shared monitor re-negotiating when another display wakes) is transient:
  the D3D device is still alive, so the duplication is rebuilt in place and the **same encode session
  and output file continue**, leaving only a gap the size of the blackout. The rebuild polls for the
  output under a bounded recovery budget (a few seconds); if the output never comes back within it,
  the recording ends cleanly (source-loss → finalise), the historic behaviour. If the fresh
  duplication returns a different frame size or format, the per-frame guard ends the recording
  cleanly rather than reconfiguring the encoder mid-session (documented limitation). Unrecoverable
  losses (`DXGI_ERROR_DEVICE_REMOVED`/`_HUNG`/`_RESET` or any unexpected HRESULT) still terminate the
  session immediately with the HRESULT surfaced to the app log.
- Cursor compositing requires a small per-frame CPU pass (cursor region only, typically 32–64 px).
