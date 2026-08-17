#pragma once

// EventQuery.h -- a bounded, read-only view of the structured event stream.
//
// The authoritative owner is recorder_core::logging: every structured event, from
// the engine and from diagnostics::logEvent alike, goes through one logger, which
// keeps a bounded in-memory ring (LoggerConfig::ringCapacity) alongside the JSONL
// file. This queries THAT ring.
//
// It is deliberately not a log-file API. There is no path parameter, no offset,
// no follow mode and no way to reach a rotated file: a consumer gets the most
// recent N records that match a filter, and nothing else. An unbounded history
// query over a file the support bundle also scrubs would be a second, unscrubbed
// way out of the process.
//
// Correlation keys are promoted out of the record's fields rather than parsed out
// of its text:
//   * `launchSessionId` -- stamped on every record by EngineLogBridge as the
//     `session` base field.
//   * `recordingSessionId` / `updateTransactionId` -- present only on the events
//     that belong to a recording or an update transaction. Absent is `null`, and
//     a filter on one of them simply matches nothing rather than erroring.

#include <QJsonObject>
#include <QString>

#include <cstdint>

namespace exosnap::observability {

// Field keys that carry a correlation id. Written once here because the emitting
// site and the query have to agree on them and there is no other coupling.
namespace event_field {
inline constexpr const char* kLaunchSession = "session";
inline constexpr const char* kRecordingSession = "recordingSessionId";
inline constexpr const char* kUpdateTransaction = "updateTransactionId";
} // namespace event_field

struct EventQueryFilter {
    // Newest-first cap. Clamped into [1, kMaxEvents]; 0 means "the default".
    int max = 0;
    QString subsystem;    // exact match on the record's component
    QString event_code;   // exact match on the record's event token
    QString min_severity; // "debug"|"info"|"warning"|"error"|"critical"
    QString launch_session_id;
    QString recording_session_id;
    QString update_transaction_id;
};

// The ring is 512 deep; asking for more than it holds is not an error, it just
// cannot be answered. A default of 50 keeps an unfiltered call cheap.
inline constexpr int kDefaultEvents = 50;
inline constexpr int kMaxEvents = 512;

// Parses the filter out of a command's params. Unknown keys are ignored; an
// invalid severity is reported so a typo does not silently widen the result.
[[nodiscard]] EventQueryFilter ParseEventQueryFilter(const QJsonObject& params, QString* error);

// Newest first. `truncated` is true when the filter matched more records than
// `max` allowed through, so a reader can tell "that is all of them" from "that is
// the newest slice".
[[nodiscard]] QJsonObject QueryEvents(const EventQueryFilter& filter);

} // namespace exosnap::observability
