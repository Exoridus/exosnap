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
- **"LIVE PIPELINE"** — `ui::widgets::LivePipelinePanel`, already live at ~5 Hz, showing avg/peak
  for compositor & encoder, fps, present cadence, frame/drop counts. **This panel is NOT the
  target of this work** (it stays as-is).

Engine-side, `PipelineDiagnosticsAggregator` already measures **compositor submit**, **encode
latency**, and **disk write** as `RollingTimeWindow`s. **Acquire/Capture** and the
**VideoProcessorBlt color conversion** are not measured. The `RecordingDiagnosticsSnapshot`
(`libs/recorder_core/include/recorder_core/pipeline_diagnostics.h:216`) is published via
`DiagnosticsCallback` (`:248`) by `SessionStatsCollector::Run()`
(`libs/recorder_core/src/session_stats_collector.cpp:25-27`, ~5 Hz / 198 ms).

## Goal

Make the six "CAPTURE PIPELINE" step cards **live during recording** so an inexperienced user can
see, at a glance, **which steps a recording goes through, how long each takes, and which resource
(CPU/GPU) each uses**. Clarity over depth. A step is flagged only when it is a *real* bottleneck
(it breaches the per-frame budget). Outside recording the cards show the static readiness/capability
status (as today, minus the "not instrumented" notes).

Non-goals: changing the LIVE PIPELINE panel; per-frame UI; GPU memory/utilization counters;
NVML/NVAPI; surfacing this anywhere but the Diagnostics PipelineFlow cards.

## Card Model

Each card = one pipeline step. Card face shows: step name, **avg duration in ms** (the headline
number), and a **CPU/GPU resource tag**. Peak goes in the tooltip.

| # | Card | Measures | Resource | Source |
|---|------|----------|----------|--------|
| 0 | Source Capture | Acquire + CopyResource duration | CPU | NEW `acquire_window_` (CPU QPC) |
| 1 | Frame Queue | Frame/packet queue **depth + backpressure** (a count, not a duration) | — | NEW queue-depth gauge; **excluded from bottleneck** |
| 2 | Compositor | Composite **+ VPBlt color-convert** | GPU | NEW `compositor_gpu_window_` + `vpblt_gpu_window_` (D3D11 GPU timestamp); CPU-submit `compositor_window_` kept as fallback |
| 3 | Encoder | NVENC encode latency (submit → bitstream-ready) | GPU (NVENC) | existing `encode_window_` (CPU-observed; see Measurement — D3D11 timestamps do NOT apply to NVENC) |
| 4 | Muxer | Mux/interleave processing | CPU | NEW `mux_window_` (CPU QPC), distinct from disk write |
| 5 | Disk | Filesystem write/flush | CPU | existing `write_window_` (CPU QPC) |

Frame Queue is backed by the **real frame/packet queue** that already exists between the encode and
mux threads (`m_state.mux_queue`) plus the existing backpressure drop counters — it shows current
(and peak) queue depth as a simple health indicator ("flowing" vs "backing up"), never a duration,
and is never a bottleneck candidate (a parallel buffer, not a serial per-frame cost). The exact
queue handle and depth accessor are pinned in the implementation plan; the card must not invent a
queue that doesn't exist — if no meaningful depth is available it shows a neutral "—".

## Measurement & Data Flow

**Hybrid, sampled, low-overhead** (recording is the use case; 0.5 s card refresh is acceptable, so
we trade temporal granularity for near-zero hot-path cost):

- **True GPU stages (Compositor incl. VPBlt) — D3D11 timestamps:** a new `GpuTimestampSampler`
  (in `recorder_core`) wrapping D3D11 timestamp queries — one `D3D11_QUERY_TIMESTAMP_DISJOINT` per
  sampled frame plus `D3D11_QUERY_TIMESTAMP` begin/end pairs around the composite shader pass and
  the `VideoProcessorBlt`. A small ring (2–3 frame slots) defers readback so `GetData` never stalls
  the pipeline (read a slot 2–3 frames old; skip if `S_FALSE`/not ready). Duration ms =
  `(end - begin) / disjoint.Frequency * 1000`, **discarded when `disjoint.Disjoint == TRUE` or
  `Frequency == 0`** (see Reliability below). This is the only component that uses D3D11 timestamps.
- **Encoder (NVENC) — CPU-observed submit→ready latency, NOT D3D11 timestamps:** NVENC runs on a
  dedicated encoder ASIC driven through the NVENC API, *not* the D3D11 command stream, so a D3D11
  timestamp query around the encode call measures nothing meaningful. The honest, GPU-relevant
  number is the latency from `EncodeFrame` submit to bitstream-ready — exactly what the existing
  `encode_window_` (`OnEncodeLatency`) already captures. The Encoder card is tagged "GPU (NVENC)"
  but its number is CPU-observed by construction; no timestamp query is added for it.
- **CPU stages (Acquire, Muxer, Disk):** plain QPC start/stop, also only on sampled frames.
- **Sampling:** measure roughly every Nth frame so ~5–10 samples/s reach the rolling windows
  (e.g. `N = max(1, target_fps / 8)`). Non-sampled frames carry zero query/timing overhead.
- **Aggregation:** all durations feed `RollingTimeWindow`s in `PipelineDiagnosticsAggregator`.
  New windows: `acquire_window_` (CPU), `compositor_gpu_window_` + `vpblt_gpu_window_` (GPU),
  `mux_window_` (CPU). Reused as-is: `encode_window_` (Encoder card), `write_window_` (Disk card),
  and `compositor_window_` (existing CPU-submit — kept as the Compositor card's fallback and for the
  untouched LIVE PIPELINE panel). New fields land in `CaptureDiagnostics`
  (`pipeline_diagnostics.h:88`) and the relevant per-stage diagnostic structs, each guarded by a
  `MetricAvailability` flag plus a "measured via GPU timestamp vs CPU submit" marker for the GPU
  stages.
