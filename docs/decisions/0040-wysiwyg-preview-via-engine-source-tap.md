# ADR 0040: WYSIWYG Preview via Engine Source-Tap (Shared GPU Texture)

## Status

Accepted — implemented in the preview source-tap work.

## Context

The Record page shows a live preview of the capture target. Before recording it is
driven by the preview's OWN capture (`DxgiPreviewRenderer` runs a Windows Graphics
Capture session of the same target). Historically that capture kept running DURING
recording as well — a **second, independent capture** of the same source alongside
the engine's recording capture. That has three problems:

1. **Cost** — two GPU captures of the same monitor/window run concurrently.
2. **VSync coupling** — a second WGC session can re-introduce the DWM sync events
   ADR 0013 removed from the recording path.
3. **Diagnostic dishonesty** — the preview shows its own capture, not what the
   engine actually receives, so a black-screen / swap-chain bug in the recording
   is hidden behind a healthy-looking independent preview.

An earlier engine callback (`SetPreviewFrameCallback`) surfaced composed frames as
CPU BGRA byte vectors (an NV12 readback + fixed-point YUV→BGRA convert on the video
thread). It had zero consumers and used the wrong transport (per-frame CPU copies).

## Decision

During recording, the engine **shares its composited, pre-encode frame** with the
preview through a GPU texture, and the preview **stops its own capture**.

- **Tap point:** `vpInput` — the compositor output the encoder consumes, sampled
  BEFORE the NV12/P010 VideoProcessorBlt and BEFORE the RGB→AYUV 4:4:4 conversion.
  It already contains the cursor and webcam PiP exactly as recorded. This covers
  **SDR, HDR-tone-map, and 4:4:4** sessions (4:4:4 preview is re-enabled for free,
  since the tap precedes the AYUV pack). **Native HDR10** has no SDR intermediate
  (it encodes straight from an FP16 scRGB surface) and does not tap; the preview
  keeps its own capture there (approximate SDR — see KNOWN_LIMITATIONS).

- **Transport:** a producer-side D3D11 texture with
  `D3D11_RESOURCE_MISC_SHARED_NTHANDLE | SHARED_KEYEDMUTEX`, created lazily from
  the first `vpInput` so it matches its exact format (B8G8R8A8 or R10G10B10A2). The
  NT handle is handed to the consumer once. The producer publishes each composed
  frame with a **0 ms keyed-mutex acquire — dropping the preview frame on
  contention so the encode path is never stalled** (observation-only; encode output
  is byte-identical). Throttled to ~30 Hz.

- **Consumer:** `DxgiPreviewRenderer` opens the handle on its own device, copies the
  shared surface into a private texture under the keyed mutex (also 0 ms, decoupling
  present cadence from producer cadence), and samples it as the preview background.
  R10G10B10A2 is sampled straight into the 8-bit swap chain. When pushed mode is
  active the renderer's OWN webcam overlay is suppressed (the pushed frame already
  contains the PiP — avoids a double draw).

- **Switch-over:** the engine fires the shared-handle callback once, on the video
  thread, when the shared texture is ready. The app marshals it to the UI thread
  (no D3D on the callback thread) and, if a DXGI preview is active, hands the handle
  to the renderer. The render thread then opens it, **closes the preview's WGC
  capture graph** (device/swap chain stay alive), and renders the engine frames.
  Until the first pushed frame arrives the preview **holds its last WGC image** (no
  black flash on countdown). On stop the renderer is torn down and the normal WGC
  preview restarts via the existing `startPreviewIfIdle` machinery.

## Consequences

- No second capture during recording; no re-introduced VSync coupling.
- The preview reflects the engine's actual encoded content (diagnostic truth).
- WYSIWYG preview during **4:4:4** recording is restored (previously disabled).
- **Native HDR10** preview stays approximate (independent capture); documented.
- **Cross-GPU** handle sharing is unsupported: if the preview and engine devices
  resolve to different adapters, `OpenSharedResource1` fails and the preview holds
  its last live image. Recording is unaffected.
- The old CPU/NV12 `PreviewFrameCallback` + staging-ring path is removed. The
  frame-snapshot feature keeps its own NV12/P010 readback (yuv_to_bgra), which is
  why single-frame snapshots remain unavailable on the 4:4:4 (AYUV) path.
- Preview behavior while NOT recording is unchanged (its own WGC capture).
