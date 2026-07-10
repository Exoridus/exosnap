# Capture hubs — design

Status: approved for planning
Date: 2026-07-10
Supersedes: `2026-07-09-dxgi-od-shared-hub-design.md`
Related: ADR 0013 (OD for monitor capture, format policy), ADR 0040 (WYSIWYG preview via
engine source-tap)

## Why

Three services capture the screen independently: `ThumbnailCapture` for the source
picker's tiles, `DxgiPreviewRenderer` for the Record page's live preview, and
`PreviewService` as the QImage fallback. All three open their own Windows Graphics Capture
session, and all three do so with `CreateForMonitor` — even for a display. The recording
engine, meanwhile, captures a display through DXGI Output Duplication.

Two consequences follow.

A display preview and the recording of that same display come from **different capture
backends**. WGC blanks a capture when another application takes over the surface — the
Snipping Tool is the everyday case — so a preview and a picker tile go empty while the
recording of the same display would not have. And the same target is captured several
times over, once per consumer.

The webcam had this exact shape until a shared capture hub replaced its two competing
readers. This applies that pattern to the rest.

## The measured constraint, stated honestly

An output can be duplicated **once per process**. Measured on an RTX 5070 Ti against
`\\.\DISPLAY6`:

```
two duplications, one device       → first S_OK, second 0x80070057 E_INVALIDARG
two duplications, two devices      → second 0x80070057 E_INVALIDARG
release the first, duplicate again → S_OK
```

Not the documented `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`, and a second D3D device does not
help.

**This constraint does not bite today.** Only the recording engine opens a duplication,
and only one session runs at a time. It begins to bite the moment a second party in this
process wants OD for the same output — which is exactly what moving the preview off WGC
does. The DXGI hub is therefore not a fix for a present bug; it is the **precondition**
for the fix. Sequencing follows from that (see Plan).

WGC carries no such restriction: several captures of one source coexist. So the WGC hub
arbitrates nothing. It earns its place for the other reason a hub exists — owning the only
capture is what makes holding the last frame possible at all.

## Model

One hub per capture technology, keyed by source. A hub owns exactly one capture per key,
counts its consumers, retains the last frame, classifies loss, and reconnects without a
deadline — the shape already proven by `WebcamService` and `ClassifyOdAcquireFailure`.

| Hub | Key | Consumers |
|---|---|---|
| DXGI | monitor (stable GDI device name) | Record preview (display, region) |
| WGC | source (HWND or monitor) | Record preview (window), picker tiles — window *and* display |
| Webcam | device id | Record PiP, Settings preview |

A consumer never opens a capture. It subscribes, receives frames, and on unsubscribe the
hub closes the capture once the last consumer leaves.

Sharing is the mechanism. **Frame control is the point.** A hub owns the only capture for
its key, so it is the only party that knows whether a frame arrived — and therefore the
only party that can hold the last good one instead of handing out an empty surface. No
consumer can do this for itself, because no consumer can tell "the source produced
nothing" apart from "someone else took the source away".

### Loss is held, not blanked

When a source stops producing, the hub keeps serving the last frame it received and
retries underneath, without a deadline. A consumer sees a still image, never an empty one
and never a black one. This is what `WebcamService::TryGetFrame` already guarantees for
the camera. The two cases that drive the design:

- **DXGI, display unplugged.** The duplication dies; `ClassifyOdAcquireFailure` reports
  `Recover`. The hub holds the last frame and reopens by stable GDI device name — the
  same key `DxgiOdCaptureSrc::Reopen` already recovers by, because HMONITOR changes across
  a hot-plug. On replug, production resumes and the consumer never saw a gap. (The engine
  already survives this; the crash it used to cause is fixed. The preview does not.)
- **WGC, the Snipping Tool takes the surface.** WGC blanks the capture. The picker's
  tiles and the window preview go empty today. Behind a hub they hold their last frame
  until WGC resumes producing.

The second case is why the WGC hub is not merely an efficiency play (D7), and why picker
tiles belong behind it (D3) even though WGC lets each tile open its own capture.

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
composited in. That is the WYSIWYG the preview already aims for. Raw hub frames must never
be substituted here: OD frames carry neither cursor nor webcam PiP.

### The engine keeps its own drain

The hub arbitrates who may duplicate an output. It does **not** pump frames for the
recording. The video thread keeps its `AcquireNextFrame` loop verbatim: format negotiation
from the first frame, the phase-correct pacing ring keyed by present-QPC, cursor shape
fetch, diagnostics taps, and `ClassifyOdAcquireFailure` / `DecideOdReopen` recovery.

