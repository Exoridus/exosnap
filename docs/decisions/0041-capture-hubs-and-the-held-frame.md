# ADR 0041: Capture hubs and the held frame

## Status

Accepted — implemented. The WGC hub and its first consumer (the source picker's
tiles) shipped first; the FP16 preview tap, the DXGI hub with the idle display
preview as its consumer, and the lease wiring have since landed. The
idle-duplication hardware probe (MPO / fullscreen behaviour beside a running
game) is still outstanding and remains the off-ramp for the DXGI hub's idle
duplication specifically — the rest does not depend on it.

**Amended for v0.9.** The first consumer named above is gone. The shipping source
picker is a named list of displays and windows with no image path at all (see
`docs/product-spec.md` §7), so `ThumbnailCapture` and the `ThumbnailMip` helper it
read back through were removed as unreferenced code. The hub architecture this ADR
decided is unaffected and still carries the preview: what changed is which
consumers exist, not how a hub arbitrates or what a held frame means. The
`ThumbnailCapture` bullet below is kept as the record of what was built.

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

The lease API (`RequestLease` / `ReturnLease`) is wired: the coordinator fires a
blocking release hook from its recording preparation worker thread, immediately
before the engine opens its capture — after every validation, guard, and
cancellation checkpoint, so a rejected or cancelled start never touches the idle
feed — and the Record page returns the lease at Ready/Completed/Failed, where the
pushed-source revert already lives. The subscription and the held frame survive
the lease, so both hand-overs read as a hold, never a flash. `ForwardFrame`
remains callerless: during a recording the preview consumes the engine's
WYSIWYG tap directly (ADR 0040), so there is no hub-side fan-out to feed.

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
- **The FP16 preview tap** (`ResolvePreviewTapPlan`, `PreviewTapDesc`) — a native
  HDR10 session shares its linear scRGB pre-encode surface and the preview
  tone-maps it on its own render thread with the same `HdrToneMapper` the
  tone-mapped recording path runs. The one untapped session is the already-PQ
  R10G10B10A2 desktop, which has no linear surface to share.
- **`DxgiSourceProducer`** — a `HubSourceProducer` over `DxgiOdCaptureSrc`. Owns
  an adapter-matched device recreated per (re)open (which is what makes even a
  `DEVICE_REMOVED` recoverable here, unlike the engine mid-session), resolves the
  stable GDI device name back to the current `HMONITOR`, and paces its own reopen
  attempts under the hub's unbounded retry.
- **`DxgiCaptureHubService`** — the DXGI hub's home: its own ~60 Hz pump thread,
  a per-key registry (in practice one key: the previewed display), and the
  publisher that feeds the preview renderer's pushed-only mode over the same
  NT-handle + keyed-mutex transport as the engine's tap. Raw frames are
  transformed by the pure `ResolveRawCaptureTapDesc` (HDR desktops tone-mapped,
  Advanced-Color SDR desktops sRGB-encoded); the renderer draws the live cursor
  (`cursor_sprite.h`, extracted from the recording compositor and unit-pinned)
  and its own webcam PiP, since Output Duplication composites neither.
- **The lease wiring** — see above. Strictly refcounted: at most one duplication
  ever, none with the preview closed, none while the engine records.

Window and Region previews, and displays on a different adapter than the preview
(explicit subscribe failure, D5), keep their own WGC capture.

## Consequences

- The picker's blanking is fixed with no duplication ever opened, which is the
  cheapest visible win in the design and independent of the risky later phases.
- Held frames are user-visible behaviour and are recorded in
  `docs/product-spec.md`.
- A plain display preview and its recording now share one capture backend: no
  double capture in any state, a VRR- and HDR-true idle preview with no OS
  capture indicator, and a preview that holds through a monitor hot-plug.
- The idle duplication's hardware probe is outstanding: if MPO / fullscreen
  behaviour degrades beside a running game on the previewed monitor, the
  off-ramp is to route the display preview back to WGC — as a WGC *hub*
  consumer, so the hold survives either way. Only the DXGI-hub idle feed is at
  stake; the tap, the hold and the lease ordering stay. See KNOWN_LIMITATIONS.
- Two product questions remain open: whether a kill-switch setting ships for
  the idle duplication, and whether a held (still) frame gets a "reconnecting"
  affordance or holds silently. The hub exposes the state either way.

## The latent bug fixed on the way

`PreviewService` and `WebcamService` posted their frame callback to
`QCoreApplication::instance()`, binding delivery to the application's lifetime
rather than the receiver's. A frame in flight while the Record page died
dereferenced freed memory. Both services now post to a receiver object, so Qt
drops the event when the receiver is gone.
