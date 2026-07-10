# DXGI Output-Duplication Shared Hub — Design

**Date:** 2026-07-09
**Status:** SUPERSEDED by `2026-07-10-capture-hubs-design.md` (2026-07-10). Do not plan
against this document.
**Related:** ADR 0013 (OD for monitor capture, format policy), ADR 0040 (WYSIWYG preview
via engine source-tap), `OdCaptureMode::SdrScrgb` (SDR Advanced-Color desktops).

> **Why it was superseded.** Three of its positions did not survive:
>
> - It treats a WGC hub as a Non-Goal, on the grounds that WGC permits several captures of
>   one source so sharing buys only efficiency. That misses the point of a hub: frame
>   control. WGC blanks under the Snipping Tool, and only the owner of the sole capture can
>   hold the last good frame. The WGC hub now ships *first*, and picker tiles sit behind it.
> - It rejects a per-key registry ("the product previews one target at a time"). Picker
>   tiles are several sources at once.
> - Two of its factual claims are wrong. `PreviewSharedTexture::Create` applies no format
>   restriction, so its migration step 1 ("allow FP16") is not a task; and the composited
>   FP16 surface it proposes to publish exists only when `needsGpuCompositor` holds, so a
>   native HDR10 session with no webcam and no cursor has nothing to tap.
>
> What survives, and is carried over verbatim: the driver-lease model (D1), the
> NT-handle/keyed-mutex transport (D2), cross-GPU honesty (D5), recreate-on-transition
> (D7), and — above all — the idle-duplication risk and the off-ramp it argues for. That
> analysis is the most valuable thing in this file and is the reason the DXGI hub is
> sequenced last. Read §"Risks / open questions" below; the successor summarises it but
> does not reproduce its detail.

## Problem

Monitor pixels are captured through two unrelated stacks today:

- **Recording** owns the only `IDXGIOutputDuplication` per session
  (`DxgiOdCaptureSrc::Open`, `libs/recorder_core/src/dxgi_od_capture_src.cpp:286`
  DuplicateOutput1 / `:296` legacy fallback), pumped by the video thread
  (`video_thread.cpp:262 Run()`), on a device adapter-matched to the HMONITOR
  (`video_thread.cpp:296-326`). Mature recovery: `ClassifyOdAcquireFailure` /
  `DecideOdReopen` + stable-GDI-name `Reopen` (`dxgi_od_capture_src.h:210-264`).
- **Live preview** always uses WGC — even for monitors — on its own default-adapter
  D3D11 device (`app/services/DxgiPreviewRenderer.cpp:437-452`). WGC monitor capture is
  exactly what ADR 0013 removed from the recording path (DWM sync events, VRR
  interference, capture indicator) — the idle Record page still pays that cost.

ADR 0040 bridges the two *during recording*: the engine shares its composited pre-encode
frame (`vpInput`) as an NT-handle + keyed-mutex texture (`preview_shared_texture.h`,
throttled by `PreviewPublishGate`), and the preview stops its own WGC capture. Two holes
remain where a second capture still runs in parallel with the recording:

- **(a) Native HDR10** — the tap skips `hdrNativeActive` sessions
  (`video_thread.cpp:2175`); the preview keeps its own WGC capture.
- **(b) Cross-GPU** — `OpenSharedResource1` fails across adapters; the preview stays on
  its own WGC capture (ADR 0040 consequences).

There is also a structural gap: nothing arbitrates "who may duplicate this output".
Today that works only because the preview never uses OD. The moment the preview wants OD
too (VRR-safe, HDR-true idle preview), an unowned handoff race appears at recording
start/stop.

## Goal

At most **one `IDXGIOutputDuplication` per monitor**, owned by an explicit arbiter, with
fan-out to N consumers (idle live preview; recording), modelled on the webcam shared-
capture hub (`RecordingCoordinator` owns one `WebcamService`; boolean consumer flags;
`SyncWebcamService()` computes `want_running`; one frame callback fans out —
`RecordingCoordinator.cpp:581-595`). Plus: close hole (a) honestly, and make hole (b) an
explicit, logged degradation instead of a silent second capture.

