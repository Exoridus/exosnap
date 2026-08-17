#include "observability/SessionQuery.h"

#include "diagnostics/AppLog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>

namespace exosnap::observability {
namespace {

QJsonObject Unavailable(const QString& reason) {
    QJsonObject json;
    json.insert(QStringLiteral("available"), false);
    json.insert(QStringLiteral("reason"), reason);
    return json;
}

QJsonObject ReadReport(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return Unavailable(QStringLiteral("unreadable"));

    QJsonParseError parse_error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
        return Unavailable(QStringLiteral("malformed"));

    QJsonObject json;
    json.insert(QStringLiteral("available"), true);
    // The report verbatim. It is already the scrubbed document the support
    // bundle ships; re-shaping it here would produce a second vocabulary for the
    // same facts.
    json.insert(QStringLiteral("report"), document.object());
    return json;
}

// An id, not a path. The writer produces `session-<id>.json`, and the id itself
// is a UUID without braces -- anything outside that alphabet cannot name a
// report and is refused rather than concatenated into a path.
bool IsPlausibleSessionId(const QString& id) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9._-]{1,128}$"));
    return pattern.match(id).hasMatch();
}

} // namespace

QString SessionReportsDirectory() {
    const QString log_path = diagnostics::AppLog::logFilePath();
    if (log_path.isEmpty())
        return {};
    const QString directory = QFileInfo(log_path).absolutePath();
    if (directory.isEmpty())
        return {};
    return directory + QStringLiteral("/reports");
}

QJsonObject LatestSessionReport() {
    const QString directory = SessionReportsDirectory();
    if (directory.isEmpty())
        return Unavailable(QStringLiteral("noReportsDirectory"));

    QDir dir(directory);
    const QFileInfoList entries =
        dir.entryInfoList({QStringLiteral("session-*.json")}, QDir::Files, QDir::Time | QDir::Reversed);
    if (entries.isEmpty())
        return Unavailable(QStringLiteral("noSessionReport"));
    // QDir::Time is newest-first; Reversed makes it oldest-first, so the newest
    // is the last entry. Spelled out because getting this backwards would report
    // the oldest recording as the latest one and look entirely plausible.
    return ReadReport(entries.last().absoluteFilePath());
}

QJsonObject SessionReportById(const QString& recording_session_id) {
    if (!IsPlausibleSessionId(recording_session_id))
        return Unavailable(QStringLiteral("invalidSessionId"));

    const QString directory = SessionReportsDirectory();
    if (directory.isEmpty())
        return Unavailable(QStringLiteral("noReportsDirectory"));

    const QString path = QDir(directory).filePath(QStringLiteral("session-%1.json").arg(recording_session_id));
    if (!QFile::exists(path))
        return Unavailable(QStringLiteral("noSessionReport"));
    return ReadReport(path);
}

} // namespace exosnap::observability
