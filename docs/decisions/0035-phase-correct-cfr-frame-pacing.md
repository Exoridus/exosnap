# ADR 0035: Phase-Correct CFR Frame Pacing (VRR / high-refresh → smooth fixed-rate)

## Status

Proposed. The engine-side counterpart to the 0.8.0 VRR/CFR judder *diagnosis* (ADR 0033): the
diagnosis tells the user *why* a recording judders; this fixes it. Builds directly on the
present-cadence instrumentation already landed in 0.8.0 (`DXGI_OUTDUPL_FRAME_INFO.LastPresentTime`
tapped per frame in `libs/recorder_core/src/video_thread.cpp`). Candidate placement: a `0.8.x`
follow or `0.10.0` (reliability hardening) — it is a capture-engine reliability feature, not a
vendor feature. Relates to [[0033-diagnostics-engine-and-fixaction.md]].

## Context

A high-refresh and/or VRR (G-Sync/FreeSync) source presents at a **variable, non-60-aligned**
cadence (e.g. uncapped 80–144 fps). The recorder targets a **fixed** output rate (60 fps CFR).
Today's CFR scheduler (`video_thread.cpp:1418`, "QPC-driven scheduler — duplicate/drop to hit
constant rate") emits, at each evenly-spaced 60 Hz tick, **the newest captured frame** (duplicating
when none arrived, dropping older ones when several did).

The result: the output *timing* is even, but the **motion sampling is uneven** — each slot holds
"whatever was newest at the tick", not the frame whose actual present time best matches the slot.
Against a variable source cadence the phase drifts continuously → **judder/microstutter in an
otherwise-constant 60 fps file**, even though the live gameplay looked perfectly smooth. This is the
single biggest quality gap for the core "record my 144 Hz G-Sync gameplay to a flawless 60 fps AV1"
use case.

**Hard ceiling (stated honestly):** 60 fps output has 60 fps of *motion resolution*. This ADR
removes **judder** (uneven pacing); it does **not** make 60 fps look as fluid as 144 Hz — that
requires a 120/144 fps output. The goal is "as smooth as a native 60 fps recording", not "looks like
144 Hz".

## Decision

Replace newest-at-tick with **phase-correct frame selection** — choosing, per output slot, the
captured frame whose source present time is nearest the slot's ideal time:

- **GPU-only, selection not interpolation.** Keep a small ring of recently-captured GPU textures
  (already in VRAM) plus each one's `LastPresentTime`. At each 60 Hz tick, **select** the ring entry
  whose present time is closest to the slot time and hand it to the encoder. No GPU→CPU readback, no
  blending/motion-compensation. Cost ≈ a comparison + a few extra textures in VRAM → near-zero
  runtime overhead; **maximum GPU headroom stays with the game** (RAM/VRAM use is explicitly an
  acceptable trade per product direction). Blending was rejected: it costs compositor time and
  alters the look; selection is enough to remove judder.

- **No elevation required.** `LastPresentTime` is a field of `DXGI_OUTDUPL_FRAME_INFO` from the
  normal desktop-duplication capture path — unprivileged. The pacer works at standard user level for
  **monitor capture**. Window/region (WGC) capture has no `LastPresentTime`; there it **falls back to
  newest-at-tick** (or, later, PresentMon-ETW enrichment, which is the only elevation-gated piece).

- **User control — frame-pacing mode (settings select).** `Newest (lowest latency)` vs
  `Smooth (phase-correct)`. Default **Smooth** (recording is the use case; latency is irrelevant).
  `Newest` is kept for lowest-latency/streaming. The 0.8.0 diagnostics **recommend** switching to
  Smooth (a `FixAction`, ADR 0033) when they measure VRR/CFR judder. Persisted in settings
  (pre-1.0: schema bump, reset).

- **Latency trade is bounded and acceptable.** Phase-correct selection may pick a frame up to ~1
  source-frame older than the absolute newest to hit the correct phase — irrelevant for recordings,
  hence the Smooth default; the Newest mode exists for the rare latency-sensitive case.

## Consequences

- A bounded ring of GPU textures (a handful of frames) is retained — extra VRAM, no extra
  CPU/readback. Sizing must cover the worst-case source-over-output ratio (e.g. 144→60) plus a small
  margin.
- The tick scheduler gains a selection step (present-time-nearest) instead of always taking newest;
  duplicate/drop accounting must stay correct (a duplicated slot = no new ring entry near the slot
  time; a dropped frame = a ring entry never selected). The existing drop/dup diagnostics must keep
  reporting truthfully.
- VFR output is unaffected (it already preserves source timing). The pacer is a CFR-path feature.
- Closes the loop with ADR 0033: the same `LastPresentTime` signal both **diagnoses** (jitter/coalesce
  → "VRR judder") and **drives the fix** (phase-correct selection). Together they are an uncommon,
  defensible differentiator: consumer recorders (incl. OBS) neither surface present-cadence
  diagnostics nor expose a present-time-aware CFR pacer.
- Honest scope: the fps-fluidity ceiling remains; for true high-refresh fluidity the user records at
  120/144 fps.
