# Capture-Card Live Wiring — Design

**Date:** 2026-06-27
**Status:** Approved (brainstorming). Next: implementation plan (writing-plans).
**Wave:** 0.8.0 Diagnostics (follows ADR 0033 diagnostics engine; sibling of the CFR pacing slice).
**Branch:** feat/0.8.0-diagnostics

## Context

The Diagnostics page has two pipeline surfaces today:

- **"CAPTURE PIPELINE"** — six `ui::widgets::PipelineFlow` step cards (Source Capture · Frame
  Queue · Compositor · Encoder · Muxer · Disk). Built at `app/pages/DiagnosticsPage.cpp:217-229`,
  refreshed at `:1064-1102`. Cards 0–2 (Capture/Queue/Compositor) are hardcoded
  `Status::Planned` with "…not instrumented yet." notes (`:1071-1073`); cards 3–5 reflect only
  static capability checks (encoder/container/output-path). **These cards never show live numbers.**
- **"LIVE PIPELINE"** — `ui::widgets::LivePipelinePanel`, already live at ~5 Hz with avg/peak for
  compositor & encoder, fps, present cadence, frame/drop counts. **NOT the target of this work**
  (it stays as-is).

Engine-side, `PipelineDiagnosticsAggregator` already measures **compositor submit**, **encode
latency**, **disk write** (each a `RollingTimeWindow`), plus the full `CaptureDiagnostics`
counters/rates (actual/target fps, frame interval, captured/emitted/dropped/duplicated). The
`RecordingDiagnosticsSnapshot` (`libs/recorder_core/include/recorder_core/pipeline_diagnostics.h:216`)
is published via `DiagnosticsCallback` (`:248`) by `SessionStatsCollector::Run()`
(`libs/recorder_core/src/session_stats_collector.cpp:25-27`, ~5 Hz / 198 ms).

## Goal

Make the six "CAPTURE PIPELINE" step cards **live during recording** so an inexperienced user can
see, at a glance, **which steps a recording goes through, whether each step is keeping up, what
resource (CPU/GPU) it uses, and roughly how long it takes**. Clarity over precision.