## Non-Goals

- **No unification of Window capture.** WGC is the only API for window/app capture
  (ADR 0013); it stays. The hub is monitor-only. No fake `ICaptureSource` abstraction
  spanning both.
- **No cross-adapter frame transport.** A 4K@30Hz PCIe readback/upload path is not
  worth building for the multi-GPU preview case; degradation stays (see Decisions).
- **No hub-side format conversion.** The hub hands out the raw desktop format
  (BGRA8 / R10G10B10A2 / FP16 scRGB); session policy (tone-map, PQ, 4:4:4) stays with
  each consumer.
- **No per-monitor hub registry.** The product previews/records one target at a time;
  one retargetable hub instance is enough (no hidden MVP growth).
- **Not moving the recording's acquire loop off the video thread.** See Decision D1 —
  this is the single most important scoping decision in this spec.

## Architecture

### Overview

`recorder_core::MonitorDuplicationHub` — engine-side, UI-agnostic (pure D3D11/Win32 +
callbacks, no Qt). It has two roles:

1. **Arbitration (always):** it is the only party allowed to create an
   `IDXGIOutputDuplication` for the current target monitor. Consumers register/
   deregister; a pure resolver (`ResolveHubState`, the `SyncWebcamService` analogue)
   computes from the consumer set: `want_open`, `want_idle_pump`, `driver_leased`.
   Refcount 0 → duplication closed.
2. **Idle fan-out (no recording):** a hub-owned pump thread drains
   `AcquireNextFrame` on a hub-owned adapter-matched device and publishes raw frames
   into a keyed-mutex NT-handle shared texture per tap consumer — the *exact*
   transport ADR 0040 already proved (`PreviewSharedTexture` + `PreviewPublishGate`,
   reused as-is).

During recording the video thread takes a **driver lease** from the hub: the idle pump
stops, its duplication closes, and the video thread opens the duplication on *its own*
session device and runs its existing drain verbatim (CFR pacing ring, cursor shape
fetch, diagnostics taps, `ClassifyOdAcquireFailure`/`DecideOdReopen` recovery —
`video_thread.cpp:2302-2412`). On stop, the lease is returned and the hub resumes the
idle pump if tap consumers remain. The duplication object is recreated at each lease
transition; it is never shared across devices and never open twice — the hub serializes
close→open.

Crucially, the **recording-time preview stays on the ADR 0040 pushed-source tap**, not
on raw hub frames: OD frames contain neither the cursor (OD does not composite it,
ADR 0013) nor the webcam PiP. Raw hub frames during recording would *break* WYSIWYG.
The hub therefore replaces the preview's **idle** WGC monitor capture; ADR 0040 keeps
covering the **recording** window, extended to FP16 to close hole (a) (Decision D3).

### Ownership & lifetime

- `MonitorDuplicationHub` lives in `libs/recorder_core` (engine stays UI-agnostic; it
  never includes app headers, all outward flow is callbacks, mirroring
  `preview_shared_handle_cb`).
- The single instance is **owned by `RecordingCoordinator`** (app), exactly like
  `webcam_service_`. The coordinator retargets it when the selected preview/recording
  monitor changes.
- `RecorderSession` receives an optional `MonitorDuplicationHub*` (setter, like
  `SetPreviewSharedHandleReadyCallback`). With a hub whose target matches the session's
  monitor, the video thread brackets its OD open/close in
  `AcquireDriverLease()` / RAII release. Without a hub (tests, headless), the video
  thread opens its own `DxgiOdCaptureSrc` exactly as today — the engine remains
  standalone.
- Open/close is consumer-refcounted: preview tap registered → open (idle pump);
  driver leased → open (on the session device); no consumers → closed. Record page
  hidden / preview stopped must deregister, so an OD never idles for an invisible page.

### Device model

Evaluated options:

