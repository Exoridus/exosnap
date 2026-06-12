#include "RecoveryManifestStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include "diagnostics/AppLog.h"
#include "settings/ConfigPaths.h"

namespace exosnap {
namespace {

constexpr int kSchemaVersion = 1;

QJsonObject EntryToJson(const RecoveryManifestEntry& e) {
    QJsonObject obj;
    obj[QStringLiteral("id")] = e.id;
    obj[QStringLiteral("artefact_path")] = e.artefact_path;
    obj[QStringLiteral("intended_container")] = e.intended_container;
    obj[QStringLiteral("final_output_path")] = e.final_output_path;
    obj[QStringLiteral("started_at")] = e.started_at;
    obj[QStringLiteral("finalized")] = e.finalized;
    return obj;
}

bool EntryFromJson(const QJsonObject& obj, RecoveryManifestEntry& out) {
    const QString id = obj.value(QStringLiteral("id")).toString().trimmed();
    if (id.isEmpty())
        return false;
    const QString artefact = obj.value(QStringLiteral("artefact_path")).toString().trimmed();
    if (artefact.isEmpty())
        return false;
    const QString container = obj.value(QStringLiteral("intended_container")).toString().trimmed().toLower();
    if (container != QStringLiteral("mkv") && container != QStringLiteral("mp4"))
        return false;

    out.id = id;
    out.artefact_path = artefact;
    out.intended_container = container;
    out.final_output_path = obj.value(QStringLiteral("final_output_path")).toString();
    out.started_at = obj.value(QStringLiteral("started_at")).toString();
    out.finalized = obj.value(QStringLiteral("finalized")).toBool(false);
    return true;
}

} // namespace

RecoveryManifestStore::RecoveryManifestStore() {
    const QString config_dir = settings::ResolveAppConfigDir();
    if (!config_dir.isEmpty()) {
        QDir().mkpath(config_dir);
        file_path_ = QDir(config_dir).filePath(QStringLiteral("recovery-manifest.json"));
    } else {
        file_path_ = QStringLiteral("recovery-manifest.json");
    }
}

RecoveryManifestStore::RecoveryManifestStore(QString file_path) : file_path_(std::move(file_path)) {
}

QVector<RecoveryManifestEntry> RecoveryManifestStore::Load() const {
    QVector<RecoveryManifestEntry> result;

    if (file_path_.isEmpty())
        return result;

    QFileInfo info(file_path_);
    if (!info.exists())
        return result; // no manifest yet — normal on first run

    QFile file(file_path_);
    if (!file.open(QIODevice::ReadOnly)) {
        diagnostics::AppLog::warning(QStringLiteral("recovery.manifest"),
                                     QStringLiteral("Cannot open manifest for reading: %1").arg(file_path_));
        return result;
    }

    const QByteArray data = file.readAll();
    file.close();

    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        // Corrupt manifest — reset silently (pre-v1 policy: no migration).
        diagnostics::AppLog::warning(QStringLiteral("recovery.manifest"),
                                     QStringLiteral("Corrupt manifest — resetting: %1").arg(parse_error.errorString()));
        return result;
    }

    const QJsonObject root = doc.object();
    const int version = root.value(QStringLiteral("schema_version")).toInt(0);
    if (version < 1 || version > kSchemaVersion) {
        diagnostics::AppLog::warning(
            QStringLiteral("recovery.manifest"),
            QStringLiteral("Incompatible manifest schema version %1 — resetting").arg(version));
        return result;
    }

    const QJsonArray entries = root.value(QStringLiteral("entries")).toArray();
    result.reserve(entries.size());
    for (const QJsonValue& val : entries) {
        if (!val.isObject())
            continue;
        RecoveryManifestEntry entry;
        if (EntryFromJson(val.toObject(), entry))
            result.append(std::move(entry));
    }

    return result;
}

bool RecoveryManifestStore::Save(const QVector<RecoveryManifestEntry>& entries) const {
    if (file_path_.isEmpty())
        return false;

    QDir().mkpath(QFileInfo(file_path_).absolutePath());

    QJsonArray arr;
    for (const auto& e : entries)
        arr.append(EntryToJson(e));

    QJsonObject root;
    root[QStringLiteral("schema_version")] = kSchemaVersion;
    root[QStringLiteral("entries")] = arr;

    QSaveFile file(file_path_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        diagnostics::AppLog::warning(QStringLiteral("recovery.manifest"),
                                     QStringLiteral("Cannot open manifest for atomic write: %1").arg(file_path_));
        return false;
    }

    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));

    if (!file.commit()) {
        diagnostics::AppLog::warning(QStringLiteral("recovery.manifest"),
                                     QStringLiteral("Atomic manifest save commit failed: %1").arg(file_path_));
        return false;
    }

    return true;
}

bool RecoveryManifestStore::Add(const RecoveryManifestEntry& entry) {
    auto entries = Load();
    entries.append(entry);
    return Save(entries);
}

bool RecoveryManifestStore::UpdateFinalized(const QString& id, bool finalized) {
    auto entries = Load();
    for (auto& e : entries) {
        if (e.id == id) {
            e.finalized = finalized;
            return Save(entries);
        }
    }
    return true; // not found — benign
}

bool RecoveryManifestStore::Remove(const QString& id) {
    auto entries = Load();
    const int before = entries.size();
    entries.removeIf([&id](const RecoveryManifestEntry& e) { return e.id == id; });
    if (entries.size() == before)
        return true; // not found — benign
    return Save(entries);
}

QVector<RecoveryManifestEntry> RecoveryManifestStore::Entries() const {
    return Load();
}

const QString& RecoveryManifestStore::StorePath() const {
    return file_path_;
}

} // namespace exosnap
