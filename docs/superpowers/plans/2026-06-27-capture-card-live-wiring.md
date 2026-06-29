# Capture-Card Live Wiring (health-first v1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the six "CAPTURE PIPELINE" step cards on the Diagnostics page live during recording — each card showing a health status (Healthy / Busy / Bottleneck), a CPU/GPU resource tag, and one cheap secondary number — using data the pipeline already collects plus negligible CPU `QueryPerformanceCounter`/`steady_clock` brackets.

**Architecture:** A new pure, UI-free `ResolvePipelineHealth` resolver (recorder_core) classifies each stage from per-stage signals; the engine feeds three new cheap CPU-timing rolling windows into the existing `PipelineDiagnosticsAggregator`; the `RecordingDiagnosticsSnapshot` carries the new durations; the Diagnostics page (UI) builds `StageSignals` from the snapshot, calls the resolver, and renders verdicts onto the existing `PipelineFlow`/`PipelineStepCard` widgets at a throttled 2 Hz. The engine stays UI-agnostic; the resolver is the only shared classification logic.

**Tech Stack:** C++20 (`std::span`, `std::chrono::steady_clock`), GoogleTest, Qt 6.9 Widgets, CMake (VS 2022 generator tree `build/windows-x64-debug`), `exosnap_add_gtest` test macro.

## Global Constraints

Copied verbatim from the design spec and project rules. Every task's requirements implicitly include this section.