- **(a) Preview adopts the hub device** — rejected. The preview swap chain, shaders and
  WGC window path live on its own device; adopting the hub device would either split
  the renderer across two devices or force the whole renderer onto a per-monitor device
  (swap-chain rebuild on every target change, cross-adapter present for windows).
- **(c) One shared device for everything** — rejected. The recording device *must* be
  adapter-matched to the monitor (multi-GPU, `video_thread.cpp:317-321`); a single
  process-wide device cannot satisfy that, and sharing one immediate context between
  the video thread, a hub pump and a preview render thread is a lock-contention
  footgun (immediate contexts are not thread-safe).
- **(b) NT-handle + keyed-mutex shared textures per consumer — chosen.** Proven by
  ADR 0040, zero new transport code (`PreviewSharedTexture`), never stalls the producer
  (0 ms acquire, drop on contention), and each thread keeps a private device/context.
  Same-adapter only — which is exactly the honest cross-GPU line we want.

Concretely: the hub owns one adapter-matched device for the idle pump (created via
`FindAdapterForMonitor`); the recording keeps its own session device (unchanged); the
preview keeps its default-adapter device and opens the hub's shared texture — if that
open fails (different adapter), the subscription fails *explicitly* and the preview
falls back to its WGC monitor path.

### Thread model & copy count

`AcquireNextFrame` is only ever pumped by one thread, enforced by the lease state
machine:

- **Idle:** hub pump thread. Loop: `TryAcquireFrame(timeout≈16ms)` → (gate) →
  `PreviewSharedTexture::TryPublish` (CopyResource while the OD frame is held) →
  `ReleaseFrame()`. One GPU copy hub-side; the preview render thread does its usual
  keyed-mutex copy into a private texture (`ConsumePushedFrame` mechanics). Two copies
  total at ≤33 Hz — identical cost to today's pushed-source path, cheaper than idle WGC
  (OD only delivers on desktop change).
- **Recording:** video thread is the driver; its loop is untouched (raw → pacing
  ring / `odCapturedTex` copy as today). It does **not** publish raw frames to hub taps
  — during recording the preview consumes the ADR 0040 composited tap instead, so hub
  taps have zero subscribers and the hot path gains nothing new.
- OD's borrowed-until-`ReleaseFrame` texture is never exposed outside the pumping
  thread; consumers only ever see the shared copy. No copy explosion: worst case is
  unchanged from today (1 producer copy + 1 consumer copy on the preview path; 1 ring
  copy on the encode path).

### Cadence

- Recording cadence is untouched (CFR scheduler, `FramePacingMode`, phase-correct
  ring). The hub never sits between the duplication and the encoder.
- Idle pump publishes through `PreviewPublishGate(kPreviewMinIntervalNs)` (~30 Hz,
  truncation-aware threshold) — same gate, same tests.
- A consumer that stops collecting simply misses frames: `TryPublish` drops on keyed-
  mutex contention; nothing queues, nothing blocks. If *all* consumers deregister, the
  resolver closes the duplication (refcount) — the "nobody drains" state cannot
  accumulate anything because OD frames are released immediately after publish.

### Formats & HDR10

- The hub delivers the **raw negotiated desktop format** (BGRA8 / R10G10B10A2 / FP16
  scRGB, per ADR 0013 negotiation rules: trust acquired frames, not ModeDesc).
  Conversion is per consumer:
  - Recording: existing `ResolveOdCaptureMode` machinery (Sdr / SdrScrgb / HdrToneMap /
    HdrNative) — unchanged.
  - Preview: BGRA8 and R10G10B10A2 sample directly into the 8-bit swap chain (already
    true for the pushed path); FP16 gains a small pixel-shader mapping — sRGB-encode
    for `SdrScrgb` desktops, scRGB→SDR tone-map (the `ScrgbToSdr709Channel` /
    `HdrPeakScale` reference math from `hdr_tonemap.h`, ported to HLSL) for HDR
    desktops. Idle HDR preview becomes colour-true instead of WGC's washed-out default.
