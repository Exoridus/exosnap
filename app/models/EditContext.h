#pragma once

// Context handed to the Edit/Output/Save surface when it opens (ADR 0022).
// Framework-independent: it names files and carries the completed recording's
// post-flight numbers, and knows nothing about how either frontend renders it.
//
// Lifted verbatim out of `app/pages/EditExportPage.h` so the Qt Quick editor
// consumes the same shape the Widgets page does. The Widgets page keeps its own
// copy for now; the two live in separate executables, so nothing ever sees both
// definitions in one translation unit.

#include <QString>

#include <cstdint>
#include <vector>

#include "RecordingMarker.h"
#include <exosnap/engine/pipeline_diagnostics.h>

namespace exosnap {

struct EditContext {
    // File metadata (from the completed recording result)
    QString output_path;     // final output (MP4 or MKV)
    QString mkv_master_path; // edit master (MKV); same as output for MKV recordings
    QString duration;        // human-readable duration (e.g. "1:23")
    QString size;            // human-readable file size (e.g. "142 MB")
    QString resolution;      // e.g. "1920x1080"
    QString fps;             // e.g. "60 fps CFR"
    QString video_codec;     // e.g. "AV1 (NVENC)"
    QString audio_codec;     // e.g. "Opus"
    QString container;       // e.g. "MKV" or "MP4"

    // Post-flight data (from RecordPage diagnostics tracking)
    double peak_av_drift_ms = 0.0;
    bool av_drift_available = false;
    exosnap::engine::RecordingDiagnosticsSnapshot completed_snapshot;

    // Markers pre-loaded from the recording session (fallback if sidecar cannot be read)
    std::vector<RecordingMarker> markers;
    QString marker_sidecar_path; // companion .markers.json path

    // Total recording duration in seconds (0.0 = unknown). Used to place
    // markers proportionally on the Edit timeline; unknown duration renders an
    // inert timeline (no handles, playhead, or markers).
    double duration_seconds = 0.0;
};

} // namespace exosnap
