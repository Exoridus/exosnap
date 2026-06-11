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

Both backends deliver `ID3D11Texture2D` (BGRA) to the same VideoProcessorBlt → NVENC pipeline.
From NVENC's perspective the source is identical.

## Consequences

- Monitor recordings no longer interfere with VRR/G-Sync.
- No OS capture indicator for monitor recording.
- Direct GPU pipeline: `IDXGIOutputDuplication` → `ID3D11Texture2D` → VideoProcessorBlt →
  NVENC NV12.
- Access loss (`DXGI_ERROR_ACCESS_LOST`) on desktop lock/session switch terminates the recording
  session (same behavior as WGC `item.Closed`).
- Cursor compositing requires a small per-frame CPU pass (cursor region only, typically 32–64 px).