- **Hole (a) is NOT closed by the hub** — it is closed by extending the ADR 0040 tap:
  native HDR10 sessions (FP16 scRGB input) already composite cursor + PiP into a linear
  FP16 surface before PQ conversion (`video_thread.cpp:1787-1795`). Publish *that*
  surface through `PreviewSharedTexture` (drop the `hdrNativeActive` guard at `:2175`,
  allow FP16 in the shared texture) and let the preview's new FP16 shader display it.
  WYSIWYG (cursor + PiP) preserved; second WGC capture gone. The rare already-PQ
  R10G10B10A2 desktop sub-path (`hdrPqInputIsPq`, overlays unsupported,
  `video_thread.cpp:1767-1775`) taps the raw PQ frame with a PQ→SDR shader variant, or
  — acceptable v1 — keeps the WGC fallback, documented.

### Cross-GPU

Honest degradation, decided at subscribe time instead of silently mid-flight:

- Preview tap subscribe opens the shared handle on the preview device; on failure
  (different adapter) the subscribe returns an explicit error, one structured log line
  is emitted, and the preview runs its WGC monitor path as today. No second OD is ever
  opened for the same monitor.
- During recording the ADR 0040 handle open can still fail cross-GPU → preview stays on
  its own capture (unchanged, documented in KNOWN_LIMITATIONS). The hub does not
  pretend to fix this; fixing it would require a CPU round-trip transport (Non-Goal).

### Recovery

- **Idle pump** reuses the *same pure policies*: `ClassifyOdAcquireFailure` on every
  failed acquire; on `Recover` → `Reopen()` polled under `DecideOdReopen` (unbounded
  budget, 250 ms cadence — same constants as the drain). While reopening, consumers
  simply receive no new publishes: the preview holds its last frame (exactly the
  frozen-not-black policy ADR 0013 pins for recording). A lightweight status callback
  (`Live / Holding / Lost`) lets the app annotate the preview if desired; v1 may ignore
  it.
- **DEVICE_REMOVED / classify=Fail** in the idle pump: hub closes, notifies `Lost`;
  the coordinator's sync loop falls the preview back to WGC (or shows the last frame +
  source-lost state). No retry loop against a dead GPU.
- **During recording** recovery is byte-for-byte today's video-thread logic
  (`odHolding` + `Reopen` + held-frame CFR duplication, `video_thread.cpp:2302-2314`).
  The lease guarantees the hub does not race a reopen: the hub owns no duplication
  while the lease is out.
- **Lease transitions during a reopen window** are serialized by the hub state machine:
  a lease request while the idle pump is `Holding` simply closes the (already dead)
  duplication and grants the lease; the video thread's own open/recovery takes over.

### Region / Window

- **Window targets:** WGC end-to-end, unchanged (recording `video_thread.cpp:280-281`;
  preview WGC item `DxgiPreviewRenderer.cpp:561-594`). The hub is not involved.
- **Region targets:** Region is `Kind::Monitor` + `crop_region`; the crop is applied
  per consumer, as today — recording via the VideoProcessor source rect
  (`video_thread.cpp:533-579,907-909`) / PQ-converter geometry, preview via its
  existing `cropBox_` shader path. The hub always duplicates the full monitor; no crop
  state leaks into it.
- The renderer's source selection becomes an explicit three-way, replacing the implicit
  "WGC unless pushed": `OwnWgc` (windows; monitor fallback) · `HubTap` (idle monitor) ·
  `EngineTap` (recording, ADR 0040). `PushedSourceState` generalizes into this enum's
  state machine (same file/test pattern, `app/tests/test_pushed_source_state.cpp`).

## Decisions (with rationale)

