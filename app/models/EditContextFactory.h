#pragma once

// Builds the EditContext that the Edit/Output/Save surface opens on (ADR 0022).
//
// This used to live in an anonymous namespace inside `pages/RecordPage.cpp`,
// which made it reachable only from the Widgets frontend. The mapping is pure
// data translation — a completed recording's post-flight facts rendered as the
// labels the edit surface shows — so it belongs on the shared app layer where
// both frontends can reach it. Nothing here knows about a widget or a QML item.
//
// The live-session extras (mkv master path, peak A/V drift, diagnostics
// snapshot) are deliberately NOT filled in here: a history row cannot carry
// them, and only the frontend that owns the running session knows whether the
// recording being opened is the current one. Callers layer them on — see
// MakeEditContextForCurrentSession().

#include "CompletedRecording.h"
#include "EditContext.h"

#include <recorder_core/pipeline_diagnostics.h>

namespace exosnap {

// Common fields every Edit entry point shares (post-stop result button, Recent
// menu action, notification-toast Edit action).
[[nodiscard]] EditContext MakeEditContext(const CompletedRecording& recording);

// The current session's recording, with the extras only a live session has.
// `mkv_master_path` overrides the best-effort fallback MakeEditContext sets:
// for an MP4 recording the edit master is a different file, and only the
// session knows which.
[[nodiscard]] EditContext
MakeEditContextForCurrentSession(const CompletedRecording& recording, const QString& mkv_master_path,
                                 double peak_av_drift_ms, bool av_drift_available,
                                 const recorder_core::RecordingDiagnosticsSnapshot& completed_snapshot);

// Minimal context for an output path that no longer resolves to a known
// recording (e.g. trimmed out of the bounded history). Every detail row renders
// as "–" rather than showing another recording's numbers.
[[nodiscard]] EditContext MakeMinimalEditContext(const QString& output_path);

} // namespace exosnap
