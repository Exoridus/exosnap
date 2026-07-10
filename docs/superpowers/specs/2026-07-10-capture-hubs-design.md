# Capture hubs — design

Status: approved for planning
Date: 2026-07-10

## Why

Three services capture the screen independently: `ThumbnailCapture` for the source
picker's tiles, `DxgiPreviewRenderer` for the Record page's live preview, and
`PreviewService`. All three open their own Windows Graphics Capture session, and all
three do so with `CreateForMonitor` — even for a display. The recording engine, meanwhile,
captures a display through DXGI Output Duplication.

Two consequences follow.

A display preview and the recording of that same display come from **different capture
backends**. WGC blanks a capture when another application takes over the surface — the
Snipping Tool is the everyday case — so a preview and a picker tile go empty while the
recording of the same display would not have. And the same target is captured several
times over, once per consumer.

The webcam had this exact shape until a shared capture hub replaced its two competing
readers. This applies that pattern to the rest.

## The measured constraint

An output can be duplicated **once per process**. Measured on an RTX 5070 Ti against
`\\.\DISPLAY6`:

```
two duplications, one device      → first S_OK, second 0x80070057 E_INVALIDARG
two duplications, two devices     → second 0x80070057 E_INVALIDARG
release the first, duplicate again → S_OK
```

Not the documented `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`, and a second D3D device does not
help. This makes a DXGI hub **necessary rather than merely efficient**: while the engine
records a display, nothing else in the process can duplicate it.

WGC carries no such restriction. Sharing there buys stability and efficiency, not
correctness.

## Model

One hub per capture technology, keyed by source. A hub owns exactly one capture per key,
counts its consumers, retains the last frame, classifies loss, and reconnects without a
deadline — the shape already proven by `WebcamService` and `ClassifyOdAcquireFailure`.

| Hub | Key | Consumers |
|---|---|---|
| DXGI | monitor (stable GDI device name) | Record preview (display, region), picker display tiles |
| WGC | window (HWND) | Record preview (window), picker window tiles |
| Webcam | device id | Record PiP, Settings preview |

A consumer never opens a capture. It subscribes, receives frames, and on unsubscribe the
hub closes the capture once the last consumer leaves.

### Loss is held, not blanked

When a source stops producing — a window minimised, a PID temporarily uncapturable, a
display renegotiating — the hub keeps serving the last frame it received and retries
underneath. A consumer sees a still image, never an empty one. This is what
`WebcamService::TryGetFrame` already guarantees for the camera.

### Handing a display to the engine

Because a display may only be duplicated once, the hub must give it up before the engine
takes it, and get it back afterwards. The sequence runs on the UI thread, before the
recording thread starts, so it is ordered by construction rather than by luck:

1. Recording is requested for display X.
2. The hub releases its duplication of X. Consumers of X keep seeing the held frame.
3. The engine opens its own duplication of X and begins recording.
4. The engine publishes its composited, pre-encode frame through the existing shared
   NT-handle tap. The hub switches X from producer to consumer and forwards those frames.
5. On stop, the engine releases; the hub duplicates X again and resumes producing.

Displays other than X are unaffected: the restriction is per output.

Consumers of X therefore see the *recorded* frame during recording — cursor and webcam
composited in. That is the WYSIWYG the preview already aims for.

**Native HDR10 has no tap.** There is no SDR intermediate to share, so during a native
HDR10 recording the hub cannot forward frames for X. It holds the last frame. Falling back
to a parallel WGC capture for that display is permitted (WGC allows it) and gives a live
but colour-approximate image; HDR preview is already documented as approximate. Which of
the two is chosen is a UI decision, not an engine one.

## Components

- `DxgiSourceHub` — owns `DxgiOdCaptureSrc` per monitor; `Yield(monitor)` / `Reclaim(monitor)`
  for the engine handover; forwards engine frames while yielded.
- `WgcSourceHub` — owns one frame pool per HWND.
- `WebcamService` — already the shape; grows the same subscribe/hold interface so the three
  read alike.
- `ThumbnailCapture`, `DxgiPreviewRenderer`, `PreviewService` — become consumers. The
  display path moves from WGC to DXGI, which is what fixes the blanking.

The engine is untouched and stays UI-agnostic. It keeps opening its own capture and keeps
publishing through the NT-handle tap it already has.

## Testing

Pure policy, unit-pinned, no GPU:

- consumer counting: the capture opens on the first subscriber and closes on the last
- a source that stops producing yields the held frame, never an empty one
- the handover order: a hub holding display X must have released before the engine opens
- reclaim after stop restores production
- loss classification and unbounded retry (mirrors the existing OD and webcam classifiers)

The one-duplication-per-process constraint is asserted by a probe, not assumed.

## Limits

Whether the handover is imperceptible — whether the held frame reads as a freeze or a
glitch — can only be judged by a human watching a real recording start on a real display.

## Not in scope

Whether webcam device selection moves into the source picker as a fourth tab is a product
decision, tracked separately. The hub does not depend on it.
