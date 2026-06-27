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
    bool available = false;            // false → no live signal this stage; classify Healthy (neutral)
    bool is_duration_stage = false;    // true → avg_ms is compared to a budget
    bool can_bottleneck = true;        // FrameQueue sets this false
    double avg_ms = 0.0;               // observed average (duration stages)
    double budget_ms = 0.0;            // per-stage budget override; <=0 → use frame_budget_ms
    double fps_ratio = 1.0;            // actual/target fps (capture); 1.0 = on target
    uint32_t recent_drops = 0;         // frames shed over the window (any shedding proxy)
    uint32_t queue_depth = 0;          // FrameQueue current depth
    uint32_t queue_busy_threshold = 0; // depth at/above which the queue reads Busy (0 → never Busy)
};

struct StageVerdict {
    StageId id = StageId::SourceCapture;
    StageHealth health = StageHealth::Healthy;
};

struct PipelineHealthVerdict {
    std::vector<StageVerdict> per_stage;         // one entry per input stage, input order
    bool has_bottleneck = false;                 // true iff any stage is Bottleneck
    StageId bottleneck = StageId::SourceCapture; // most-downstream bottleneck (valid iff has_bottleneck)
};

// Classify every stage and pick the single most-downstream bottleneck (if any).
// frame_budget_ms is 1000/target_fps; per-stage budget_ms overrides it when > 0.
[[nodiscard]] PipelineHealthVerdict ResolvePipelineHealth(std::span<const StageSignals> stages, double frame_budget_ms);

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
