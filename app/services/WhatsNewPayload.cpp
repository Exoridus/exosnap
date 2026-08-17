// WhatsNewPayload.cpp -- JSON persistence for the post-update What's-new overlay.

#include "WhatsNewPayload.h"

#include "settings/ConfigPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace exosnap {

bool WriteWhatsNewPayload(const QString& path, const WhatsNewPendingPayload& payload) {
    QJsonArray notes;
    for (const WhatsNewNote& n : payload.notes) {
        QJsonObject obj;
        obj[QStringLiteral("version")] = n.version;
        obj[QStringLiteral("body")] = n.body;
        obj[QStringLiteral("html_url")] = n.html_url;
        notes.append(obj);
    }

    QJsonObject root;
    root[QStringLiteral("target_version")] = payload.target_version;
    root[QStringLiteral("notes")] = notes;

    const QFileInfo info(path);
    QDir().mkpath(info.absolutePath());

    // Atomic write so a crash mid-write can't leave a half-written payload that
    // would be misread on the next launch.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit();
}

std::optional<WhatsNewPendingPayload> ReadWhatsNewPayload(const QString& path) {
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return std::nullopt;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return std::nullopt;

    const QJsonObject root = doc.object();
    WhatsNewPendingPayload payload;
    payload.target_version = root.value(QStringLiteral("target_version")).toString();
    for (const QJsonValue& v : root.value(QStringLiteral("notes")).toArray()) {
        const QJsonObject obj = v.toObject();
        WhatsNewNote note;
        note.version = obj.value(QStringLiteral("version")).toString();
        note.body = obj.value(QStringLiteral("body")).toString();
        note.html_url = obj.value(QStringLiteral("html_url")).toString();
        payload.notes.push_back(note);
    }
    return payload;
}

void DeleteWhatsNewPayload(const QString& path) {
    QFile::remove(path);
}

bool ShouldShowWhatsNew(const std::optional<WhatsNewPendingPayload>& payload, const QString& running_version,
                        bool suppressed) {
    if (!payload.has_value() || payload->notes.isEmpty())
        return false;
    if (suppressed)
        return false;
    return payload->target_version == running_version;
}

WhatsNewConsumption ConsumeWhatsNewPayload(const QString& path, const QString& running_version, bool suppressed) {
    const std::optional<WhatsNewPendingPayload> payload = ReadWhatsNewPayload(path);
    // Unconditional, and before the decision is even read: every outcome ends with
    // the file gone. See the header for why each one does.
    DeleteWhatsNewPayload(path);

    if (!ShouldShowWhatsNew(payload, running_version, suppressed))
        return {};
    return {/*show=*/true, payload->notes};
}

QString WhatsNewPayloadPath() {
    const QString config_dir = settings::ResolveAppConfigDir();
    if (config_dir.isEmpty())
        return QStringLiteral("whats-new-pending.json");
    return QDir(config_dir).filePath(QStringLiteral("whats-new-pending.json"));
}

} // namespace exosnap
