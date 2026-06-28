#include "recorder_core/pipeline_health.h"

namespace recorder_core {

namespace {

// Pinned, defensible thresholds (see plan Task 1).
constexpr double kBusyRatio = 0.8;               // duration stage Busy at >= 0.8 * budget
constexpr double kCaptureBottleneckRatio = 0.85; // capture Bottleneck below 0.85 of target
constexpr double kCaptureBusyRatio = 0.95;       // capture Busy below 0.95 of target

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