- **D1 — The hub arbitrates; it does not pump for the recording.** The video thread
  keeps its acquire loop and takes a driver lease. Rationale: the OD drain is the most
  battle-hardened part of the engine (format negotiation from first frame, phase-
  correct pacing ring keyed by present-QPC, cursor shape, diagnostics, hold-and-reopen
  recovery). Re-homing `AcquireNextFrame` onto a hub thread would put a queue between
  capture and CFR scheduling, re-open every recovery edge case, and buy nothing —
  during recording there is no second OD consumer (the preview uses the composited
  tap).
- **D2 — Transport = NT-handle + keyed-mutex shared texture (option b).** Proven
  (ADR 0040), non-blocking, per-thread devices, reuses `PreviewSharedTexture` +
  `PreviewPublishGate` verbatim. Options (a)/(c) rejected for swap-chain/multi-GPU/
  context-contention reasons detailed above.
- **D3 — Hole (a) is fixed in the ADR 0040 tap, not in the hub.** The composited FP16
  surface exists; raw hub frames would lose cursor + PiP. This also makes hole (a) the
  first migration step, independent of the hub.
- **D4 — Raw formats out, per-consumer conversion.** Keeps session policy
  (`ResolveOdCaptureMode`) out of shared infrastructure; the hub stays dumb.
- **D5 — Cross-GPU: explicit subscribe failure + WGC fallback.** No silent second OD,
  no cross-adapter transport.
- **D6 — One retargetable hub owned by `RecordingCoordinator`; engine gets an optional
  pointer.** Mirrors the webcam hub ownership; engine remains UI-agnostic and works
  hubless.
- **D7 — Duplication is recreated at lease transitions, never shared across devices.**
  Serialized close→open beats device-sharing complexity; reopen is milliseconds and the
  preview already holds its last frame across the countdown.

## What is removed / what stays

Removed / demoted:

- The preview's **WGC-by-default monitor capture** (idle) — demoted to explicit
  fallback (cross-GPU, hub open failure, PQ-desktop sub-path). This deletes the idle
  Record page's DWM/VRR coupling on the happy path.
- The `hdrNativeActive` tap exclusion (`video_thread.cpp:2175`) and the "Native HDR10
  preview stays approximate" consequence in ADR 0040 / KNOWN_LIMITATIONS.
- The unowned assumption "only recording ever duplicates a monitor".

Explicitly **kept** (the initial idea — "the hub makes the pushed source obsolete" — is
wrong and rejected):

- `PreviewSharedTexture`, `PreviewPublishGate`, `BeginPushedSource`/`EndPushedSource`,
  the ADR 0040 switch-over. Raw OD frames lack cursor + PiP; the composited tap remains
  the only WYSIWYG recording preview. `PushedSourceState` is generalized (renamed
  `PreviewSourceState`, three sources), not deleted.
- `DxgiOdCaptureSrc` (the hub composes it), `ClassifyOdAcquireFailure`,
  `DecideOdReopen`, and the whole video-thread drain.
- `PreviewService` (QImage fallback surface) — untouched.

## Migration steps (each independently green)

1. **FP16 engine tap (closes hole a; no hub).** Allow `R16G16B16A16_FLOAT` in
   `PreviewSharedTexture::Create`; publish the composited FP16 surface when
   `hdrNativeActive && !hdrPqInputIsPq`; add the preview FP16 sampling + scRGB→SDR
   tone-map shader (port of `ScrgbToSdr709Channel`, session peak passed with the
   handle). Standalone value: no double capture during native HDR10 recording, and the
   FP16 shader is prerequisite work for step 3's HDR idle preview.
2. **Hub skeleton + pure arbitration.** `MonitorDuplicationHub` with consumer
   registry, `ResolveHubState` (pure), lease grant/return, refcounted open/close —
   compiled and unit-tested, not yet wired to any UI. No behavior change.
3. **Idle monitor preview via hub tap.** `RecordingCoordinator` registers the preview
   as tap consumer for monitor targets; `DxgiPreviewRenderer` gains the `HubTap` source
   (mechanically identical to pushed-source consumption); explicit WGC fallback on
   subscribe failure. WGC remains the code path for windows. User-visible value:
   VRR-safe, HDR-true idle preview.
