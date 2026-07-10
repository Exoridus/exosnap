# ADR 0041: Capture hubs and the held frame

## Status

Accepted — partially implemented. The WGC hub and its first consumer (the source
picker's tiles) have shipped. The DXGI hub, the lease wiring, and the FP16 tap
are designed but not built; this ADR records the model so those phases land
against a fixed decision rather than re-deriving it.

Design source: `docs/superpowers/specs/2026-07-10-capture-hubs-design.md`.
Related: ADR 0013 (OD for monitor capture, format policy), ADR 0040 (WYSIWYG
preview via engine source-tap).

## Context

Several services capture the screen independently. The source picker's tiles
(`ThumbnailCapture`), the Record page's live preview (`DxgiPreviewRenderer`), and
the QImage fallback (`PreviewService`) each opened their own Windows Graphics
Capture session. The recording engine captures a display through DXGI Output
Duplication in `video_thread`.

Two consequences followed. The same target was captured several times over, once
per consumer. And WGC blanks a capture when another application takes the surface
— the Snipping Tool is the everyday case — so a picker tile or a preview went
empty while a recording of the same display would not have.

The webcam had this exact shape until a shared capture hub replaced its two
competing readers (#147). This applies that pattern to the rest.

## Decision

**One hub per capture technology, keyed by source.** A hub owns exactly one
capture per key, counts its consumers, retains the last frame, classifies loss,
and reconnects without a deadline.

- The **arbitration is pure** (`recorder_core::StepCaptureHub` +
  `ResolveHubFrame`): no D3D, no WGC, no threads, no Qt. The hub classes are thin
  drivers that apply the decision and perform the flagged actions. This is what
  makes the reconnect and hold behaviour unit-pinnable at all — the webcam's
  identical loop is untestable to this day for want of that seam.
- **Sharing is the mechanism; frame control is the point.** Because the hub owns
  the only capture for its key, it is the only party that can tell "the source
  produced nothing" apart from "someone took the source away", and therefore the
  only party that can hold the last good frame instead of handing out an empty
  surface. No consumer can do this for itself.
- **Loss is held, not blanked.** A consumer sees a still image, never an empty or
  black one. Reopen retries are unbounded; only the last consumer leaving stops
  them. A source that never produced a first frame reports "no frame yet", which
  the consumer must be able to tell from a held frame.
- **A per-key registry, born with the first consumer and discarded with the
  last.** No idle capture stays open behind a source nobody watches.

### The one-duplication-per-process constraint

An output can be duplicated **once per process** (measured on an RTX 5070 Ti: the
second attempt answers `E_INVALIDARG`, on any device, not the documented
`DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`; releasing the first and duplicating again
succeeds). This does not bite today — only the engine opens a duplication, one
session at a time — but it is the reason the DXGI hub must hand a display to the
engine before the engine takes it. `StepCaptureHub` emits `close_capture` and
`grant_lease` from a single decision, so no interleaving can put them in the
wrong order.

The lease API (`RequestLease` / `ReturnLease` / `ForwardFrame`) is implemented
and unit-pinned but **has no caller yet**. It is deliberately pre-built: the
constraint it enforces is load-bearing for the DXGI hub, and pinning the ordering
now is cheaper than reconstructing it when Phase 4/5 wire it in. It is dead code
by design, not by oversight, and removing it would only cost the tests that hold
the ordering.

### The WGC hub ships first, and alone fixes the blanking

WGC allows several captures of one source, so the WGC hub arbitrates nothing. It
earns its place for the other reason a hub exists — owning the only capture is
what makes holding the last frame possible. It opens no duplication, carries none
of the idle-duplication risk, and fixes the user-visible blanking on its own.
Picker display tiles route through the WGC hub, not the DXGI hub, because routing
them to DXGI would hold a duplication open for every visible display tile at once.

### The engine keeps its own drain

The hub arbitrates who may duplicate an output; it does **not** pump frames for
the recording. The video thread keeps its `AcquireNextFrame` loop verbatim —
format negotiation, phase-correct pacing, cursor shape fetch, diagnostics taps,
and `ClassifyOdAcquireFailure` / `DecideOdReopen` recovery. Re-homing that onto a
hub thread would put a queue between capture and CFR scheduling and buy nothing.
Given no hub (tests, headless), the engine opens its own `DxgiOdCaptureSrc`
exactly as today.

## What shipped

- `recorder_core::StepCaptureHub` / `ResolveHubFrame` — pure arbitration.
- `CaptureSourceHub` — drives the policy; injectable `HubSourceProducer`.
- `CaptureHubRegistry` + `CaptureSubscription` — one hub per `CaptureSourceKey`,
  refcounted.
- `WgcSourceProducer` — a `HubSourceProducer` over `Direct3D11CaptureFramePool`.
  Drains to the newest queued frame (WGC hands back the oldest), copies it into a
  producer-owned texture (the pool recycles its surfaces), and classifies
  `item.Closed` / device-loss.
- `ThumbnailCapture` — rewritten from a one-shot-per-tile capture into a hub
  consumer. A tile holds its last image through a Snipping Tool session; a source
  that never produced reports itself unavailable. Readback shrinks the frame on
  the GPU (mip chain, `ThumbnailMip::ChooseMipLevel`) so the CPU never sees the
  source resolution.

The lease API and the DXGI hub are **not** wired. No idle duplication is opened.

## Consequences

- The picker's blanking is fixed with no duplication ever opened, which is the
  cheapest visible win in the design and independent of the risky later phases.
- Held frames are user-visible behaviour and are recorded in
  `docs/product-spec.md`.
- The FP16 preview tap for native HDR10 (Phase 1) and the DXGI hub with its idle
  duplication (Phase 4) remain open. Phase 4 carries an explicit off-ramp: if its
  hardware probe shows degraded DWM / fullscreen behaviour beside a running game,
  the preview stays on WGC — by then a WGC *hub* consumer, so the hold survives
  either way. See KNOWN_LIMITATIONS.

## The latent bug fixed on the way

`PreviewService` and `WebcamService` posted their frame callback to
`QCoreApplication::instance()`, binding delivery to the application's lifetime
rather than the receiver's. A frame in flight while the Record page died
dereferenced freed memory. Both services now post to a receiver object, so Qt
drops the event when the receiver is gone.
