#include "observability/EventQuery.h"

#include "observability/ObservabilityJson.h"

#include "diagnostics/AppLog.h"

#include <recorder_core/logging/logging.h>

#include <QDateTime>
#include <QJsonArray>
#include <QTimeZone>

#include <algorithm>
#include <chrono>
#include <optional>
#include <vector>

namespace exosnap::observability {
namespace {

using recorder_core::logging::LogLevel;
using recorder_core::logging::LogRecord;

QString SeverityKey(LogLevel level) {
    switch (level) {
    case LogLevel::Trace:
        return QStringLiteral("trace");
    case LogLevel::Debug:
        return QStringLiteral("debug");
    case LogLevel::Info:
        return QStringLiteral("info");
    case LogLevel::Warn:
        return QStringLiteral("warning");
    case LogLevel::Error:
        return QStringLiteral("error");
    case LogLevel::Critical:
        return QStringLiteral("critical");
    }
    return QStringLiteral("info");
}

std::optional<LogLevel> SeverityFromKey(const QString& key) {
    if (key == QLatin1String("trace"))
        return LogLevel::Trace;
    if (key == QLatin1String("debug"))
        return LogLevel::Debug;
    if (key == QLatin1String("info"))
        return LogLevel::Info;
    if (key == QLatin1String("warning"))
        return LogLevel::Warn;
    if (key == QLatin1String("error"))
        return LogLevel::Error;
    if (key == QLatin1String("critical"))
        return LogLevel::Critical;
    return std::nullopt;
}

QString FieldValue(const LogRecord& record, const char* key) {
    for (const recorder_core::logging::LogField& field : record.fields) {
        if (field.key == key)
            return QString::fromStdString(field.value);
    }
    return {};
}

bool MatchesId(const QString& wanted, const LogRecord& record, const char* key) {
    return wanted.isEmpty() || FieldValue(record, key) == wanted;
}

// The launch session id is stamped into the JSONL file as a logger BASE field,
// which the engine deliberately keeps out of the in-memory record: base fields
// would otherwise also land in the flattened text line, putting the same session
// id on every log entry when the startup banner already carries it once.
//
// The ring therefore has no session field -- but it does not need one. Every
// record in it was produced by THIS process, so this launch's id is the answer
// for all of them. A per-call field of the same key still wins, so a record that
// deliberately names a different launch (a replayed or imported record, should
// one ever exist) is reported as what it says it is.
QString LaunchSessionOf(const LogRecord& record) {
    const QString explicit_value = FieldValue(record, event_field::kLaunchSession);
    return explicit_value.isEmpty() ? diagnostics::AppLog::sessionId() : explicit_value;
}

QString Iso8601(const std::chrono::system_clock::time_point& timestamp) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch()).count();
    return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(ms), QTimeZone::UTC).toString(Qt::ISODateWithMs);
}

QJsonObject EventToJson(const LogRecord& record) {
    QJsonObject json;
    json.insert(QStringLiteral("timestamp"), Iso8601(record.timestamp));
    json.insert(QStringLiteral("severity"), SeverityKey(record.level));
    json.insert(QStringLiteral("subsystem"), QString::fromStdString(record.component));
    // The engine's own records carry a human sentence here; the application's
    // structured events carry a stable token (diagnostics::logEvent's event_code).
    // Both are the record's own identity -- this does not invent a second one.
    json.insert(QStringLiteral("eventCode"), QString::fromStdString(record.message));

    QJsonObject fields;
    for (const recorder_core::logging::LogField& field : record.fields)
        fields.insert(QString::fromStdString(field.key), QString::fromStdString(field.value));
    json.insert(QStringLiteral("fields"), fields);

    json.insert(QStringLiteral("launchSessionId"), TextOrNull(LaunchSessionOf(record)));
    json.insert(QStringLiteral("recordingSessionId"), TextOrNull(FieldValue(record, event_field::kRecordingSession)));
    json.insert(QStringLiteral("updateTransactionId"), TextOrNull(FieldValue(record, event_field::kUpdateTransaction)));
    return json;
}

} // namespace

EventQueryFilter ParseEventQueryFilter(const QJsonObject& params, QString* error) {
    EventQueryFilter filter;
    filter.max = static_cast<int>(params.value(QStringLiteral("max")).toDouble());
    filter.subsystem = params.value(QStringLiteral("subsystem")).toString();
    filter.event_code = params.value(QStringLiteral("eventCode")).toString();
    filter.min_severity = params.value(QStringLiteral("severity")).toString();
    filter.launch_session_id = params.value(QStringLiteral("launchSessionId")).toString();
    filter.recording_session_id = params.value(QStringLiteral("recordingSessionId")).toString();
    filter.update_transaction_id = params.value(QStringLiteral("updateTransactionId")).toString();

    if (!filter.min_severity.isEmpty() && !SeverityFromKey(filter.min_severity).has_value()) {
        // Loud rather than ignored: a mistyped severity that silently fell back
        // to "everything" would make a check that filters for errors pass on a
        // stream full of Info records.
        if (error != nullptr)
            *error = QStringLiteral("Unknown severity \"%1\"").arg(filter.min_severity);
    }
    return filter;
}

QJsonObject QueryEvents(const EventQueryFilter& filter) {
    const int limit = filter.max <= 0 ? kDefaultEvents : std::min(filter.max, kMaxEvents);
    const std::optional<LogLevel> min_level = SeverityFromKey(filter.min_severity);

    const std::vector<LogRecord> ring = recorder_core::logging::snapshot_ring_buffer();

    // Newest first, and stop as soon as `limit` matched -- the walk is backwards
    // so a full ring with a narrow filter still costs at most one pass.
    QJsonArray events;
    std::uint64_t matched = 0;
    for (auto it = ring.rbegin(); it != ring.rend(); ++it) {
        const LogRecord& record = *it;
        if (!filter.subsystem.isEmpty() && QString::fromStdString(record.component) != filter.subsystem)
            continue;
        if (!filter.event_code.isEmpty() && QString::fromStdString(record.message) != filter.event_code)
            continue;
        if (min_level.has_value() && static_cast<int>(record.level) < static_cast<int>(*min_level))
            continue;
        if (!filter.launch_session_id.isEmpty() && LaunchSessionOf(record) != filter.launch_session_id)
            continue;
        if (!MatchesId(filter.recording_session_id, record, event_field::kRecordingSession))
            continue;
        if (!MatchesId(filter.update_transaction_id, record, event_field::kUpdateTransaction))
            continue;

        ++matched;
        if (static_cast<int>(events.size()) < limit)
            events.append(EventToJson(record));
    }

    QJsonObject json;
    const int returned = static_cast<int>(events.size());
    json.insert(QStringLiteral("events"), events);
    json.insert(QStringLiteral("returned"), returned);
    json.insert(QStringLiteral("matched"), Count(matched));
    json.insert(QStringLiteral("max"), limit);
    // The ring is the whole history this process keeps in memory. Saying how deep
    // it is turns "matched == ringCapacity" from a coincidence into an answer.
    json.insert(QStringLiteral("ringSize"), static_cast<int>(ring.size()));
    json.insert(QStringLiteral("truncated"), matched > static_cast<std::uint64_t>(returned));
    return json;
}

} // namespace exosnap::observability
