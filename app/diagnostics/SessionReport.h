#pragma once

#include "../viewmodels/RecordViewModel.h"
#include "SessionLedger.h"

#include <exosnap/engine/pipeline_diagnostics.h>

#include <QByteArray>
#include <QString>

#include <vector>

namespace exosnap::diagnostics {

// Everything the session report needs, gathered at recording end. Kept as plain
// data (no engine/UI coupling beyond the two structs it summarizes) so the pure
// builder below is deterministically testable from fixtures.
//
// "Session" here is the *recording* session (a stable id per StartRecording,
// distinct from the launch/log session id, which is carried alongside for
// support correlation). Metrics come from the final diagnostics snapshot; a
// missing/invalid snapshot (e.g. an early failure) yields "unavailable" rather
// than fabricated zeros. The report holds NO absolute paths — only a scrubbed
// output file name, byte counts and codecs.
struct SessionReportInputs {
    int schema_version = 1;
    QString recording_session_id;
    QString launch_session_id;
    QString app_version;
    QString started_at; // ISO 8601 (local); empty to omit
    QString ended_at;

    UiRecordingResult result;

    bool has_snapshot = false;
    exosnap::engine::RecordingDiagnosticsSnapshot snapshot;

    // Requested config, stringified by the caller so a failed recording (no
    // snapshot) still records what was asked for. Independent of the snapshot.
    QString capture_backend; // "dxgi-od" / "wgc" / "unknown"
    int bit_depth = 8;
    QString chroma;
    QString color_range;
    QString hdr_mode;

    // Scrubbed output file *name* only (never a path). Empty to omit entirely.
    QString output_filename;

    // QCR-804: how many times an active WINDOW capture was confirmed to have
    // stopped producing frames during this recording and the user was warned.
    // Non-zero means the picture was frozen for at least one stretch of the
    // session even though the recording ran to completion — the one fact a
    // finished file cannot tell its owner by itself.
    uint32_t window_capture_stall_episodes = 0;

    // The session ledger, frozen at Stop: every Tier-2 measured problem that fired
    // during this recording, with its occurrences. Empty means nothing measured
    // went wrong, and the report then carries no ledger key at all -- an empty
    // array and "the build has no ledger" would read the same.
    std::vector<LedgerEntry> ledger;
};

// Pure: serialize the inputs to the canonical session-report JSON (Qt JSON, not
// nlohmann — the app layer is uniformly Qt JSON). Deterministic for fixed inputs.
[[nodiscard]] QByteArray BuildSessionReportJson(const SessionReportInputs& inputs);

// Write session-<recording_session_id>.json into reports_dir atomically
// (QSaveFile), then prune so at most keep_n reports remain (oldest by mtime
// removed). Returns true on success; sets *out_path to the written file path.
bool WriteSessionReport(const QString& reports_dir, const SessionReportInputs& inputs, int keep_n,
                        QString* out_path = nullptr, QString* out_error = nullptr);

} // namespace exosnap::diagnostics