4. **Driver lease wiring.** `RecorderSession::SetMonitorHub`; video thread brackets its
   OD lifetime in the lease; coordinator sequences preview-tap release → recording
   start (and the reverse on stop) through the resolver. Removes the last double-OD
   window.
5. **Docs.** New ADR (hub + lease), amend ADR 0040 (FP16 tap; "hub does not replace the
   tap"), update KNOWN_LIMITATIONS (cross-GPU unchanged; PQ-desktop preview sub-path).

## Tests

GPU-free (unit-pinned, each named with the production path it exercises):

- `ResolveHubState`: consumer-set → want_open/want_pump/lease transitions, refcount-0
  close, lease-while-holding, deregister-during-lease. Exercises the real arbitration
  code *only if* the hub's pump/lease methods are thin drivers of this resolver — that
  binding is a review requirement, mirroring how `ClassifyOdAcquireFailure` is actually
  called from the drain.
- Idle-pump step decision (pure `DecidePumpStep(acquire_result, gate, classify)`
  factored like the drain's failure handling): timeout→idle, frame→publish,
  ACCESS_LOST→reopen schedule via existing `DecideOdReopen`, DEVICE_REMOVED→Lost.
  Exercises the same classify/reopen policies the pump calls in production.
- `PreviewPublishGate` (exists, `test_preview_publish_gate.cpp`) — now also the idle
  pump's gate; no new test needed, but the pump must call the same class.
- Generalized `PreviewSourceState` (extends `test_pushed_source_state.cpp`): WGC↔hub↔
  engine-tap transitions, fallback-never-stopped-WGC invariant, no-double-overlay
  invariant. Exercises the renderer's real per-iteration decisions (as today).
- Format-policy addition: FP16 accepted by the tap; `ResolveOdCaptureMode` untouched
  (existing tests remain the pin).

Real-GPU / manual probes (no current test opens a real OD; that stays true):

- One OD per monitor actually enforced across preview→record→preview (log-based).
- Cross-GPU subscribe failure → WGC fallback (needs a multi-adapter box).
- HDR desktop: idle hub preview colour vs WGC; FP16 tap during HDR10 recording.
- ACCESS_LOST churn while idle (fullscreen game entry, HDR toggle, lock screen).

## Risks / open questions

1. **An always-open idle duplication has desktop-wide side effects.** An active
   duplication can force DWM out of multiplane-overlay/fullscreen-optimization paths
   and (per docs) other processes can hit `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE` if
   duplication slots are exhausted. Today the idle preview's WGC has its own (different)
   costs, but the trade is not strictly better on all systems. Mitigations: strict
   refcount (page hidden → closed), and a kill-switch setting if probes show MPO
   regressions. This is the strongest argument for stopping after step 1+3 evaluation.
2. **Idle ACCESS_LOST churn.** Fullscreen transitions/HDR toggles kill duplications;
   a user gaming with the app open in the background puts the idle pump into a
   reopen-poll loop indefinitely (cheap, but a new steady-state behavior). Needs the
   throttled retry + probably a "suspend after N minutes hidden" rule.
3. **Three preview source modes in one renderer** (WGC / hub / engine tap) is real
   state-machine complexity; without the pure `PreviewSourceState` generalization and
   its tests this becomes the bug farm of the project.
4. **DRM/protected content** duplicates as black where WGC may render a placeholder —
   an idle-preview behavior change to document.
5. **Marginal-value honesty:** the recording pipeline gains nothing from the hub
   (Decision D1 keeps it out of the hot path by design). The hub's payoff is idle-
   preview quality (VRR/HDR) + arbitration hygiene. If the probes in step 3 show the
   idle-OD side effects (risk 1) outweigh the WGC issues, the correct outcome is to
   ship step 1 (FP16 tap — unconditional win), keep WGC idle preview, and drop the
   hub. Step ordering makes that off-ramp free: nothing in step 1 depends on steps 2-4.
