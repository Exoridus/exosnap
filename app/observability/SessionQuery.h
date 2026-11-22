#pragma once

// SessionQuery.h -- read-only access to the canonical session reports.
//
// RecordingCoordinator writes one `session-<recording id>.json` per finished
// recording. The query and support bundle both read the same report and omit
// unstructured error details present in older reports.
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
