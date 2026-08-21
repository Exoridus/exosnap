#pragma once

// Qt Quick preview instrumentation, read off the scene-graph render thread.
//
// Lives in its own header because both ExoPreviewItem (which produces it) and
// RecordPreviewAdapter (which exposes it) need the type, and ExoPreviewItem.h
// already includes RecordPreviewAdapter.h — the adapter cannot include the item
// back without a cycle.

#include <QtGlobal>

#include <cstdint>

namespace exosnap::quick {

struct PreviewMetricsSnapshot {
    quint64 render_frames = 0;
    quint64 consumed_frames = 0;
    quint64 mutex_misses = 0;
    // The rest of the consume funnel, so that within one measurement window
    //   render_frames = mutex_misses + acquire_abandoned + acquires
    //   acquires      = consumed_frames + conversion_failures
    // holds exactly. A branch that breaks the identity is a branch nobody counted.
    quint64 acquires = 0;
    quint64 acquire_abandoned = 0;
    quint64 conversion_failures = 0;
    double scene_fps = 0.0;
    double scene_frame_ms_p50 = 0.0;
    double scene_frame_ms_p95 = 0.0;
    double scene_frame_ms_p99 = 0.0;
    double scene_frame_ms_max = 0.0;
    double source_delivery_fps = 0.0;
    double source_interval_ms_p95 = 0.0;
    double source_interval_ms_p99 = 0.0;
    double submit_us_p50 = 0.0;
    double submit_us_p95 = 0.0;
    double submit_us_p99 = 0.0;
    uint32_t source_dxgi_format = 0;

    // Preview update scheduling (PreviewUpdateScheduler). Filled by
    // RecordPreviewAdapter, which owns the scheduler, not by the item.
    //
    // publish_signals   — per-frame edges the producers emitted.
    // coalesced_signals — of those, the ones that found a wake-up already in
    //                     flight and correctly added no second render.
    // wakeups           — wake-ups the GUI thread handled.
    // scene_update_requests — wake-ups that reached a live item as update().
    //
    // render_frames / scene_update_requests is the render amplification the
    // scheduling fix exists to collapse.
    quint64 publish_signals = 0;
    quint64 coalesced_signals = 0;
    quint64 wakeups = 0;
    quint64 scene_update_requests = 0;

    // The two producer-side distributions, taken on the same clock and the same
    // measurement window as everything above.
    //
    // publish_interval says how fast the source actually fed. Without it a long
    // arrival gap on the consumer side is unreadable: a quiet desktop and a
    // stalled preview produce exactly the same number, which is how a run over an
    // idle screen was once mistaken for a regression.
    //
    // debt_age says how long the preview OWED a frame — measured from the first
    // publish after the last successful consume. That is the quantity the eye
    // sees, and the one a last-value slot can otherwise hide.
    double publish_interval_ms_p50 = 0.0;
    double publish_interval_ms_p95 = 0.0;
    double publish_interval_ms_p99 = 0.0;
    double publish_interval_ms_max = 0.0;
    double debt_age_ms_p50 = 0.0;
    double debt_age_ms_p95 = 0.0;
    double debt_age_ms_p99 = 0.0;
    double debt_age_ms_max = 0.0;
    // Added alongside: a single visible stall can hide under p99.
    double source_interval_ms_max = 0.0;
};

} // namespace exosnap::quick
