#pragma once

// SessionQuery.h -- read-only access to the canonical session reports.
//
// The authoritative owner is diagnostics::SessionReport: RecordingCoordinator
// writes one privacy-safe `session-<recording id>.json` per finished recording,
// already scrubbed (no absolute paths, no machine or user names, only a scrubbed
// output FILE name), and the support bundle collects exactly these files. This
// reads them back.
//
// It builds no second report. A report ExoSnap generated for support and a report
// an automation client reads have to be the same document, or one of them is
// evidence of the other's bugs.
//
// The directory is not a parameter and there is no path in the payload: reports
// live beside the application log, this resolves that location itself, and the
// only addressing a client gets is "the latest one" or "the one with this
// recording session id". That is deliberate -- a path parameter would turn a
// bounded report query into an arbitrary file read.

#include <QJsonObject>
#include <QString>

namespace exosnap::observability {

// Where the reports live, derived from the application log's directory. Empty
// when logging was never initialized (no directory, therefore no reports).
[[nodiscard]] QString SessionReportsDirectory();

// The newest report by modification time. `available` is false when no recording
// has finished in any session yet.
[[nodiscard]] QJsonObject LatestSessionReport();

// One report by its recording session id. `available` is false when no such
// report exists. The id is matched against the file NAME the writer produces and
// is rejected if it could escape the directory -- it is an id, not a path.
[[nodiscard]] QJsonObject SessionReportById(const QString& recording_session_id);

} // namespace exosnap::observability