Re-homing the acquire loop onto a hub thread would put a queue between capture and CFR
scheduling, re-open every recovery edge case, and buy nothing: during recording there is
no second OD consumer, because the preview consumes the composited tap.

The engine works hubless. Given no hub (tests, headless), it opens its own
`DxgiOdCaptureSrc` exactly as today.

## Native HDR10 has no tap

The tap publishes `vpInput` — the composited pre-encode surface. A native HDR10 session
encodes straight from FP16 scRGB and has no such SDR intermediate, so the guard at
`video_thread.cpp:2175` skips the tap and the preview keeps its own WGC capture.

Closing this is independent of the hub, and two prior claims about it are wrong:

- `PreviewSharedTexture::Create` (`preview_shared_texture.cpp:11-70`) applies **no format
  restriction**. It passes `format` straight into the texture desc. FP16 is already
  permitted; there is nothing to "allow".
- A composited FP16 surface only exists when `needsGpuCompositor` holds
  (`video_thread.cpp:1786`) — that is, when a webcam or the cursor is being drawn. With
  neither, a native HDR10 session composites nothing and there is no surface to tap. That
  case must fall back to tapping the raw FP16 capture, or keep WGC. It is not covered by
  "publish the composited surface".

The already-PQ `R10G10B10A2` sub-path (`hdrPqInputIsPq`, overlays unsupported,
`video_thread.cpp:1785`) composites nothing by construction and keeps the WGC fallback.

Consuming FP16 needs a preview-side shader: sRGB-encode for `SdrScrgb` desktops,
scRGB→SDR tone-map (port of `ScrgbToSdr709Channel` / `HdrPeakScale` from `hdr_tonemap.h`)
for HDR desktops.

## Decisions

- **D1 — The hub arbitrates; it does not pump for the recording.** Rationale above. The
  video thread takes a lease around its OD lifetime.
- **D2 — Transport is the existing NT-handle + keyed-mutex shared texture.** Proven by
  ADR 0040, non-blocking (0 ms acquire, drop on contention), each thread keeps a private
  device and context. Reuses `PreviewSharedTexture` and `PreviewPublishGate` unchanged.
- **D3 — Picker display tiles go behind the WGC hub, not the DXGI hub.** Routing them to
  DXGI would hold a duplication open for every visible display tile at once — the sharpest
  form of the idle-duplication risk below, paid for a grid of thumbnails. Leaving them on
  bare WGC keeps today's blanking. Behind the WGC hub they get the hold without opening a
  single duplication, which is the whole point. The DXGI hub stays reserved for the live
  preview: one display at a time, and the only consumer that needs to match the recording
  backend.
- **D4 — A per-key registry, not one retargetable hub.** The WGC hub genuinely serves
  several windows at once (picker tiles). The DXGI hub will in practice hold at most one
  key, but a registry costs nothing over a special case and removes the "which target is
  it pointed at" state.
- **D5 — Cross-GPU: explicit subscribe failure, then WGC fallback.** `OpenSharedResource1`
  fails across adapters. The subscribe returns an error, one structured log line is
  emitted, and that consumer runs WGC. No second duplication is ever opened for the same
  output. A cross-adapter CPU transport is not worth building.
- **D6 — Duplication is recreated at each lease transition, never shared across devices.**
  A serialized close→open beats device sharing. Reopen costs milliseconds, and the
  preview holds its last frame across it.
- **D7 — The WGC hub is in scope, and ships before the DXGI hub.** WGC allows several
  captures of one source, so the hub buys no *arbitration* there. It buys the hold: today
  a Snipping Tool session empties every picker tile and the window preview. The WGC hub
  opens no duplication, so it carries none of the idle-duplication risk below, and it
  fixes user-visible blanking on its own. It is the cheapest visible win in this design,
  not an afterthought.

## The idle-duplication risk

An always-open idle duplication has desktop-wide side effects. It can force DWM out of
multiplane-overlay and fullscreen-optimisation paths, and other processes can hit
`DXGI_ERROR_NOT_CURRENTLY_AVAILABLE` when duplication slots run out. For an app that sits
open beside a game, that is not a footnote.

Today's idle preview pays a different cost (WGC's DWM sync coupling, VRR interference, the
capture indicator — the very costs ADR 0013 removed from the recording path). The trade is
not strictly better on every system.

Mitigations, all required:

- Strict refcount. The Record page hidden, or the preview stopped, closes the duplication.
  An OD must never idle for an invisible page.
- A suspend rule for a hub held open with no visible consumer.
- A kill-switch setting, if the probe shows regressions.