**Design centre: health-first.** Each card's primary signal is a **status** — *healthy / working
hard / bottleneck* — derived from data the pipeline **already collects** (fps-vs-target, drop
counters, queue depth, existing latency windows, low-disk). A secondary number ("how long / how
much") is shown where we already have it cheaply. This keeps the feature **unobtrusive**: no new
GPU work, no GPU→CPU syncs, no per-frame hot-path cost — only negligible CPU `QueryPerformanceCounter`
brackets and counter reads. Precise per-stage GPU microsecond timing (D3D11 timestamp queries) is
**explicitly deferred** to an optional follow-up (see end) — it adds hot-path complexity and, due to
GPU DVFS, noisy numbers that don't serve this audience.

Non-goals (v1): GPU timestamp queries / precise GPU execution-time; changing the LIVE PIPELINE
panel; GPU utilization/VRAM (NVML/NVAPI); per-frame UI.

## Card Model

Each card = one pipeline step, showing three things: a **status chip** (Healthy / Busy /
Bottleneck), a **CPU/GPU resource tag** (what resource the step uses), and one **secondary number**
(the meaningful "how much/how long" we already have). Status is the headline; the number is detail.

| # | Card | Status driven by (existing signals) | Resource | Secondary number |
|---|------|-------------------------------------|----------|------------------|
| 0 | Source Capture | `actual_fps` vs `target_fps`, capture/coalesce drops | CPU | actual fps (and acquire ms — NEW cheap CPU QPC) |
| 1 | Frame Queue | backpressure drops, queue depth trend | — | queue depth (e.g. "2 / N") |
| 2 | Compositor | existing `compositor_window_` avg + active/inactive | GPU* | compositor submit avg ms (+ VPBlt submit ms — NEW cheap CPU QPC) |
| 3 | Encoder | existing `encode_window_` avg, output fps, encoder drops / NEED_MORE_INPUT | GPU (NVENC) | encode latency avg ms |
| 4 | Muxer | mux throughput / queue not backing up | CPU | mux processing avg ms (NEW cheap CPU QPC) |
| 5 | Disk | existing `write_window_` avg, low-disk guard | CPU | write avg ms |

\* Compositor/Encoder run on the GPU, but their numbers are **CPU-observed** (submit / submit→ready
latency). The card's tag shows the true resource; the tooltip notes the number is "CPU submit
(GPU execution time not measured in this view)". This is honest and matches how the existing LIVE
PIPELINE panel already reports compositor/encoder.

Frame Queue is backed by the **real frame/packet queue** between the encode and mux threads
(`m_state.mux_queue`) plus existing backpressure counters — a health indicator, never a duration,
never a bottleneck candidate. If no meaningful depth is available it shows a neutral "—". The exact
queue handle/accessor is pinned in the plan; the card must not invent a queue that doesn't exist.

## Measurement & Data Flow

Reuse first; add only negligible CPU instrumentation:

- **Reused as-is (no new cost):** `CaptureDiagnostics` fps/interval/drop/dup counters;
  `compositor_window_`, `encode_window_`, `write_window_` rolling averages; low-disk state. These
  already flow in `RecordingDiagnosticsSnapshot`.
- **New, negligible CPU `QueryPerformanceCounter` brackets** (no GPU work, no sync): acquire+copy
  duration (Source Capture), `VideoProcessorBlt` submit duration (folded into Compositor), mux
  processing duration (Muxer). Each feeds a new `RollingTimeWindow` (`acquire_window_`,
  `vpblt_window_`, `mux_window_`) in `PipelineDiagnosticsAggregator`. A QPC bracket around a call is
  a few nanoseconds — safe to run every frame; no sampling machinery needed.
- **New queue-depth gauge:** read the existing `mux_queue` depth (and a peak) into the snapshot.
- **New snapshot fields:** the three durations (avg/peak/latest), queue depth, and per-stage
  `StageHealth` inputs land in `CaptureDiagnostics` / the relevant per-stage structs, each guarded by
  a `MetricAvailability` flag (e.g. acquire shape differs for WGC vs DXGI-OD; compositor inactive
  when no overlay).
- **Transport:** the existing `DiagnosticsCallback` (~5 Hz). **The UI throttles applying to the
  cards to 2 Hz (0.5 s)** via a last-applied timestamp guard in the page's apply path — independent
  of the LIVE PIPELINE panel, which keeps its cadence.

## Health & Bottleneck — pure resolver

A free, unit-testable function classifies each stage and picks the bottleneck; the UI only renders
the verdict:

```cpp
enum class StageHealth { Healthy, Busy, Bottleneck };

// Per-stage inputs distilled from the snapshot (fps ratio, drop deltas, latency vs budget,
// queue trend), plus the frame budget (1000/target_fps). A stage is:
//  - Bottleneck: it cannot keep up — e.g. a duration stage whose avg exceeds the frame budget,
//    OR a stage shedding frames (capture coalesce / encoder NEED_MORE_INPUT / backpressure) over
//    the window, OR disk write breaching budget / low-disk.
//  - Busy: working hard but keeping up (near budget) — informational, no alarm.
//  - Healthy: comfortably within budget, no drops.
// Frame Queue is a depth/health indicator and is never classified Bottleneck.
struct StageSignals { StageId id; double avg_ms; double fps_ratio; uint32_t recent_drops;
                      bool is_duration_stage; bool available; /* ... */ };
struct PipelineHealthVerdict { /* per-stage StageHealth + the primary bottleneck id, if any */ };
PipelineHealthVerdict ResolvePipelineHealth(std::span<const StageSignals> stages,
                                            double frame_budget_ms);
```

Per the "only real bottleneck" decision: when every stage is keeping up, **no card is alarmed**
(cards read Healthy/Busy). Only a stage that genuinely can't keep up turns Bottleneck (warning
color + plain-language hint, e.g. "Encoder can't keep up with 60 fps"). "Busy" is a calm middle
state, not an alarm.