- **Transport:** the existing `DiagnosticsCallback` (~5 Hz). **The UI throttles applying to the
  cards to 2 Hz (0.5 s)** — a timestamp/last-applied guard in the page's apply path, independent of
  the LIVE PIPELINE panel which keeps its current cadence.

## Bottleneck — pure resolver

A free, unit-testable function decides bottleneck status; the UI only renders the verdict:

```cpp
// frame_budget_ms = 1000.0 / target_fps. A duration stage is a bottleneck when its average
// exceeds the per-frame budget — i.e. it cannot sustain the target rate. Non-duration stages
// (Frame Queue) and unavailable stages are never bottlenecks.
struct StageTiming { StageId id; double avg_ms; bool is_duration_stage; bool available; };
struct BottleneckVerdict { /* per-stage is_bottleneck flag */ };
BottleneckVerdict ResolvePipelineBottleneck(std::span<const StageTiming> stages,
                                            double frame_budget_ms);
```

When all stages are within budget, no card is highlighted (the "only real bottleneck" decision).
When a stage breaches budget, its card gets a warning color (amber/coral) and a plain-language
hint, e.g. "Encoder can't keep up with 60 fps."

## States & Fallback

- **Idle (not recording):** cards show the static readiness/capability status (as today, without the
  "not instrumented yet" notes).
- **Recording:** live avg durations + resource tags, bottleneck highlight only on budget breach.
- **GPU timestamp unreliable** (`disjoint.Disjoint == TRUE`, `Frequency == 0`, query unsupported,
  or `DXGI_ERROR_DEVICE_REMOVED`): the affected GPU card falls back to its CPU-submit measurement
  (compositor/encode already have CPU-submit windows) and the tooltip marks it "CPU submit (GPU time
  unavailable)". A single disjoint sample is simply dropped (keep the last good average); only a
  persistent disjoint condition flips the card to the CPU-submit label. **Never crash, never show a
  fabricated number.**

### When is a GPU timestamp "unreliable"?

The D3D11 disjoint query reports `Disjoint == TRUE` when the GPU clock frequency was **not constant**
across the begin/end window, so the tick delta can't be converted to a trustworthy time. In a
recording-while-gaming scenario the dominant cause is **GPU DVFS / boost-clock changes under
variable game load** — exactly when the GPU ramps clocks up/down, which is common during gameplay.
Other causes: the GPU transitioning out of an idle/clock-gated state; a TDR/device reset
(`DXGI_ERROR_DEVICE_REMOVED`); `Frequency == 0`; and drivers/feature levels (or WARP/software
adapters) where timestamp queries aren't reliably supported. (`GetData` returning `S_FALSE` is *not*
unreliability — the sample just isn't ready yet; defer the read.) Because disjoint samples are
expected intermittently, the design **discards** them rather than treating them as errors, and only
a *sustained* disjoint/unsupported condition switches the card to the CPU-submit fallback label.

## Testing

- **Pure:** `ResolvePipelineBottleneck` — within-budget → none; one/multiple over-budget; Frame
  Queue excluded; unavailable/partial data; empty input.
- **Aggregator:** new rolling windows produce correct avg/peak from fed samples.
- **UI:** `PipelineFlow` card reflects snapshot fields (avg ms, resource tag, bottleneck highlight,
  idle↔live transition, CPU-submit fallback marker).
- **GPU sampler hot-path:** dev-machine-verified by a real recording (not headless), as with the CFR
  pacer — confirm cards populate, durations are plausible, no stutter introduced, disjoint samples
  are dropped gracefully.

## File / Component Map

| File | Change |
|------|--------|
| `libs/recorder_core/src/gpu_timestamp_sampler.{h,cpp}` | NEW — sampled D3D11 timestamp ring for the composite pass + VPBlt only (NOT NVENC) |
| `libs/recorder_core/include/recorder_core/pipeline_bottleneck.{h}` + `src/.cpp` | NEW — pure `ResolvePipelineBottleneck` + `StageTiming`/`BottleneckVerdict` |
| `libs/recorder_core/include/recorder_core/pipeline_diagnostics.h` | New `CaptureDiagnostics`/stage fields (acquire/vpblt/mux avg+peak+latest, queue depth, availability + GPU/CPU-source markers) |
| `libs/recorder_core/src/pipeline_diagnostics_aggregator.{h,cpp}` | New `acquire_window_`, `compositor_gpu_window_`, `vpblt_gpu_window_`, `mux_window_`; `OnAcquireLatency`/`OnCompositeGpu`/`OnVpbltGpu`/`OnMuxLatency`; queue-depth gauge |
| `libs/recorder_core/src/video_thread.cpp` | QPC around Acquire; `GpuTimestampSampler` around Composite + VPBlt only; sampling cadence (encode keeps existing CPU-observed window) |
| `libs/recorder_core/src/mux_thread.cpp` | QPC around mux processing (distinct from existing disk-write tap) |
| `app/pages/DiagnosticsPage.cpp` | Light up PipelineFlow cards 0–5 from the live snapshot; 2 Hz throttle; idle↔live; bottleneck render |
| `app/ui/widgets/PipelineFlow.{h,cpp}` | Card face: avg ms + CPU/GPU tag + bottleneck color + tooltip(peak/fallback) |
| tests | `ResolvePipelineBottleneck`, aggregator windows, PipelineFlow card |

## Open follow-ups (out of scope here)

- True GPU utilization/VRAM (NVML/NVAPI) — capability-gated, later.
- Acquire latency for WGC vs DXGI-OD has different shapes; both measured, availability-flagged.