**This risk is why the plan has an off-ramp**, and why the DXGI hub is sequenced last
among the hubs. Only Phase 4 opens an idle duplication. If its probe shows degraded DWM
behaviour, the correct outcome is to stop there: the preview stays on WGC — but by then it
is a WGC *hub* consumer, so it already holds its last frame instead of blanking. The
blanking is fixed either way. What Phase 4 alone adds is preview and recording sharing one
backend, and a VRR- and HDR-true idle preview.

## Plan

Each phase is independently green and independently valuable. The hold — the thing the
hubs exist for — arrives in Phase 3, before any duplication is opened.

1. **Native HDR10 tap.** Drop the `hdrNativeActive` guard where a composited FP16 surface
   exists; tap the raw FP16 capture where it does not; add the preview FP16 sampling and
   tone-map shader. Removes the double capture during HDR10 recording. **No hub.**
2. **Hub skeleton, pure arbitration.** Consumer registry, `ResolveHubState`, hold state,
   lease grant/return, refcounted open/close. Shared by both hubs, compiled and
   unit-pinned, wired to nothing. **No behaviour change.**
3. **WGC hub.** Window preview and picker tiles — window and display — become consumers.
   Picker tiles and the window preview hold their last frame through a Snipping Tool
   session instead of going empty. **No duplication is opened; none of the idle risk.**
4. **DXGI hub, and the preview moves onto it** — probe the idle-duplication risk first.
   Display preview and recording share a backend; the preview holds through an unplug.
   This is the phase that can be abandoned.
5. **Lease wiring.** The coordinator sequences preview release → recording start, and the
   reverse on stop. Only reachable if Phase 4 landed.
6. **Docs.** New ADR for hub + lease; amend ADR 0040 (FP16 tap; the hub does not replace
   the tap); update KNOWN_LIMITATIONS and `docs/product-spec.md` (held frames are
   user-visible behaviour).

## Testing

Pure policy, unit-pinned, no GPU:

- consumer counting: the capture opens on the first subscriber and closes on the last
- a source that stops producing yields the held frame, never an empty one
- a source that never produced a first frame reports "no frame yet" — a hold has nothing
  to hold, and the consumer must be able to tell that from a held frame
- the held frame survives an unbounded number of failed reopens, and is replaced only by a
  newer good frame — never cleared on loss
- the handover order: a hub holding display X must have released before the engine opens
- reclaim after stop restores production
- a lease requested while the hub is reopening: the hub drops the dead duplication and
  grants
- loss classification and unbounded retry (mirrors the existing OD and webcam classifiers)
- a consumer unsubscribing while a frame is in flight is never called afterwards

This is only a real pin if the hub's lease and pump methods are thin drivers of the
resolver, the way the drain actually calls `ClassifyOdAcquireFailure`. That binding is a
review requirement, not a suggestion.

The one-duplication-per-process constraint is asserted by a probe, not assumed. No
existing test opens a real duplication; that stays true.

Requires real hardware, and a human:

- **the hold, in both hubs**: run the Snipping Tool over a source picker and watch the
  tiles freeze rather than empty (WGC); unplug and replug a monitor mid-preview and watch
  the preview freeze and resume (DXGI)
- whether the handover reads as a freeze or a glitch, watched on a real display
- the idle-duplication probe: MPO and fullscreen behaviour with a game running
- cross-GPU subscribe failure → WGC fallback (needs a multi-adapter machine)
- HDR desktop: idle hub preview colour against WGC's

## A latent bug to fix on the way

`WebcamService::PostFrame` (`WebcamService.cpp:574-581`) copies `frame_callback_` and posts
it to `QCoreApplication::instance()`, so Qt binds delivery to the application's lifetime,
not the page's. The callback set in `RecordPage.cpp:2361` captures a raw `this` alongside a
`QPointer safeSurface`. A frame in flight while the page dies dereferences freed memory.
The `PreviewService` callback above it (`RecordPage.cpp:2352`) has the same shape.

Any hub that fans out to consumer callbacks across a thread boundary inherits this unless
subscription lifetime is explicit. Consumer handles must be revocable under the hub's lock.

## Open questions

- Does a kill-switch setting ship for the idle duplication, or is the off-ramp a code
  revert?
- A held frame is a still image the user may mistake for a live one. Does the preview
  annotate it (a "reconnecting" affordance), or hold silently? The hub exposes the state
  either way; whether the UI draws it is a product decision.

## Not in scope

- Cross-adapter frame transport.
- Hub-side format conversion. The hub hands out the raw desktop format; session policy
  (`ResolveOdCaptureMode`, tone-map, 4:4:4) stays with each consumer.
- Whether webcam device selection moves into the source picker as a fourth tab. A product
  decision, tracked separately. The hub does not depend on it.
