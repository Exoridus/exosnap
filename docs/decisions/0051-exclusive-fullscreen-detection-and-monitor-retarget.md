# ADR 0051: Exclusive-fullscreen detection and the monitor-retarget fix

## Status

Accepted — implemented (0.10.0). Elevation-free detection, the honest start/mid-session
errors, and the "Record the monitor instead" fix have landed. The parts that require a
real legacy-FSE title — the ProvenBlack blocker firing end to end, the mid-session
standing notification, and the monitor-OD-records-FSE promise — are gated on live
verification before the capture matrix is documented as guaranteed behaviour.

Related: ADR 0013 (DXGI Output Duplication for monitor capture; FSE as a transient
access loss), ADR 0016 / ADR 0033 (anti-cheat posture: no injection/hooking; Info +
opt-out, not auto-disable), ADR 0041 (capture hubs and the held frame).

## Context

A game in **legacy exclusive fullscreen (FSE)** bypasses the desktop compositor.
Window capture (WGC) then has no surface to read: at start it runs into the 5 s
first-frame timeout with an opaque error; a mid-recording switch to FSE freezes the
picture **silently** (the CFR encoder duplicates the last frame while the recording
runs "green"). Monitor capture (DXGI Output Duplication) *can* record FSE — that is
how OBS and others do it — but the FSE transition throws `DXGI_ERROR_ACCESS_LOST`,
which the drain already recovers from, while the **start** path failed immediately.
Detection existed only behind PresentMon (opt-in + elevation), so the default user got
no warning at all.

Perfect FSE *window* capture would need hook/injection capture (the OBS model). That
is rejected pre-1.0: it contradicts the published "no injection" promise, risks
anti-cheat bans for an unsigned newcomer, is a large permanent maintenance surface, and
addresses a case Windows is retiring (FSO/flip-model is displacing legacy FSE).

## Decision

Do not build a new capture path for FSE windows. Instead:

1. **Honest, elevation-free detection** of an exclusive-fullscreen *window* target,
   built as a severity ladder over pure resolvers (`WindowTargetFacts`):
   - **Shape heuristic** (weak): the window covers its monitor and has no
     caption/resize frame. Borderless and FSE are indistinguishable here, so shape
     alone raises **nothing**.
   - **Hub evidence** (strong, measured): a dedicated pre-flight WGC subscription on
     the selected window (`WindowEvidenceProbe`). Two proofs — the same API the
     recording would use produced nothing for ≥ 2 s (`None`), or it froze at the
     moment the window became fullscreen-shaped (`Held` with no fresh frame since the
     transition). The transition correlation is what separates an FSE freeze from a
     legitimately static borderless window (paused fullscreen video).
   - **`SHQueryUserNotificationState`** and **PresentMon** as corroborating fullscreen
     signals (never used alone).
   Only positive evidence speaks: fullscreen signal → **Notice** (Suspected); measured
   black proof → **Blocker** (ProvenBlack). One problem, one card — while this card
   fires it suppresses the generic `rec.present.exclusive` card.

2. **The primary fix is "Record the monitor instead"** (`fix.capture.monitor_instead`):
   an **executable** retarget from the FSE window to its hosting monitor (DXGI OD can
   capture FSE). It is **Auto but never one-click** — it changes the recording scope
   and track structure (the per-application APP audio row drops to System/Microphone),
   so the confirm dialog with a `changes_summary` that names those consequences is
   mandatory. A silent Window→Monitor fallback was rejected: it changes *what* is
   recorded and *which* audio tracks exist without the user's knowledge. The
   borderless recommendation stays in the card's text (the app cannot flip a foreign
   game's display mode).

3. **Honest start/mid-session behaviour** in the engine:
   - The WGC first-frame timeout now names the window state and the FSE possibility.
   - The DXGI-OD start path no longer fails immediately on an `ACCESS_LOST` before the
     first frame: it enters a **bounded 15 s start-hold** and polls `Reopen()` under
     `DecideOdReopen`, reusing the drain's recovery. The 5 s first-frame guard is
     suspended while holding (`FirstFrameWaitStep`) and restarts fresh after a
     successful reopen — otherwise the 15 s budget would be dead code.
   - A window that goes FSE mid-recording is *reported* (a standing notice) instead of
     silently frozen; no auto-stop (the user decides, mirroring the OD hold).

The FixAction **Auto** taxonomy is extended in product-spec §11 to cover confirmed
capture-target changes, rather than silently stretching the "config-only" definition.

## Consequences

- ExoSnap still cannot capture an FSE *window* in isolation — a named limitation
  (versus OBS's hook capture) that the monitor path covers in practice.
- The start-hold changes start semantics: a formerly instant failure now waits up to
  15 s in "Preparing". The budget is deliberately short and logged.
- One extra pre-flight WGC subscription exists while a window target is selected
  (paused during recording; WGC is not exclusive, so no lease is needed). Failure
  modes degrade to "no hub evidence", never to false evidence.
- The pure resolvers (`ClassifyWindowShape`, `CombineFullscreenEvidence`,
  `FirstFrameWaitStep`, `WindowStallSuspected`/`EvaluateWindowStall`, the evidence
  accumulator) are unit-pinned without a GPU; the end-to-end FSE behaviour is
  live-verified.

## Amends

- **ADR 0013** — the OD start path now shares the drain's hold/reopen recovery (bounded
  by a 15 s budget) instead of failing immediately on a start-time access loss.
- **ADR 0016 / ADR 0033** — reaffirm the no-injection posture: FSE is handled by
  detection + an OS-capture retarget, never by hooking. Hook capture remains rejected
  pre-1.0 (a post-1.0 reconsideration would require code-signing established, an
  anti-cheat-vendor relationship, and demonstrated demand).
