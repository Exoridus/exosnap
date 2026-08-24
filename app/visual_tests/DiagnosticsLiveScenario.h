#pragma once

// DiagnosticsLiveScenario.h -- deterministic live-pipeline snapshots for the
// visual harness.
//
// The Diagnostics live summary is built from a RecordingDiagnosticsSnapshot the
// engine publishes ~5 times a second during a real recording. That makes its
// interesting states -- an encoder under pressure, a disk that cannot keep up, a
// lost audio device -- exactly the ones a capture run cannot reach on purpose:
// they need a real recording that is really going wrong, on this machine, at the
// moment of the screenshot.
//
// These fixtures are that state, written down. They are seeded through the SAME
// applyLiveDiagnostics() path the recording engine uses, so the real
// BuildLiveTiles() policy runs on them; nothing here reaches past the policy to
// place a tile directly. A capture is therefore evidence about the product's own
// classification, not about a hand-drawn picture of it.
//
// Pure and harness-only. Nothing in a shipping code path calls it.

#include <exosnap/engine/pipeline_diagnostics.h>

#include <QString>

namespace exosnap::visual {

// Recognised kinds, matching VisualScenario::diag_live:
//   "idle"                 no live pipeline (valid == false)
//   "healthy"              Good, no bottleneck, present diagnostics AVAILABLE
//   "present-unavailable"  the same healthy pipeline with present diagnostics off
//   "encoder"              Warning, encoder latency approaching the frame budget
//   "disk"                 Warning, disk write pressure and a write failure
//   "judder"               Warning, capture-side present judder with tearing
//   "degraded"             Good pipeline, one audio source lost mid-recording
//   "paused"               a paused recording
//   "split"                a split pending mid-recording
//   "post"                 a completed recording's final snapshot
//
// An unrecognised kind returns an invalid snapshot, which renders as no live
// tiles at all -- the honest result for "no scenario".
[[nodiscard]] exosnap::engine::RecordingDiagnosticsSnapshot MakeDiagnosticsLiveSnapshot(const QString& kind);

} // namespace exosnap::visual