- **Health-first / status-primary.** Each card's primary signal is a status (Healthy / Busy / Bottleneck) derived from data the pipeline already collects. The secondary number is detail, not the headline.
- **NO GPU timestamp queries in v1.** No D3D11 timestamp/disjoint queries, no GPU→CPU syncs, no per-frame hot-path GPU work. Precise per-stage GPU microsecond timing is explicitly deferred.
- **Only negligible CPU instrumentation.** New measurements are CPU `QueryPerformanceCounter` / `steady_clock::now()` brackets around existing calls plus counter reads — a few nanoseconds, safe every frame, no sampling machinery.
- **Reuse existing windows/counters first.** Reuse `CaptureDiagnostics` fps/interval/drop/dup counters, `compositor_window_`, `encode_window_`, `write_window_`, low-disk state. Add only `acquire_window_`, `vpblt_window_`, `mux_window_`.
- **CPU-observed numbers carry an honest CPU/GPU resource tag + tooltip.** Compositor/Encoder run on the GPU but their numbers are CPU-observed (submit / submit→ready). The tag shows the true resource; the tooltip notes "CPU submit (GPU execution time not measured in this view)".
- **Frame Queue is bound to the real `mux_queue`, never a bottleneck.** It is a depth/health indicator only. Show "—" if no meaningful depth is available. Never invent a queue that doesn't exist.
- **"Only real bottleneck" highlight.** When every stage is keeping up, no card is alarmed (Healthy/Busy). Only a stage that genuinely can't keep up turns Bottleneck. "Busy" is a calm middle state, not an alarm.
- **2 Hz UI throttle.** The page throttles applying to the cards to ≤2 Hz (0.5 s) via a last-applied timestamp guard, independent of the LIVE PIPELINE panel which keeps its ~5 Hz cadence.
- **Engine stays UI-agnostic** (CLAUDE.md). No Qt / UI types in recorder_core. Keep VFR / WGC / Newest behavior untouched.
- **Build/test discipline.** Build in the VS-tree `build/windows-x64-debug`. Run a **full build** (no `--target`) so all test targets pick up new translation units, then full `ctest -j6` before commit. Qt must be on PATH for any rendering/visual step. Known `-j` flakes: `HistoryStore` / `PresetStore` tests — re-run serially to confirm.
- **No `emit` / `signals` / `slots` struct member names.** `pipeline_health.h` is included from Qt translation units; avoid Qt macro collisions (mirror `frame_pacing.h`'s guard discipline).

---

## File Structure

| File | Responsibility |
|------|----------------|
| `libs/recorder_core/include/recorder_core/pipeline_health.h` | NEW. Pure types `StageId`, `StageHealth`, `StageSignals`, `StageVerdict`, `PipelineHealthVerdict` + `ResolvePipelineHealth(span, frame_budget_ms)` declaration. UI-free. |
| `libs/recorder_core/src/pipeline_health.cpp` | NEW. `ResolvePipelineHealth` implementation + pinned thresholds. |
| `libs/recorder_core/tests/test_pipeline_health.cpp` | NEW. Pure unit tests for the resolver. |
| `libs/recorder_core/include/recorder_core/pipeline_diagnostics.h` | MODIFY. New snapshot fields: acquire (Capture), vpblt (Compositor), mux-process (Mux) latest/avg/peak + availability flags. No GPU-timing fields. |
| `libs/recorder_core/src/pipeline_diagnostics_aggregator.h` | MODIFY. New `acquire_window_`/`vpblt_window_`/`mux_window_` + `*_observed_` flags + `OnAcquireLatency`/`OnVpbltSubmit`/`OnMuxLatency` declarations. |
| `libs/recorder_core/src/pipeline_diagnostics_aggregator.cpp` | MODIFY. Implement the three taps; clear in `Reset`; populate the new snapshot fields in `BuildSnapshot`. |
| `libs/recorder_core/tests/test_pipeline_diagnostics.cpp` | MODIFY. Aggregator tests for the three new windows. |
| `libs/recorder_core/src/video_thread.cpp` | MODIFY. Cheap brackets around Acquire (DXGI-OD + WGC drain) and around `VideoProcessorBlt` (CFR + VFR); feed `OnAcquireLatency`/`OnVpbltSubmit`. |
| `libs/recorder_core/src/mux_thread.cpp` | MODIFY. Cheap bracket around the mux drain loop; feed `OnMuxLatency`. (Queue depth already wired via `OnVideoQueueDepth`.) |
| `app/ui/widgets/PipelineStepCard.{h,cpp}` | MODIFY. Card face: resource-tag label + secondary-number label + tooltip, plus `Status` mapping for Busy/Bottleneck. |
| `app/ui/widgets/PipelineFlow.{h,cpp}` | MODIFY. `setStepLive(...)` convenience to push status + note + resource + number + tooltip. |
| `app/tests/test_pipeline_flow.cpp` | MODIFY. Widget test for the new card face. |
| `app/pages/DiagnosticsPage.{h,cpp}` | MODIFY. Build `StageSignals` from the snapshot, call `ResolvePipelineHealth`, push verdicts to the six cards; 2 Hz throttle; idle↔live transition. |
| `app/tests/test_diagnostics_page.cpp` | MODIFY. UI tests for live cards, idle reset, "—" when unavailable. |
| `app/MainWindow.cpp` | MODIFY (Task 5/6). Extend `makeLiveDiagnosticsSnapshot` to set the new synthetic fields so the existing diagnostics visual scenarios render real numbers. |
| `libs/recorder_core/CMakeLists.txt` | MODIFY. Register `pipeline_health_tests` after the existing `frame_pacing_tests` block (main block, after the NVENC `endif()`). |
| `docs/superpowers/specs/2026-06-27-capture-card-live-wiring-design.md` | MODIFY (Task 6). Mark delivered. |
| `docs/roadmap.md` | MODIFY (Task 6). Roadmap line. |

### Key findings from reading the real code (deviations from the prompt's task sketch)

1. **The Frame-Queue depth gauge already exists.** `mux_thread.cpp:534` already calls `OnVideoQueueDepth(mux_queue.size())`, and `BuildSnapshot` already populates `s.video_queue.current_depth` / `s.video_queue.peak_depth` from the real `m_state.mux_queue`. **Task 2 does NOT add a new queue gauge** — Task 5 reads the existing `snapshot.video_queue`. This is the spec's "real `mux_queue`" requirement, already satisfied engine-side.
2. **Existing QPC brackets use `std::chrono::steady_clock::now()`**, not raw `QueryPerformanceCounter`, e.g. the compositor tap at `video_thread.cpp:1736-1741` and the encode tap at `:1806-1810`. The new brackets mirror that idiom (the aggregator windows are keyed on `steady_clock::time_point`). "QPC bracket" in the spec == this `steady_clock` bracket.
3. **`VideoProcessorBlt` is a distinct call** (`video_thread.cpp:1766` CFR, ~`:2030` VFR) from `compositeFrameGpu` (the existing `compositor_window_` bracket). The new `vpblt_window_` brackets only the `VideoProcessorBlt` submit, and its number is folded into the **Compositor** card per the spec.
4. **`PipelineStepCard::Status` already has `Hotspot` and `Over`** (reserved for live timing). Map `StageHealth::Busy → Hotspot`, `StageHealth::Bottleneck → Over`, `StageHealth::Healthy → Ok`. No new pill vocabulary needed.
5. The honesty test `PipelineFlowTest.CardsCarryNoFakeLiveMetrics` asserts no QLabel contains `" ms"`/`"fps"` — it only exercises static `setStepStatus`. New live labels default empty, so it keeps passing. Do not populate live numbers in that test.

---

## Task 1: Pure `ResolvePipelineHealth` resolver (TDD)

**Files:**
- Create: `libs/recorder_core/include/recorder_core/pipeline_health.h`
- Create: `libs/recorder_core/src/pipeline_health.cpp`
- Test: `libs/recorder_core/tests/test_pipeline_health.cpp`
- Modify: `libs/recorder_core/CMakeLists.txt` (register `pipeline_health_tests`)

**Interfaces:**
- Produces (used by Task 5):
  - `enum class recorder_core::StageId : uint8_t { SourceCapture, FrameQueue, Compositor, Encoder, Muxer, Disk }`
  - `enum class recorder_core::StageHealth : uint8_t { Healthy, Busy, Bottleneck }`
  - `struct StageSignals { StageId id; bool available; bool is_duration_stage; bool can_bottleneck; double avg_ms; double budget_ms; double fps_ratio; uint32_t recent_drops; uint32_t queue_depth; uint32_t queue_busy_threshold; }`
  - `struct StageVerdict { StageId id; StageHealth health; }`
  - `struct PipelineHealthVerdict { std::vector<StageVerdict> per_stage; bool has_bottleneck; StageId bottleneck; }`
  - `PipelineHealthVerdict ResolvePipelineHealth(std::span<const StageSignals> stages, double frame_budget_ms)`
  - `constexpr const char* ToString(StageHealth)` (for logs/tests)

**Pinned thresholds (in `pipeline_health.cpp`, defensible defaults):**
- `kBusyRatio = 0.8` — a duration stage is **Busy** when `avg_ms >= 0.8 * budget`.
- A duration stage is **Bottleneck** when `avg_ms > budget` (strictly over) **OR** `recent_drops > 0` (it shed frames this window).
- `kCaptureBottleneckRatio = 0.85` — capture (fps-based, non-duration) is **Bottleneck** when `fps_ratio < 0.85` or `recent_drops > 0`.
- `kCaptureBusyRatio = 0.95` — capture is **Busy** when `fps_ratio < 0.95`.
- Frame Queue (`can_bottleneck == false`): **Busy** when `queue_busy_threshold > 0 && queue_depth >= queue_busy_threshold`; otherwise **Healthy**. Never Bottleneck.
- `available == false` → **Healthy** (neutral, never alarms; UI renders the number as "—" separately).
- Bottleneck selection: among stages classified Bottleneck, the **most-downstream** wins (rank Disk > Muxer > Encoder > Compositor > SourceCapture; FrameQueue never participates).

- [ ] **Step 1: Write the failing test**

Create `libs/recorder_core/tests/test_pipeline_health.cpp`:

```cpp
#include <gtest/gtest.h>

#include "recorder_core/pipeline_health.h"

#include <vector>

namespace {

using namespace recorder_core;

// 60 fps budget = 16.667 ms.
constexpr double kBudget = 1000.0 / 60.0;

StageSignals Duration(StageId id, double avg_ms, bool can_bottleneck = true, double budget_ms = 0.0) {
    StageSignals s;
    s.id = id;
    s.available = true;
    s.is_duration_stage = true;
    s.can_bottleneck = can_bottleneck;
    s.avg_ms = avg_ms;
    s.budget_ms = budget_ms;
    s.fps_ratio = 1.0;
    return s;
}

StageSignals Capture(double fps_ratio, uint32_t recent_drops = 0) {
    StageSignals s;
    s.id = StageId::SourceCapture;
    s.available = true;
    s.is_duration_stage = false;
    s.can_bottleneck = true;
    s.fps_ratio = fps_ratio;
    s.recent_drops = recent_drops;
    return s;
}

StageSignals Queue(uint32_t depth, uint32_t busy_threshold) {
    StageSignals s;
    s.id = StageId::FrameQueue;
    s.available = true;
    s.is_duration_stage = false;
    s.can_bottleneck = false; // never a bottleneck candidate
    s.queue_depth = depth;
    s.queue_busy_threshold = busy_threshold;
    return s;
}

StageHealth HealthOf(const PipelineHealthVerdict& v, StageId id) {
    for (const auto& sv : v.per_stage)
        if (sv.id == id)
            return sv.health;
    ADD_FAILURE() << "stage not found";
    return StageHealth::Healthy;
}

TEST(ResolvePipelineHealth, EmptyInputNoBottleneck) {
    const auto v = ResolvePipelineHealth({}, kBudget);
    EXPECT_TRUE(v.per_stage.empty());
    EXPECT_FALSE(v.has_bottleneck);
}

TEST(ResolvePipelineHealth, AllHealthyNoBottleneck) {
    std::vector<StageSignals> stages = {
        Capture(0.997), Queue(1, 8), Duration(StageId::Compositor, 1.4),
        Duration(StageId::Encoder, 2.1), Duration(StageId::Muxer, 0.8),
        Duration(StageId::Disk, 0.8, /*can_bottleneck=*/true, /*budget_ms=*/8.0)};
    const auto v = ResolvePipelineHealth(stages, kBudget);
    EXPECT_FALSE(v.has_bottleneck);
    for (const auto& sv : v.per_stage)
        EXPECT_EQ(sv.health, StageHealth::Healthy) << ToString(sv.health);
}

TEST(ResolvePipelineHealth, DurationOverBudgetIsBottleneck) {
    std::vector<StageSignals> stages = {Duration(StageId::Encoder, kBudget + 5.0)};
    const auto v = ResolvePipelineHealth(stages, kBudget);
    EXPECT_EQ(HealthOf(v, StageId::Encoder), StageHealth::Bottleneck);
    EXPECT_TRUE(v.has_bottleneck);
    EXPECT_EQ(v.bottleneck, StageId::Encoder);
}

TEST(ResolvePipelineHealth, DurationNearBudgetIsBusyNotBottleneck) {
    std::vector<StageSignals> stages = {Duration(StageId::Compositor, kBudget * 0.85)};
    const auto v = ResolvePipelineHealth(stages, kBudget);
    EXPECT_EQ(HealthOf(v, StageId::Compositor), StageHealth::Busy);
    EXPECT_FALSE(v.has_bottleneck);
}

TEST(ResolvePipelineHealth, DropDrivenBottleneck) {
    std::vector<StageSignals> stages = {Duration(StageId::Encoder, 2.0)};
    stages[0].recent_drops = 4; // shedding frames despite low avg
    const auto v = ResolvePipelineHealth(stages, kBudget);
    EXPECT_EQ(HealthOf(v, StageId::Encoder), StageHealth::Bottleneck);
}

TEST(ResolvePipelineHealth, CaptureBelowTargetIsBottleneck) {
    std::vector<StageSignals> stages = {Capture(0.80)};
    const auto v = ResolvePipelineHealth(stages, kBudget);
    EXPECT_EQ(HealthOf(v, StageId::SourceCapture), StageHealth::Bottleneck);
}

TEST(ResolvePipelineHealth, CaptureSlightlyBelowIsBusy) {
    std::vector<StageSignals> stages = {Capture(0.92)};
    const auto v = ResolvePipelineHealth(stages, kBudget);
    EXPECT_EQ(HealthOf(v, StageId::SourceCapture), StageHealth::Busy);
}

TEST(ResolvePipelineHealth, FrameQueueNeverBottleneckEvenWhenDeep) {
    std::vector<StageSignals> stages = {Queue(999, 8)};
    const auto v = ResolvePipelineHealth(stages, kBudget);
    EXPECT_EQ(HealthOf(v, StageId::FrameQueue), StageHealth::Busy);
    EXPECT_FALSE(v.has_bottleneck);
}

TEST(ResolvePipelineHealth, UnavailableStageIsHealthyNeutral) {
    StageSignals s = Duration(StageId::Compositor, kBudget + 99.0);
    s.available = false; // no signal → must not alarm
    const auto v = ResolvePipelineHealth({&s, 1}, kBudget);
    EXPECT_EQ(HealthOf(v, StageId::Compositor), StageHealth::Healthy);
    EXPECT_FALSE(v.has_bottleneck);
}

TEST(ResolvePipelineHealth, MostDownstreamBottleneckWins) {
    std::vector<StageSignals> stages = {
        Duration(StageId::Compositor, kBudget + 1.0), // bottleneck (upstream)
        Duration(StageId::Disk, 99.0, true, 8.0)};    // bottleneck (downstream)
    const auto v = ResolvePipelineHealth(stages, kBudget);
    EXPECT_TRUE(v.has_bottleneck);
    EXPECT_EQ(v.bottleneck, StageId::Disk);
}

} // namespace
```

- [ ] **Step 2: Register the test target**

In `libs/recorder_core/CMakeLists.txt`, immediately after the `frame_pacing_tests` block (currently ending at line ~706 with `target_include_directories(frame_pacing_tests ...)`), append. This is in the **main block, after the NVENC `endif()`** — do NOT add it inside the NVENC-fallback `if/else` (which duplicates registrations):

```cmake
# Capture-card live wiring (0.8.0): pure stage-health resolver (no D3D/NVENC/Qt).
exosnap_add_gtest(
    NAME pipeline_health_tests
    TEST_PREFIX pipeline_health.
    SOURCES tests/test_pipeline_health.cpp
            src/pipeline_health.cpp
)
target_include_directories(pipeline_health_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
```

- [ ] **Step 3: Run test to verify it fails (configure + build the new target)**

Run:
```bash
cmake --build build/windows-x64-debug --target pipeline_health_tests
```
Expected: FAIL — `pipeline_health.h: No such file or directory` (header/impl not yet created).

- [ ] **Step 4: Write the header**

Create `libs/recorder_core/include/recorder_core/pipeline_health.h`:

```cpp
#pragma once

// Pure, UI-free per-stage health classification for the Diagnostics capture cards
// (Capture-Card Live Wiring, 0.8.0). Distilled signals in, per-stage verdict + the
// single most-downstream bottleneck out. No Qt, no engine, no I/O — unit-testable.
//
// NOTE: this header is included from Qt translation units (DiagnosticsPage.cpp).
// Do not name any member `emit`, `signals`, or `slots` (Qt macro collisions).

#include <cstdint>
#include <span>
#include <vector>

namespace recorder_core {

// Canonical capture-pipeline stages, left to right (mirrors PipelineFlow card order).
enum class StageId : uint8_t {
    SourceCapture = 0,
    FrameQueue = 1,
    Compositor = 2,
    Encoder = 3,
    Muxer = 4,
    Disk = 5,
};

// Health-first vocabulary. Healthy = comfortably within budget; Busy = working hard
// but keeping up (calm, no alarm); Bottleneck = genuinely cannot keep up (alarm).
enum class StageHealth : uint8_t {
    Healthy = 0,
    Busy = 1,
    Bottleneck = 2,
};

// Per-stage inputs distilled from the snapshot by the UI. The resolver never reads a
// snapshot directly — it only sees these.
struct StageSignals {
    StageId id = StageId::SourceCapture;
    bool available = false;          // false → no live signal this stage; classify Healthy (neutral)
    bool is_duration_stage = false;  // true → avg_ms is compared to a budget
    bool can_bottleneck = true;      // FrameQueue sets this false
    double avg_ms = 0.0;             // observed average (duration stages)
    double budget_ms = 0.0;          // per-stage budget override; <=0 → use frame_budget_ms
    double fps_ratio = 1.0;          // actual/target fps (capture); 1.0 = on target
    uint32_t recent_drops = 0;       // frames shed over the window (any shedding proxy)
    uint32_t queue_depth = 0;        // FrameQueue current depth
    uint32_t queue_busy_threshold = 0; // depth at/above which the queue reads Busy (0 → never Busy)
};

struct StageVerdict {
    StageId id = StageId::SourceCapture;
    StageHealth health = StageHealth::Healthy;
};

struct PipelineHealthVerdict {
    std::vector<StageVerdict> per_stage;       // one entry per input stage, input order
    bool has_bottleneck = false;               // true iff any stage is Bottleneck
    StageId bottleneck = StageId::SourceCapture; // most-downstream bottleneck (valid iff has_bottleneck)
};

// Classify every stage and pick the single most-downstream bottleneck (if any).
// frame_budget_ms is 1000/target_fps; per-stage budget_ms overrides it when > 0.
[[nodiscard]] PipelineHealthVerdict ResolvePipelineHealth(std::span<const StageSignals> stages,
                                                          double frame_budget_ms);

[[nodiscard]] constexpr const char* ToString(StageHealth h) noexcept {
    switch (h) {
    case StageHealth::Healthy:
        return "Healthy";
    case StageHealth::Busy:
        return "Busy";
    case StageHealth::Bottleneck:
        return "Bottleneck";
    }
    return "Healthy";
}

} // namespace recorder_core
```

- [ ] **Step 5: Write the implementation**

Create `libs/recorder_core/src/pipeline_health.cpp`:

```cpp
#include "recorder_core/pipeline_health.h"

namespace recorder_core {

namespace {

// Pinned, defensible thresholds (see plan Task 1).
constexpr double kBusyRatio = 0.8;             // duration stage Busy at >= 0.8 * budget
constexpr double kCaptureBottleneckRatio = 0.85; // capture Bottleneck below 0.85 of target
constexpr double kCaptureBusyRatio = 0.95;     // capture Busy below 0.95 of target

StageHealth ClassifyStage(const StageSignals& s, double frame_budget_ms) {
    if (!s.available) {
        return StageHealth::Healthy; // no signal → neutral, never alarms
    }

    // Any shedding escalates a bottleneck-capable stage immediately.
    if (s.can_bottleneck && s.recent_drops > 0) {
        return StageHealth::Bottleneck;
    }

    if (s.is_duration_stage) {
        const double budget = (s.budget_ms > 0.0) ? s.budget_ms : frame_budget_ms;
        if (budget <= 0.0) {
            return StageHealth::Healthy;
        }
        if (s.can_bottleneck && s.avg_ms > budget) {
            return StageHealth::Bottleneck;
        }
        if (s.avg_ms >= kBusyRatio * budget) {
            return StageHealth::Busy;
        }
        return StageHealth::Healthy;
    }

    // Frame Queue: depth-only, never a bottleneck candidate.
    if (s.id == StageId::FrameQueue) {
        if (s.queue_busy_threshold > 0 && s.queue_depth >= s.queue_busy_threshold) {
            return StageHealth::Busy;
        }
        return StageHealth::Healthy;
    }

    // Capture: fps-ratio driven (non-duration).
    if (s.fps_ratio > 0.0) {
        if (s.can_bottleneck && s.fps_ratio < kCaptureBottleneckRatio) {
            return StageHealth::Bottleneck;
        }
        if (s.fps_ratio < kCaptureBusyRatio) {
            return StageHealth::Busy;
        }
    }
    return StageHealth::Healthy;
}

// Higher rank == more downstream (the true root when several stages stall).
int DownstreamRank(StageId id) {
    switch (id) {
    case StageId::Disk:
        return 5;
    case StageId::Muxer:
        return 4;
    case StageId::Encoder:
        return 3;
    case StageId::Compositor:
        return 2;
    case StageId::SourceCapture:
        return 1;
    case StageId::FrameQueue:
        return 0; // never a bottleneck
    }
    return 0;
}

} // namespace

PipelineHealthVerdict ResolvePipelineHealth(std::span<const StageSignals> stages, double frame_budget_ms) {
    PipelineHealthVerdict v;
    v.per_stage.reserve(stages.size());

    int best_rank = -1;
    for (const StageSignals& s : stages) {
        const StageHealth h = ClassifyStage(s, frame_budget_ms);
        v.per_stage.push_back(StageVerdict{s.id, h});
        if (h == StageHealth::Bottleneck) {
            const int rank = DownstreamRank(s.id);
            if (rank > best_rank) {
                best_rank = rank;
                v.has_bottleneck = true;
                v.bottleneck = s.id;
            }
        }
    }
    return v;
}

} // namespace recorder_core
```

- [ ] **Step 6: Build and run the test to verify it passes**

Run:
```bash
cmake --build build/windows-x64-debug --target pipeline_health_tests
ctest --test-dir build/windows-x64-debug -R "pipeline_health\." --output-on-failure
```
Expected: PASS — all 11 `pipeline_health.*` tests green.

- [ ] **Step 7: Commit**

```bash
git add libs/recorder_core/include/recorder_core/pipeline_health.h \
        libs/recorder_core/src/pipeline_health.cpp \
        libs/recorder_core/tests/test_pipeline_health.cpp \
        libs/recorder_core/CMakeLists.txt
git commit -m "feat(diagnostics): pure ResolvePipelineHealth stage-health resolver (TDD)"
```

---

## Task 2: Snapshot fields + aggregator CPU-timing windows (TDD)

**Files:**
- Modify: `libs/recorder_core/include/recorder_core/pipeline_diagnostics.h`
- Modify: `libs/recorder_core/src/pipeline_diagnostics_aggregator.h`
- Modify: `libs/recorder_core/src/pipeline_diagnostics_aggregator.cpp`
- Test: `libs/recorder_core/tests/test_pipeline_diagnostics.cpp` (existing target `test_pipeline_diagnostics`)

**Interfaces:**
- Consumes: `RollingTimeWindow`, `MetricAvailability`, `RecordingDiagnosticsSnapshot` (Task 0 / existing).
- Produces (used by Tasks 3 & 5):
  - `CaptureDiagnostics`: `double acquire_latest_ms, acquire_average_ms, acquire_peak_ms; MetricAvailability acquire_availability`
  - `CompositorDiagnostics`: `double vpblt_latest_ms, vpblt_average_ms, vpblt_peak_ms; MetricAvailability vpblt_availability`
  - `MuxDiagnostics`: `double process_latest_ms, process_average_ms, process_peak_ms; MetricAvailability process_availability`
  - `PipelineDiagnosticsAggregator::OnAcquireLatency(time_point, double)`
  - `PipelineDiagnosticsAggregator::OnVpbltSubmit(time_point, double)`
  - `PipelineDiagnosticsAggregator::OnMuxLatency(time_point, double)`

Note: the Frame-Queue depth gauge is **already** in the snapshot (`s.video_queue.current_depth`/`peak_depth`, fed by `OnVideoQueueDepth`). No new queue field is added.

- [ ] **Step 1: Write the failing aggregator tests**

Append to `libs/recorder_core/tests/test_pipeline_diagnostics.cpp` (before the final closing `} // namespace`; the file already provides `MakeConfig`, `MakeStats`, `At`):

```cpp
// ---------------------------------------------------------------------------
// Capture-card live wiring: acquire / vpblt / mux-process CPU-timing windows
// ---------------------------------------------------------------------------

TEST(CaptureCardWiring, AcquireWindowAverageAndPeak) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnAcquireLatency(At(0), 1.0);
    agg.OnAcquireLatency(At(10), 3.0);
    agg.OnAcquireLatency(At(20), 2.0);
    const auto s = agg.BuildSnapshot(At(30), MakeStats(), DiagnosticsLifecycle::Recording, 0.03);
    EXPECT_EQ(s.capture.acquire_availability, MetricAvailability::Available);
    EXPECT_DOUBLE_EQ(s.capture.acquire_average_ms, 2.0);
    EXPECT_DOUBLE_EQ(s.capture.acquire_peak_ms, 3.0);
    EXPECT_DOUBLE_EQ(s.capture.acquire_latest_ms, 2.0);
}

TEST(CaptureCardWiring, AcquireUnavailableWhenNoSamples) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    const auto s = agg.BuildSnapshot(At(0), MakeStats(), DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_EQ(s.capture.acquire_availability, MetricAvailability::Unavailable);
    EXPECT_DOUBLE_EQ(s.capture.acquire_average_ms, 0.0);
}

TEST(CaptureCardWiring, VpbltWindowFoldsIntoCompositor) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnVpbltSubmit(At(0), 0.4);
    agg.OnVpbltSubmit(At(16), 0.6);
    const auto s = agg.BuildSnapshot(At(20), MakeStats(), DiagnosticsLifecycle::Recording, 0.02);
    EXPECT_EQ(s.compositor.vpblt_availability, MetricAvailability::Available);
    EXPECT_DOUBLE_EQ(s.compositor.vpblt_average_ms, 0.5);
    EXPECT_DOUBLE_EQ(s.compositor.vpblt_peak_ms, 0.6);
}

TEST(CaptureCardWiring, MuxProcessWindow) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnMuxLatency(At(0), 0.2);
    agg.OnMuxLatency(At(5), 0.8);
    const auto s = agg.BuildSnapshot(At(10), MakeStats(), DiagnosticsLifecycle::Recording, 0.01);
    EXPECT_EQ(s.mux.process_availability, MetricAvailability::Available);
    EXPECT_DOUBLE_EQ(s.mux.process_average_ms, 0.5);
    EXPECT_DOUBLE_EQ(s.mux.process_peak_ms, 0.8);
}

TEST(CaptureCardWiring, ResetClearsNewWindows) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnAcquireLatency(At(0), 5.0);
    agg.Reset(2, MakeConfig()); // new session
    const auto s = agg.BuildSnapshot(At(0), MakeStats(), DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_EQ(s.capture.acquire_availability, MetricAvailability::Unavailable);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run:
```bash
cmake --build build/windows-x64-debug --target test_pipeline_diagnostics
```
Expected: FAIL — `OnAcquireLatency` / `acquire_availability` are not members.

- [ ] **Step 3: Add the snapshot fields**

In `libs/recorder_core/include/recorder_core/pipeline_diagnostics.h`, extend `CaptureDiagnostics` (after the present-mode block, before `frames_dropped_total()`):

```cpp
    // Acquire+copy CPU duration (Source Capture card). steady_clock bracket around the
    // backend acquire/drain; NOT GPU time. Unavailable until the first sample this session.
    double acquire_latest_ms = 0.0;
    double acquire_average_ms = 0.0;
    double acquire_peak_ms = 0.0;
    MetricAvailability acquire_availability = MetricAvailability::Unavailable;
```

Extend `CompositorDiagnostics` (after the existing CPU-submission comment):

```cpp
    // VideoProcessorBlt (BGRA->NV12) CPU submission duration, folded into the Compositor
    // card. steady_clock bracket around VideoProcessorBlt; NOT GPU execution time.
    double vpblt_latest_ms = 0.0;
    double vpblt_average_ms = 0.0;
    double vpblt_peak_ms = 0.0;
    MetricAvailability vpblt_availability = MetricAvailability::Unavailable;
```

Extend `MuxDiagnostics` (after `availability`):

```cpp
    // Mux drain-loop CPU processing duration (Muxer card). steady_clock bracket around
    // the per-iteration queue drain; Unavailable until the first batch is processed.
    double process_latest_ms = 0.0;
    double process_average_ms = 0.0;
    double process_peak_ms = 0.0;
    MetricAvailability process_availability = MetricAvailability::Unavailable;
```

- [ ] **Step 4: Add the aggregator windows, flags, and tap declarations**

In `libs/recorder_core/src/pipeline_diagnostics_aggregator.h`:

Add tap declarations in the worker-inputs block, right after `OnCompositorSubmit` (keep the grouping sensible):

```cpp
    // Capture-card live wiring (0.8.0): cheap CPU-timing brackets (steady_clock).
    void OnAcquireLatency(time_point now, double ms) noexcept; // acquire+copy (Source Capture)
    void OnVpbltSubmit(time_point now, double ms) noexcept;    // VideoProcessorBlt (Compositor)
    void OnMuxLatency(time_point now, double ms) noexcept;     // mux drain loop (Muxer)
```

Add member windows + flags. Place the acquire window near the capture members, the vpblt window near the compositor members, and the mux window near the mux/disk members:

```cpp
    // Capture-card live wiring (0.8.0). Each is a steady_clock CPU-timing window.
    bool acquire_observed_ = false;
    RollingTimeWindow acquire_window_{256, std::chrono::milliseconds(2000)};
    bool vpblt_observed_ = false;
    RollingTimeWindow vpblt_window_{256, std::chrono::milliseconds(2000)};
    bool mux_process_observed_ = false;
    RollingTimeWindow mux_window_{256, std::chrono::milliseconds(2000)};
```

- [ ] **Step 5: Implement the taps, Reset clearing, and BuildSnapshot population**

In `libs/recorder_core/src/pipeline_diagnostics_aggregator.cpp`:

Add tap implementations (after `OnCompositorSubmit`, near the other worker inputs):

```cpp
void PipelineDiagnosticsAggregator::OnAcquireLatency(time_point now, double ms) noexcept {
    std::lock_guard lk(mutex_);
    acquire_observed_ = true;
    acquire_window_.Add(now, ms);
}

void PipelineDiagnosticsAggregator::OnVpbltSubmit(time_point now, double ms) noexcept {
    std::lock_guard lk(mutex_);
    vpblt_observed_ = true;
    vpblt_window_.Add(now, ms);
}

void PipelineDiagnosticsAggregator::OnMuxLatency(time_point now, double ms) noexcept {
    std::lock_guard lk(mutex_);
    mux_process_observed_ = true;
    mux_window_.Add(now, ms);
}
```

In `Reset`, after the existing `compositor_window_.Clear();` and `write_window_.Clear();` lines, add:

```cpp
    acquire_observed_ = false;
    acquire_window_.Clear();
    vpblt_observed_ = false;
    vpblt_window_.Clear();
    mux_process_observed_ = false;
    mux_window_.Clear();
```

In `BuildSnapshot`, in the Capture block (after `cap.interval_observed` is set):

```cpp
    if (acquire_observed_) {
        const RollingTimeWindow::Aggregate a = acquire_window_.Compute(now);
        cap.acquire_latest_ms = a.latest;
        cap.acquire_average_ms = a.average;
        cap.acquire_peak_ms = a.peak;
        cap.acquire_availability = (a.count > 0) ? MetricAvailability::Available : MetricAvailability::Unavailable;
    }
```

In the Compositor block (after `s.compositor.frames_composed = frames_composed_;`):

```cpp
    if (vpblt_observed_) {
        const RollingTimeWindow::Aggregate vp = vpblt_window_.Compute(now);
        s.compositor.vpblt_latest_ms = vp.latest;
        s.compositor.vpblt_average_ms = vp.average;
        s.compositor.vpblt_peak_ms = vp.peak;
        s.compositor.vpblt_availability = (vp.count > 0) ? MetricAvailability::Available : MetricAvailability::Unavailable;
    }
```

In the Mux/Disk block (after `mux.availability = ...;`):

```cpp
    if (mux_process_observed_) {
        const RollingTimeWindow::Aggregate mp = mux_window_.Compute(now);
        mux.process_latest_ms = mp.latest;
        mux.process_average_ms = mp.average;
        mux.process_peak_ms = mp.peak;
        mux.process_availability = (mp.count > 0) ? MetricAvailability::Available : MetricAvailability::Unavailable;
    }
```

- [ ] **Step 6: Build and run the tests to verify they pass**

Run:
```bash
cmake --build build/windows-x64-debug --target test_pipeline_diagnostics
ctest --test-dir build/windows-x64-debug -R "test_pipeline_diagnostics|CaptureCardWiring" --output-on-failure
```
Expected: PASS — the 5 new `CaptureCardWiring.*` cases green, all prior cases still green.

- [ ] **Step 7: Commit**

```bash
git add libs/recorder_core/include/recorder_core/pipeline_diagnostics.h \
        libs/recorder_core/src/pipeline_diagnostics_aggregator.h \
        libs/recorder_core/src/pipeline_diagnostics_aggregator.cpp \
        libs/recorder_core/tests/test_pipeline_diagnostics.cpp
git commit -m "feat(diagnostics): acquire/vpblt/mux-process CPU-timing windows in aggregator (TDD)"
```

---

## Task 3: Engine instrumentation wiring (compile + dev-sanity)

**Files:**
- Modify: `libs/recorder_core/src/video_thread.cpp` (Acquire drains; `VideoProcessorBlt` CFR + VFR)
- Modify: `libs/recorder_core/src/mux_thread.cpp` (mux drain loop)

**Interfaces:**
- Consumes: `m_state.diagnostics.OnAcquireLatency / OnVpbltSubmit / OnMuxLatency` (Task 2).
- Produces: populated snapshot durations during a real recording (not headless-verifiable).

No GPU queries. Brackets use `std::chrono::steady_clock::now()`, mirroring the existing compositor/encode taps. Verification for this task is **compile + link of the full app** plus an optional real-recording sanity check; there is no unit assertion for hardware-driven values.

- [ ] **Step 1: Bracket the DXGI-OD acquire drain (Source Capture)**

In `libs/recorder_core/src/video_thread.cpp`, the DXGI-OD branch begins at the `if (useOdCapture) {` around line 1511 with `while (true) {` at ~1514. Wrap the whole OD drain loop. Add a timestamp just before `while (true) {`:

```cpp
            if (useOdCapture) {
                // Capture-card live wiring: cheap CPU bracket around the acquire/drain.
                const auto acq_t0 = std::chrono::steady_clock::now();
                // DXGI OD: drain all available frames. Newest-at-tick copies into
                // odCapturedTex; phase-correct copies into the present-QPC ring.
                while (true) {
```

Then immediately after the OD `while (true)` loop closes (the `}` that ends the inner drain, currently at line ~1586, just before the `} else {` WGC branch), record the bracket. Only feed it when actually recording (mirror `diag_recording`):

```cpp
                } // end while(true) OD drain
                if (!m_state.pause_requested.load()) {
                    const auto acq_t1 = std::chrono::steady_clock::now();
                    m_state.diagnostics.OnAcquireLatency(
                        acq_t1, std::chrono::duration<double, std::milli>(acq_t1 - acq_t0).count());
                }
            } else {
```

- [ ] **Step 2: Bracket the WGC acquire drain (Source Capture)**

In the WGC branch (`} else {` at ~1587, `try { while (true) {` at ~1589). Add a timestamp before the `try`:

```cpp
            } else {
                // WGC: drain frame pool — keep latest (always drain, even when paused)
                const auto acq_t0 = std::chrono::steady_clock::now();
                try {
                    while (true) {
```

And after the `catch (...) {}` that closes the WGC drain (currently at ~1621-1622):

```cpp
                } catch (...) {
                }
                if (!m_state.pause_requested.load()) {
                    const auto acq_t1 = std::chrono::steady_clock::now();
                    m_state.diagnostics.OnAcquireLatency(
                        acq_t1, std::chrono::duration<double, std::milli>(acq_t1 - acq_t0).count());
                }
            }
```

- [ ] **Step 3: Bracket `VideoProcessorBlt` (CFR path) → Compositor**

In the CFR encode path, the `VideoProcessorBlt` call is at ~1766. Bracket only that call:

```cpp
                    if (SUCCEEDED(hr) && inputView != nullptr) {
                        D3D11_VIDEO_PROCESSOR_STREAM stream{};
                        stream.Enable = TRUE;
                        stream.pInputSurface = inputView.get();

                        const auto vp_t0 = std::chrono::steady_clock::now();
                        hr = videoContext->VideoProcessorBlt(videoProcessor.get(), videoOutputViews[slot].get(), 0, 1,
                                                             &stream);
                        const auto vp_t1 = std::chrono::steady_clock::now();
                        m_state.diagnostics.OnVpbltSubmit(
                            vp_t1, std::chrono::duration<double, std::milli>(vp_t1 - vp_t0).count());
                        inputView = nullptr;
```

- [ ] **Step 4: Bracket `VideoProcessorBlt` (VFR path) → Compositor**

The VFR path has its own `VideoProcessorBlt` near line ~2030 (the second `compositeFrameGpu`/`VideoProcessorBlt` block, mirror of the CFR one, guarded by the VFR branch). Apply the identical bracket around that `VideoProcessorBlt(...)` call. Find it via:

```bash
grep -n "VideoProcessorBlt" libs/recorder_core/src/video_thread.cpp
```
Expect two call sites (CFR + VFR). Wrap the second the same way as Step 3 (vp_t0 / vp_t1 / `OnVpbltSubmit`). Do not alter any other VFR logic.

- [ ] **Step 5: Bracket the mux drain loop (Muxer)**

In `libs/recorder_core/src/mux_thread.cpp`, the streaming drain loop (lines 525-532) dequeues and `std::visit`s each `MuxItem`. Bracket the whole inner drain and feed `OnMuxLatency` once per non-empty batch:

```cpp
        const uint32_t mux_queue_depth = static_cast<uint32_t>(m_state.mux_queue.size());
        if (m_state.HasFailure())
            break;

        const auto mux_t0 = std::chrono::steady_clock::now();
        bool processed_any = false;
        while (!m_state.mux_queue.empty()) {
            MuxItem item = std::move(m_state.mux_queue.front());
            m_state.mux_queue.pop_front();
            lk.unlock();
            std::visit([&](auto&& payload) { handle_payload(std::move(payload), videoEos, audioEosReceived); },
                       item.payload);
            lk.lock();
            processed_any = true;
        }
        lk.unlock();
        if (processed_any) {
            const auto mux_t1 = std::chrono::steady_clock::now();
            m_state.diagnostics.OnMuxLatency(mux_t1,
                                             std::chrono::duration<double, std::milli>(mux_t1 - mux_t0).count());
        }
        m_state.diagnostics.OnVideoQueueDepth(mux_queue_depth);
        sample_mux_diagnostics();
```

(`<chrono>` is already transitively available; if the build complains, add `#include <chrono>` at the top of `mux_thread.cpp`.)

- [ ] **Step 6: Full build to verify compile + link**

Run (full build — recorder_core feeds many test targets):
```bash
cmake --build build/windows-x64-debug
```
Expected: build succeeds, no errors. The diagnostics aggregator now receives acquire/vpblt/mux samples from the live engine. (Runtime population is verified optionally in Task 6 via a real recording; it is not headless-verifiable.)

- [ ] **Step 7: Commit**

```bash
git add libs/recorder_core/src/video_thread.cpp libs/recorder_core/src/mux_thread.cpp
git commit -m "feat(diagnostics): cheap CPU brackets for acquire/vpblt/mux feeding live cards"
```

---

## Task 4: PipelineStepCard / PipelineFlow card face (widget TDD)

**Files:**
- Modify: `app/ui/widgets/PipelineStepCard.h`
- Modify: `app/ui/widgets/PipelineStepCard.cpp`
- Modify: `app/ui/widgets/PipelineFlow.h`
- Modify: `app/ui/widgets/PipelineFlow.cpp`
- Test: `app/tests/test_pipeline_flow.cpp` (existing target `pipeline_flow_tests`)

**Interfaces:**
- Produces (used by Task 5):
  - `PipelineStepCard::setResourceTag(const QString&)`, `QString resourceTag() const`
  - `PipelineStepCard::setSecondaryNumber(const QString&)`, `QString secondaryNumber() const`
  - `PipelineFlow::setStepLive(int index, PipelineStepCard::Status status, const QString& note, const QString& resourceTag, const QString& secondaryNumber, const QString& tooltip)`
  - Resource label objectName `"pipelineStepResource"`, number label objectName `"pipelineStepNumber"`.

- [ ] **Step 1: Write the failing widget test**

Append to `app/tests/test_pipeline_flow.cpp` (before the final `} // namespace`):

```cpp
TEST_F(PipelineFlowTest, SetStepLivePopulatesResourceNumberAndTooltip) {
    PipelineFlow flow;
    flow.setStepLive(3, PipelineStepCard::Status::Over, QStringLiteral("Encoder can't keep up with 60 fps"),
                     QStringLiteral("GPU (NVENC)"), QStringLiteral("9.2 ms"),
                     QStringLiteral("peak 14.0 ms · CPU submit (GPU execution time not measured)"));
    auto* card = flow.card(3);
    ASSERT_NE(card, nullptr);
    EXPECT_EQ(card->status(), PipelineStepCard::Status::Over);
    EXPECT_EQ(card->statusText(), QStringLiteral("Over"));
    EXPECT_EQ(card->resourceTag(), QStringLiteral("GPU (NVENC)"));
    EXPECT_EQ(card->secondaryNumber(), QStringLiteral("9.2 ms"));
    EXPECT_TRUE(card->toolTip().contains(QStringLiteral("peak 14.0 ms")));

    auto* resource = card->findChild<QLabel*>(QStringLiteral("pipelineStepResource"));
    auto* number = card->findChild<QLabel*>(QStringLiteral("pipelineStepNumber"));
    ASSERT_NE(resource, nullptr);
    ASSERT_NE(number, nullptr);
    EXPECT_EQ(resource->text(), QStringLiteral("GPU (NVENC)"));
    EXPECT_EQ(number->text(), QStringLiteral("9.2 ms"));
}

TEST_F(PipelineFlowTest, SetStepLiveDashWhenNumberUnavailable) {
    PipelineFlow flow;
    const QString dash = QString::fromUtf8("\xE2\x80\x94");
    flow.setStepLive(2, PipelineStepCard::Status::Ok, QStringLiteral("Compositor idle (no overlay)"),
                     QStringLiteral("GPU"), dash, QString());
    auto* card = flow.card(2);
    ASSERT_NE(card, nullptr);
    EXPECT_EQ(card->secondaryNumber(), dash);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run:
```bash
cmake --build build/windows-x64-debug --target pipeline_flow_tests
```
Expected: FAIL — `setStepLive` / `resourceTag` / `secondaryNumber` not declared.

- [ ] **Step 3: Extend the PipelineStepCard header**

In `app/ui/widgets/PipelineStepCard.h`, add to the public API (after `setNote`/`note`):

```cpp
    void setResourceTag(const QString& tag);
    QString resourceTag() const;

    void setSecondaryNumber(const QString& number);
    QString secondaryNumber() const;
```

And add the private member labels (after `note_label_`):

```cpp
    QLabel* resource_label_ = nullptr;
    QLabel* number_label_ = nullptr;
```

- [ ] **Step 4: Implement the PipelineStepCard additions**

In `app/ui/widgets/PipelineStepCard.cpp`, build the two labels in the constructor. Insert into the pill row so resource tag + number sit beside the status pill. Replace the existing `pill_row` block:

```cpp
    status_label_ = new QLabel(StatusLabel(status_), this);
    status_label_->setObjectName(QStringLiteral("pipelineStepStatus"));
    status_label_->setProperty("labelRole", "pipelineStepStatus");
    status_label_->setProperty("pipelineStatus", QString::fromLatin1(StatusKey(status_)));
    status_label_->setAlignment(Qt::AlignCenter);
    status_label_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);

    resource_label_ = new QLabel(this);
    resource_label_->setObjectName(QStringLiteral("pipelineStepResource"));
    resource_label_->setProperty("labelRole", "pipelineStepResource");

    number_label_ = new QLabel(this);
    number_label_->setObjectName(QStringLiteral("pipelineStepNumber"));
    number_label_->setProperty("labelRole", "pipelineStepNumber");
    number_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    auto* pill_row = new QHBoxLayout();
    pill_row->setContentsMargins(0, 0, 0, 0);
    pill_row->setSpacing(6);
    pill_row->addWidget(status_label_, 0, Qt::AlignLeft);
    pill_row->addWidget(resource_label_, 0, Qt::AlignLeft);
    pill_row->addStretch(1);
    pill_row->addWidget(number_label_, 0, Qt::AlignRight);
    root->addLayout(pill_row);
```

Add the accessor implementations (after `note()`):

```cpp
void PipelineStepCard::setResourceTag(const QString& tag) {
    if (resource_label_)
        resource_label_->setText(tag);
}

QString PipelineStepCard::resourceTag() const {
    return resource_label_ ? resource_label_->text() : QString{};
}

void PipelineStepCard::setSecondaryNumber(const QString& number) {
    if (number_label_)
        number_label_->setText(number);
}

QString PipelineStepCard::secondaryNumber() const {
    return number_label_ ? number_label_->text() : QString{};
}
```

- [ ] **Step 5: Add `setStepLive` to PipelineFlow**

In `app/ui/widgets/PipelineFlow.h`, declare (after `setStepStatus`):

```cpp
    // Live card face: status + note + resource tag + one secondary number + tooltip.
    void setStepLive(int index, PipelineStepCard::Status status, const QString& note, const QString& resourceTag,
                     const QString& secondaryNumber, const QString& tooltip);
```

In `app/ui/widgets/PipelineFlow.cpp`, implement (after `setStepStatus`):

```cpp
void PipelineFlow::setStepLive(int index, PipelineStepCard::Status status, const QString& note,
                               const QString& resourceTag, const QString& secondaryNumber, const QString& tooltip) {
    if (auto* step = card(index)) {
        step->setStatus(status);
        step->setNote(note);
        step->setResourceTag(resourceTag);
        step->setSecondaryNumber(secondaryNumber);
        step->setToolTip(tooltip);
    }
}
```

- [ ] **Step 6: Build and run the tests to verify they pass**

Run:
```bash
cmake --build build/windows-x64-debug --target pipeline_flow_tests
ctest --test-dir build/windows-x64-debug -R "diagnostic\.PipelineFlow|pipeline_flow" --output-on-failure
```
Expected: PASS — new `SetStepLive*` cases green, and the existing `CardsCarryNoFakeLiveMetrics` still green (it never calls `setStepLive`).

- [ ] **Step 7: Commit**

```bash
git add app/ui/widgets/PipelineStepCard.h app/ui/widgets/PipelineStepCard.cpp \
        app/ui/widgets/PipelineFlow.h app/ui/widgets/PipelineFlow.cpp \
        app/tests/test_pipeline_flow.cpp
git commit -m "feat(diagnostics): PipelineStepCard live face (resource tag + secondary number + tooltip)"
```

---

## Task 5: DiagnosticsPage live wiring (UI TDD)

**Files:**
- Modify: `app/pages/DiagnosticsPage.h`
- Modify: `app/pages/DiagnosticsPage.cpp`
- Modify: `app/MainWindow.cpp` (extend `makeLiveDiagnosticsSnapshot` synthetic fields for render-verify)
- Test: `app/tests/test_diagnostics_page.cpp` (existing target `diagnostics_page_tests`)

**Interfaces:**
- Consumes: `ResolvePipelineHealth` + `StageSignals`/`StageId`/`StageHealth` (Task 1); new snapshot fields (Task 2); `PipelineFlow::setStepLive` (Task 4).
- Produces: live capture cards reacting to `applyLiveDiagnostics`.

Card mapping (status from the resolver verdict; numbers from the snapshot; "—" when the relevant `MetricAvailability` is Unavailable):

| Card | StageId | Resource tag | Secondary number | Number availability |
|------|---------|--------------|------------------|---------------------|
| 0 Source Capture | SourceCapture | `CPU` | `actual.fps / target.fps` (e.g. `59.8 / 60.0 fps`) | `capture.target_fps > 0` |
| 1 Frame Queue | FrameQueue | `—` | depth `cur` (or `cur / cap` if bounded) | `video_queue` always present |
| 2 Compositor | Compositor | `GPU` | `compositor.average_ms` ms | `compositor.active` |
| 3 Encoder | Encoder | `GPU (NVENC)` | `video_encoder.average_ms` ms | `video_encoder.average_ms > 0` |
| 4 Muxer | Muxer | `CPU` | `mux.process_average_ms` ms | `mux.process_availability == Available` |
| 5 Disk | Disk | `CPU` | `disk.average_write_ms` ms | `disk.latency_availability == Available` |

- [ ] **Step 1: Write the failing UI tests**

Append to `app/tests/test_diagnostics_page.cpp` (before the final `} // namespace`; reuse the existing `MakeRecordingSnapshot`, `LiveValue`, `kDash`):

```cpp
TEST_F(DiagnosticsPageTest, CaptureCardsLiveDuringRecording) {
    DiagnosticsPage page;
    LoadData(page);
    auto s = MakeRecordingSnapshot();
    s.video_encoder.average_ms = 2.1;
    s.mux.process_average_ms = 0.5;
    s.mux.process_availability = recorder_core::MetricAvailability::Available;
    s.disk.average_write_ms = 0.8;
    page.applyLiveDiagnostics(s);

    auto* flow = page.findChild<PipelineFlow*>(QStringLiteral("pipelineFlow"));
    ASSERT_NE(flow, nullptr);
    // Healthy recording → no card alarmed (no Over).
    for (int i = 0; i < flow->stepCount(); ++i)
        EXPECT_NE(flow->card(i)->status(), PipelineStepCard::Status::Over) << i;
    // Capture card shows the fps number; Encoder shows ms.
    EXPECT_TRUE(flow->card(0)->secondaryNumber().contains(QStringLiteral("fps")));
    EXPECT_TRUE(flow->card(3)->secondaryNumber().contains(QStringLiteral("ms")));
    EXPECT_EQ(flow->card(3)->resourceTag(), QStringLiteral("GPU (NVENC)"));
}

TEST_F(DiagnosticsPageTest, CaptureCardBottleneckShownAsOver) {
    DiagnosticsPage page;
    LoadData(page);
    auto s = MakeRecordingSnapshot();
    s.video_encoder.average_ms = 22.0; // way over the 16.7 ms budget
    s.video_encoder.backlog = 6;
    page.applyLiveDiagnostics(s);

    auto* flow = page.findChild<PipelineFlow*>(QStringLiteral("pipelineFlow"));
    ASSERT_NE(flow, nullptr);
    EXPECT_EQ(flow->card(3)->status(), PipelineStepCard::Status::Over);
}

TEST_F(DiagnosticsPageTest, MuxNumberDashWhenUnavailable) {
    DiagnosticsPage page;
    LoadData(page);
    auto s = MakeRecordingSnapshot();
    s.mux.process_availability = recorder_core::MetricAvailability::Unavailable;
    page.applyLiveDiagnostics(s);

    auto* flow = page.findChild<PipelineFlow*>(QStringLiteral("pipelineFlow"));
    ASSERT_NE(flow, nullptr);
    EXPECT_EQ(flow->card(4)->secondaryNumber(), kDash);
}

TEST_F(DiagnosticsPageTest, IdleAfterRecordingRestoresStaticCards) {
    DiagnosticsPage page;
    LoadData(page);
    page.applyLiveDiagnostics(MakeRecordingSnapshot());

    recorder_core::RecordingDiagnosticsSnapshot idle;
    idle.lifecycle = recorder_core::DiagnosticsLifecycle::Idle;
    idle.valid = false;
    page.applyLiveDiagnostics(idle);

    auto* flow = page.findChild<PipelineFlow*>(QStringLiteral("pipelineFlow"));
    ASSERT_NE(flow, nullptr);
    // Idle restores static readiness: probe-less cards Planned, probed cards Ok.
    EXPECT_EQ(flow->card(0)->status(), PipelineStepCard::Status::Planned);
    EXPECT_EQ(flow->card(3)->status(), PipelineStepCard::Status::Ok);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run:
```bash
cmake --build build/windows-x64-debug --target diagnostics_page_tests
```
Expected: FAIL — cards are not yet wired live (capture card still Planned during recording; no secondary numbers).

- [ ] **Step 3: Add the header includes + members**

In `app/pages/DiagnosticsPage.h`, add the include near the other recorder_core include:

```cpp
#include <recorder_core/pipeline_health.h>
```

Add a private method declaration (after `refreshPipeline();`):

```cpp
    void updatePipelineCards(const recorder_core::RecordingDiagnosticsSnapshot& snapshot);
```

Add members (after `last_live_snapshot_`):

```cpp
    // Capture-card live wiring (0.8.0): 2 Hz apply throttle + drop-delta tracking.
    std::chrono::steady_clock::time_point last_cards_applied_{};
    uint64_t cards_last_generation_ = 0;
    uint64_t cards_last_problem_drops_ = 0;
```

Add `#include <chrono>` to the header if not already present (it includes `<cstdint>`/`<filesystem>`; add `<chrono>`).

- [ ] **Step 4: Implement `updatePipelineCards` and call it**

In `app/pages/DiagnosticsPage.cpp`, at the end of `applyLiveDiagnostics` (after `live_pipeline_panel_->applySnapshot(...)`), add the call (it must run even if the panel is null, so place it before the early `return` — restructure to always call it):

```cpp
    updatePipelineCards(last_live_snapshot_);

    if (live_pipeline_panel_ == nullptr) {
        return;
    }
    live_pipeline_panel_->applySnapshot(last_live_snapshot_);
}
```

(Move `updatePipelineCards(last_live_snapshot_);` to just before the `if (live_pipeline_panel_ == nullptr)` guard so it always runs.)

Add the implementation (after `applyLiveDiagnostics`, before `// --- Helpers ---`):

```cpp
void DiagnosticsPage::updatePipelineCards(const recorder_core::RecordingDiagnosticsSnapshot& s) {
    using recorder_core::MetricAvailability;
    using recorder_core::StageHealth;
    using recorder_core::StageId;
    using recorder_core::StageSignals;
    using Status = ui::widgets::PipelineStepCard::Status;

    if (!pipeline_flow_)
        return;

    const bool recording = s.valid && (s.lifecycle == recorder_core::DiagnosticsLifecycle::Recording ||
                                       s.lifecycle == recorder_core::DiagnosticsLifecycle::Paused);
    if (!recording) {
        // Idle / stopping / completed → restore the static readiness cards.
        refreshPipeline();
        last_cards_applied_ = {};
        return;
    }

    // 2 Hz throttle (0.5 s), independent of the LIVE PIPELINE panel cadence.
    const auto now = std::chrono::steady_clock::now();
    if (last_cards_applied_ != std::chrono::steady_clock::time_point{} &&
        (now - last_cards_applied_) < std::chrono::milliseconds(500)) {
        return;
    }
    last_cards_applied_ = now;

    const QString dash = QString::fromUtf8("\xE2\x80\x94");
    const double budget_ms = (s.capture.target_fps > 0.0) ? 1000.0 / s.capture.target_fps : (1000.0 / 60.0);

    // Drop delta over the publish interval (problem drops only; coalesce is benign).
    if (s.session_generation != cards_last_generation_) {
        cards_last_generation_ = s.session_generation;
        cards_last_problem_drops_ = 0;
    }
    const uint64_t problem_drops = s.capture.frames_dropped_cfr + s.capture.frames_dropped_backpressure;
    const uint32_t capture_recent_drops =
        (problem_drops > cards_last_problem_drops_) ? static_cast<uint32_t>(problem_drops - cards_last_problem_drops_)
                                                    : 0;
    cards_last_problem_drops_ = problem_drops;

    // ---- Build per-stage signals (distilled from the snapshot) ----
    constexpr uint32_t kQueueBusyDepth = 8; // mirrors DiagnosticsThresholds::mux_queue_warn
    constexpr double kDiskBudgetMs = 8.0;   // mirrors DiagnosticsThresholds::disk_write_ms_warn

    StageSignals capture{};
    capture.id = StageId::SourceCapture;
    capture.available = s.capture.target_fps > 0.0 && s.capture.actual_fps > 0.0;
    capture.is_duration_stage = false;
    capture.can_bottleneck = true;
    capture.fps_ratio = (s.capture.target_fps > 0.0) ? s.capture.actual_fps / s.capture.target_fps : 1.0;
    capture.recent_drops = capture_recent_drops;

    StageSignals queue{};
    queue.id = StageId::FrameQueue;
    queue.available = true;
    queue.is_duration_stage = false;
    queue.can_bottleneck = false;
    queue.queue_depth = s.video_queue.current_depth;
    queue.queue_busy_threshold = kQueueBusyDepth;

    StageSignals comp{};
    comp.id = StageId::Compositor;
    comp.available = s.compositor.active;
    comp.is_duration_stage = true;
    comp.can_bottleneck = true;
    comp.avg_ms = s.compositor.average_ms;

    StageSignals enc{};
    enc.id = StageId::Encoder;
    enc.available = s.video_encoder.average_ms > 0.0 || s.video_encoder.frames_encoded > 0;
    enc.is_duration_stage = true;
    enc.can_bottleneck = true;
    enc.avg_ms = s.video_encoder.average_ms;
    enc.recent_drops = (s.video_encoder.backlog >= 2) ? 1u : 0u; // NEED_MORE_INPUT shedding proxy

    StageSignals mux{};
    mux.id = StageId::Muxer;
    mux.available = s.mux.process_availability == MetricAvailability::Available;
    mux.is_duration_stage = true;
    mux.can_bottleneck = true;
    mux.avg_ms = s.mux.process_average_ms;

    StageSignals disk{};
    disk.id = StageId::Disk;
    disk.available = s.disk.latency_availability == MetricAvailability::Available;
    disk.is_duration_stage = true;
    disk.can_bottleneck = true;
    disk.avg_ms = s.disk.average_write_ms;
    disk.budget_ms = kDiskBudgetMs;

    const StageSignals stages[] = {capture, queue, comp, enc, mux, disk};
    const recorder_core::PipelineHealthVerdict verdict = recorder_core::ResolvePipelineHealth(stages, budget_ms);

    auto to_status = [](StageHealth h) -> Status {
        switch (h) {
        case StageHealth::Healthy:
            return Status::Ok;
        case StageHealth::Busy:
            return Status::Hotspot;
        case StageHealth::Bottleneck:
            return Status::Over;
        }
        return Status::Ok;
    };
    auto health_of = [&](StageId id) -> StageHealth {
        for (const auto& sv : verdict.per_stage)
            if (sv.id == id)
                return sv.health;
        return StageHealth::Healthy;
    };

    auto ms = [&](double v, bool avail) { return avail ? QString::number(v, 'f', 1) + QStringLiteral(" ms") : dash; };

    // ---- Card 0: Source Capture ----
    const bool cap_num = s.capture.target_fps > 0.0;
    const QString cap_number =
        cap_num ? QString::number(s.capture.actual_fps, 'f', 1) + QStringLiteral(" / ") +
                      QString::number(s.capture.target_fps, 'f', 1) + QStringLiteral(" fps")
                : dash;
    const QString cap_tip =
        (s.capture.acquire_availability == MetricAvailability::Available)
            ? QStringLiteral("Acquire ") + QString::number(s.capture.acquire_average_ms, 'f', 2) +
                  QStringLiteral(" ms (CPU)")
            : QStringLiteral("Acquire timing unavailable for this capture mode");
    pipeline_flow_->setStepLive(0, to_status(health_of(StageId::SourceCapture)), QString(), QStringLiteral("CPU"),
                                cap_number, cap_tip);

    // ---- Card 1: Frame Queue ----
    const QString q_number = s.video_queue.bounded && s.video_queue.capacity > 0
                                 ? QString::number(s.video_queue.current_depth) + QStringLiteral(" / ") +
                                       QString::number(s.video_queue.capacity)
                                 : QString::number(s.video_queue.current_depth);
    pipeline_flow_->setStepLive(1, to_status(health_of(StageId::FrameQueue)), QString(), dash, q_number,
                                QStringLiteral("Frames waiting between encode and mux (peak ") +
                                    QString::number(s.video_queue.peak_depth) + QStringLiteral(")"));

    // ---- Card 2: Compositor ----
    const QString comp_tip =
        QStringLiteral("CPU submit (GPU execution time not measured in this view). VPBlt ") +
        ((s.compositor.vpblt_availability == MetricAvailability::Available)
             ? QString::number(s.compositor.vpblt_average_ms, 'f', 2) + QStringLiteral(" ms")
             : dash);
    pipeline_flow_->setStepLive(2, to_status(health_of(StageId::Compositor)), QString(), QStringLiteral("GPU"),
                                ms(s.compositor.average_ms, s.compositor.active), comp_tip);

    // ---- Card 3: Encoder ----
    pipeline_flow_->setStepLive(3, to_status(health_of(StageId::Encoder)), QString(), QStringLiteral("GPU (NVENC)"),
                                ms(s.video_encoder.average_ms, enc.available),
                                QStringLiteral("CPU submit→ready latency (peak ") +
                                    QString::number(s.video_encoder.peak_ms, 'f', 1) + QStringLiteral(" ms)"));

    // ---- Card 4: Muxer ----
    pipeline_flow_->setStepLive(4, to_status(health_of(StageId::Muxer)), QString(), QStringLiteral("CPU"),
                                ms(s.mux.process_average_ms, mux.available),
                                QStringLiteral("Mux drain processing (peak ") +
                                    QString::number(s.mux.process_peak_ms, 'f', 2) + QStringLiteral(" ms)"));

    // ---- Card 5: Disk ----
    pipeline_flow_->setStepLive(5, to_status(health_of(StageId::Disk)), QString(), QStringLiteral("CPU"),
                                ms(s.disk.average_write_ms, disk.available),
                                QStringLiteral("Filesystem write-call latency (peak ") +
                                    QString::number(s.disk.peak_write_ms, 'f', 1) + QStringLiteral(" ms)"));
}
```

- [ ] **Step 4b: Drop the now-false "not instrumented yet" idle notes**

The idle path calls `refreshPipeline()`, which currently sets `Status::Planned` notes on cards 0–2
reading "Capture frame timing is not instrumented yet.", "Frame-queue depth is not instrumented
yet.", and "Compositor timing is not instrumented yet." (`DiagnosticsPage.cpp:~1071-1073`). Those are
now false — the cards ARE instrumented during recording. Replace the three note strings with the
neutral "Live during recording." (keep the `Status::Planned` status unchanged, so the
`IdleAfterRecordingRestoresStaticCards` test — which checks status, not note — still passes):

```cpp
    pipeline_flow_->setStepStatus(0, PipelineStepCard::Status::Planned, QStringLiteral("Live during recording."));
    pipeline_flow_->setStepStatus(1, PipelineStepCard::Status::Planned, QStringLiteral("Live during recording."));
    pipeline_flow_->setStepStatus(2, PipelineStepCard::Status::Planned, QStringLiteral("Live during recording."));
```

(Match the exact `setStepStatus`/note call shape already used in `refreshPipeline` for those three
cards; only the note text changes.)

- [ ] **Step 5: Build and run the UI tests to verify they pass**

Run:
```bash
cmake --build build/windows-x64-debug --target diagnostics_page_tests
ctest --test-dir build/windows-x64-debug -R "diagnostic\." --output-on-failure
```
Expected: PASS — the 4 new cases green; the existing live-panel cases (`liveCaptureFps` etc.) still green (the panel path is unchanged).

- [ ] **Step 6: Extend the synthetic snapshot for render-verify**

In `app/MainWindow.cpp`, in `makeLiveDiagnosticsSnapshot`, set the new fields on the base (healthy) snapshot so the existing diagnostics visual scenarios render real numbers (after the existing `s.disk.*` block, before `s.split.*`):

```cpp
    s.capture.acquire_average_ms = 0.6;
    s.capture.acquire_peak_ms = 1.2;
    s.capture.acquire_availability = MetricAvailability::Available;
    s.compositor.vpblt_average_ms = 0.4;
    s.compositor.vpblt_peak_ms = 0.9;
    s.compositor.vpblt_availability = MetricAvailability::Available;
    s.mux.process_average_ms = 0.5;
    s.mux.process_peak_ms = 1.1;
    s.mux.process_availability = MetricAvailability::Available;
```

In the `encoder` branch, bump the encoder duration over budget so the Encoder card reads Over:

```cpp
    } else if (kind == QStringLiteral("encoder")) {
        s.video_encoder.average_ms = 20.0; // over the 16.7 ms budget → Bottleneck
        s.video_encoder.peak_ms = 24.0;
        s.video_encoder.backlog = 6;
        s.video_encoder.frames_encoded = 2440;
        s.video_queue.current_depth = 5;
        ...
```

In the `disk` branch, the existing `s.disk.average_write_ms = 14.0` already exceeds the 8 ms disk budget → Disk card reads Over. Leave as-is.

- [ ] **Step 7: Commit**

```bash
git add app/pages/DiagnosticsPage.h app/pages/DiagnosticsPage.cpp \
        app/MainWindow.cpp app/tests/test_diagnostics_page.cpp
git commit -m "feat(diagnostics): live capture cards via ResolvePipelineHealth (2 Hz throttle, idle reset)"
```

---

## Task 6: Full verification + docs

**Files:**
- Modify: `docs/superpowers/specs/2026-06-27-capture-card-live-wiring-design.md` (mark delivered)
- Modify: `docs/roadmap.md` (roadmap line)

- [ ] **Step 1: Full build (no `--target`)**

So every test target picks up the new translation units (recorder_core feeds many):
```bash
cmake --build build/windows-x64-debug
```
Expected: build succeeds with no errors/warnings-as-errors.

- [ ] **Step 2: Full test suite**

```bash
ctest --test-dir build/windows-x64-debug -j6 --output-on-failure
```
Expected: all green. If `HistoryStore` / `PresetStore` cases flake under `-j6` (known pre-existing `-j` flakiness), re-run them serially to confirm they pass:
```bash
ctest --test-dir build/windows-x64-debug -R "HistoryStore|PresetStore" --output-on-failure
```

- [ ] **Step 3: Render-verify the Capture cards (Qt on PATH)**

Render the existing diagnostics scenarios (they now exercise the live cards via the extended synthetic snapshot). With Qt on PATH:
```powershell
$env:PATH = "C:\Qt\6.9.0\msvc2022_64\bin;$env:PATH"
Start-Process -Wait -FilePath build/windows-x64-debug/app/exosnap.exe `
  -ArgumentList "--visual-test","diagnostics-recording-healthy","--visual-test-screenshot"
Start-Process -Wait -FilePath build/windows-x64-debug/app/exosnap.exe `
  -ArgumentList "--visual-test","diagnostics-recording-encoder-pressure","--visual-test-screenshot"
Start-Process -Wait -FilePath build/windows-x64-debug/app/exosnap.exe `
  -ArgumentList "--visual-test","diagnostics-recording-disk-pressure","--visual-test-screenshot"
Start-Process -Wait -FilePath build/windows-x64-debug/app/exosnap.exe `
  -ArgumentList "--visual-test","diagnostics-idle","--visual-test-screenshot"
```
Inspect the screenshots at full resolution (per "Visual verification" memory — judge rendered pixels, not QSS): healthy shows all six cards Ok/Hotspot with CPU/GPU tags + numbers; encoder/disk scenarios show exactly the bottleneck card as Over (warning color) with a plain-language note; idle shows the static readiness cards (no live numbers, no "not instrumented yet" notes are required — the static `refreshPipeline` notes remain). Confirm Frame Queue is never Over.

- [ ] **Step 4 (optional): Real-recording sanity check**

Per "Live recording verification OK" memory: do a short real recording to a temp dir, open Diagnostics, confirm the six cards populate with plausible numbers (acquire/compositor/encoder/mux/disk in low single-digit ms, capture fps near target). Discard the output file — never commit/push a recording.

- [ ] **Step 5: Mark the spec delivered**

In `docs/superpowers/specs/2026-06-27-capture-card-live-wiring-design.md`, change the `**Status:**` line to:
```markdown
**Status:** Delivered (0.8.0 Diagnostics wave). Implemented per docs/superpowers/plans/2026-06-27-capture-card-live-wiring.md.
```

- [ ] **Step 6: Add a roadmap line**

In `docs/roadmap.md`, under the 0.8.0 Diagnostics wave, add:
```markdown
- Capture-card live wiring (health-first v1): the six CAPTURE PIPELINE cards show live status (Healthy/Busy/Bottleneck) + CPU/GPU tag + one secondary number during recording, via a pure `ResolvePipelineHealth` resolver and cheap CPU timing brackets (no GPU timestamp queries).
```

No ADR is required — this is a UI/diagnostics feature, not an architecture decision. It sits under the existing ADR 0033 diagnostics engine; no ADR text change is needed (the resolver and CPU-timing windows are additive, consistent with ADR 0033's "measure honestly, never fabricate" stance).

- [ ] **Step 7: Commit**

```bash
git add docs/superpowers/specs/2026-06-27-capture-card-live-wiring-design.md docs/roadmap.md
git commit -m "docs: mark capture-card live wiring delivered (0.8.0 diagnostics)"
```

---

## Self-Review

**1. Spec coverage** (each spec section → task):
- Goal / health-first card model (status + resource tag + secondary number) → Tasks 1 (resolver), 4 (card face), 5 (wiring). ✓
- Card Model table (per-card signal/resource/number) → Task 5 mapping table + `updatePipelineCards`. ✓
- Frame Queue bound to real `mux_queue`, never Bottleneck, "—" if no depth → Task 1 (`can_bottleneck=false`, `FrameQueueNeverBottleneck` test) + Task 5 (reads existing `video_queue`). Finding: queue gauge already engine-side; no new gauge. ✓
- Measurement & data flow: reuse existing windows; new acquire/vpblt/mux CPU brackets; new `RollingTimeWindow`s; availability flags → Tasks 2 + 3. ✓
- CPU-observed numbers carry resource tag + tooltip ("CPU submit…") → Task 4 (tooltip plumbing) + Task 5 (Compositor/Encoder tooltips). ✓
- 2 Hz UI throttle via last-applied timestamp → Task 5 (`last_cards_applied_`, 500 ms guard). ✓
- Health & bottleneck pure resolver (`StageHealth`, `StageSignals`, `PipelineHealthVerdict`, `ResolvePipelineHealth`) → Task 1. ✓
- "Only real bottleneck" / most-downstream wins / Busy is calm → Task 1 (`MostDownstreamBottleneckWins`, `DurationNearBudgetIsBusy`). ✓
- States: Idle (static readiness, with the now-false "not instrumented yet" notes replaced by "Live during recording." per Task 5 Step 4b), Recording (live), Metric unavailable ("—", never crash/fabricate) → Task 5 (`IdleAfterRecording…`, `MuxNumberDashWhenUnavailable`). ✓
- Testing: pure resolver (all-healthy, over-budget, drop-driven, queue-never, unavailable, empty) → Task 1 (11 cases). Aggregator windows → Task 2 (5 cases). UI card reflects snapshot / idle↔live / "—" → Tasks 4 + 5. ✓
- Optional GPU-timing follow-up → explicitly deferred, no task. ✓

**2. Placeholder scan:** No "TBD"/"add error handling"/"similar to Task N". Every code step shows complete code with real signatures read from the tree. The VFR `VideoProcessorBlt` site (Task 3 Step 4) is given as "~2030" with an exact `grep` to locate it, because the surrounding VFR code mirrors the CFR block verbatim.

**3. Type consistency across tasks:** `StageId{SourceCapture,FrameQueue,Compositor,Encoder,Muxer,Disk}`, `StageHealth{Healthy,Busy,Bottleneck}`, `StageSignals` fields (`available`, `is_duration_stage`, `can_bottleneck`, `avg_ms`, `budget_ms`, `fps_ratio`, `recent_drops`, `queue_depth`, `queue_busy_threshold`), `StageVerdict{id,health}`, `PipelineHealthVerdict{per_stage,has_bottleneck,bottleneck}`, and `ResolvePipelineHealth(span<const StageSignals>, double)` are defined in Task 1 and used identically in Task 5. Snapshot fields (`acquire_*`, `vpblt_*`, `process_*` + `*_availability`) defined in Task 2 are read with the same names in Task 5. Card API (`setStepLive`, `resourceTag`, `secondaryNumber`, objectNames `pipelineStepResource`/`pipelineStepNumber`) defined in Task 4 and used in Tasks 4-5 tests + Task 5 wiring. No `emit`/`signals`/`slots` member names. ✓

**Coverage summary:** Every spec requirement maps to a concrete TDD task with real code. One distillation choice to note for review: the encoder "NEED_MORE_INPUT" bottleneck is modeled as a `recent_drops` shedding proxy via `backlog >= 2`, a defensible interpretation rather than a literal drop counter (there is no separate NEED_MORE_INPUT counter in the snapshot).
