#pragma once

// PipelineSnapshotJson.h -- the serialization boundary for the recording
// pipeline's live telemetry.
//
// The authoritative owner is recorder_core::RecordingDiagnosticsSnapshot, built
// on a worker thread by SessionStatsCollector out of the per-session
// PipelineDiagnosticsAggregator. This file adds NO measurement, NO smoothing and
// NO classification of its own: every number below already exists, already has a
// documented meaning, and is already consumed by the Diagnostics surface, the
// session report and the overlays. What was missing was only a transport-neutral
// way to read it.
//
// Three properties are load-bearing:
//
//  * The drop taxonomy is the engine's. `problemDrops` is
//    CaptureDiagnostics::frames_dropped_problem() verbatim -- the same predicate
//    the bottleneck classifier, the dropped-frames notification and the report
//    card use. A second definition here would let the protocol and the product
//    disagree about whether a session dropped frames.
//  * Compositor and mux timings are CPU submission time. They are named for what
//    they measure (`latestMs`, `vpBltLatestMs`, `processLatestMs`) and never
//    `gpuTimeMs`, because this pipeline takes no GPU timestamp at all.
//  * Disk write latency is write-call latency over buffered stdio. The payload
//    says so in `disk.latencySemantics` rather than leaving a reader to assume
//    physical-media latency.
//
// Pure and free of Qt GUI types: a snapshot in, a QJsonObject out. That is what
// makes the whole surface testable from fixtures without a recording.

#include <recorder_core/pipeline_diagnostics.h>

#include <QJsonObject>

namespace exosnap::observability {

// The full payload of `pipeline.snapshot`.
//
// `valid == false` (idle, or a failure before any data existed) still returns a
// well-formed object: `valid`, `lifecycle` and `health` are answered, and the
// measurement groups are omitted rather than filled with zeros that would read
// as a perfectly healthy idle pipeline.
[[nodiscard]] QJsonObject PipelineSnapshotToJson(const recorder_core::RecordingDiagnosticsSnapshot& snapshot);

} // namespace exosnap::observability