## States & Fallback

- **Idle (not recording):** cards show the static readiness/capability status (as today, minus the
  "not instrumented yet" notes).
- **Recording:** live status chips + resource tags + secondary numbers; bottleneck only when a
  stage can't keep up.
- **Metric unavailable** (e.g. compositor inactive with no overlay; acquire metric n/a for a capture
  mode): the card shows its status from the signals that *are* available and renders the missing
  number as "—". Never crash, never fabricate a number.

## File / Component Map

| File | Change |
|------|--------|
| `libs/recorder_core/include/recorder_core/pipeline_health.h` + `src/pipeline_health.cpp` | NEW — pure `StageHealth`/`StageSignals`/`PipelineHealthVerdict` + `ResolvePipelineHealth` |
| `libs/recorder_core/include/recorder_core/pipeline_diagnostics.h` | New fields: acquire/vpblt/mux avg+peak+latest, queue depth+peak, per-stage availability; (no GPU-timing fields) |
| `libs/recorder_core/src/pipeline_diagnostics_aggregator.{h,cpp}` | New `acquire_window_`, `vpblt_window_`, `mux_window_`; `OnAcquireLatency`/`OnVpbltSubmit`/`OnMuxLatency`; queue-depth gauge |
| `libs/recorder_core/src/video_thread.cpp` | Cheap QPC around Acquire and around `VideoProcessorBlt` submit (no GPU queries) |
| `libs/recorder_core/src/mux_thread.cpp` | Cheap QPC around mux processing; expose `mux_queue` depth |
| `app/pages/DiagnosticsPage.cpp` | Light up PipelineFlow cards 0–5 from the live snapshot via `ResolvePipelineHealth`; 2 Hz throttle; idle↔live |
| `app/ui/widgets/PipelineFlow.{h,cpp}` | Card face: status chip + CPU/GPU tag + secondary number + tooltip (peak / CPU-submit note) |
| tests | `ResolvePipelineHealth`, aggregator windows, PipelineFlow card |

## Testing

- **Pure:** `ResolvePipelineHealth` — all-healthy → no bottleneck; one/multiple stages over budget;
  drop-driven bottleneck (capture coalesce, encoder NEED_MORE_INPUT, backpressure); disk
  budget/low-disk; Frame Queue never Bottleneck; unavailable/partial inputs; empty.
- **Aggregator:** new rolling windows + queue gauge produce correct avg/peak/depth from fed samples.
- **UI:** `PipelineFlow` card reflects snapshot (status chip, resource tag, secondary number,
  bottleneck highlight, idle↔live transition, "—" when unavailable).
- No dev-machine GPU verification needed in v1 (no GPU timestamp queries); a real recording is a nice
  sanity check that the cards populate and statuses are plausible.

## Optional follow-up (deferred, out of scope here)

- **Precise per-stage GPU timing** via a `GpuTimestampSampler` (D3D11 `TIMESTAMP_DISJOINT` +
  `TIMESTAMP` begin/end around the composite pass and `VideoProcessorBlt`, sampled ~8/s,
  `GetData` with `DONOTFLUSH`, deferred 2–3-frame readback so it never stalls). It would add a true
  GPU-ms number to Compositor. Caveat that motivated deferral: the disjoint query reports
  `Disjoint == TRUE` whenever the GPU clock changed across the window — common under gaming DVFS —
  so samples are intermittently discarded; only a sustained disjoint/unsupported condition would
  flip to a CPU-submit label. NVENC encode is **not** measurable this way (separate ASIC, not on the
  D3D11 timeline) — it stays CPU-observed regardless.
- True GPU utilization / VRAM (NVML/NVAPI) — capability-gated, later.
