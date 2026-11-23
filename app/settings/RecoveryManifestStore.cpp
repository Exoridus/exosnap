#include "RecoveryManifestStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSaveFile>

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>

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

std::optional<QJsonObject> ReadEntry(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const auto document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? std::optional(document.object()) : std::nullopt;
}

} // namespace

struct RecoveryManifestStore::EntryLocks {
    std::shared_ptr<std::mutex> ForFile(const QString& path) {
        const auto name = QFileInfo(path).fileName();
        std::lock_guard lock(mutex);
        std::erase_if(by_file, [](const auto& item) { return item.second.expired(); });
        auto& weak = by_file[name];
        auto entry = weak.lock();
        if (!entry) {
            entry = std::make_shared<std::mutex>();
            weak = entry;
        }
        return entry;
    }

    std::mutex mutex;
    std::map<QString, std::weak_ptr<std::mutex>> by_file;
};

RecoveryManifestStore::RecoveryManifestStore() : locks_(std::make_shared<EntryLocks>()) {
    const QString config_dir = settings::ResolveAppConfigDir();
    if (!config_dir.isEmpty()) {
        QDir().mkpath(config_dir);
        file_path_ = QDir(config_dir).filePath(QStringLiteral("recovery-manifest.json"));
    } else {
        file_path_ = QStringLiteral("recovery-manifest.json");
    }
}

RecoveryManifestStore::RecoveryManifestStore(QString file_path)
    : locks_(std::make_shared<EntryLocks>()), file_path_(std::move(file_path)) {
}

QVector<RecoveryManifestEntry> RecoveryManifestStore::Load() const {
    QMap<QString, RecoveryManifestEntry> entries;
    if (const auto legacy = LoadLegacy())
        for (const auto& entry : *legacy)
            entries.insert(entry.id, entry);
    if (!file_path_.isEmpty()) {
        const QDir directory(EntriesDirectory());
        for (const auto& file : directory.entryInfoList({QStringLiteral("*.json")}, QDir::Files)) {
            const auto path = file.absoluteFilePath();
            // A Windows reader without delete sharing can deny QSaveFile's replacement of this entry.
            const auto entry_lock = locks_->ForFile(path);
            const std::lock_guard lock(*entry_lock);
            const auto record = ReadEntry(path);
            if (!record)
                continue;
            const auto id = record->value(QStringLiteral("id")).toString();
            if (id.isEmpty() || file.fileName() != QFileInfo(EntryPath(id)).fileName())
                continue;
            if (record->value(QStringLiteral("removed")).toBool()) {
                entries.remove(id);
            } else if (RecoveryManifestEntry entry; EntryFromJson(*record, entry)) {
                entries.insert(id, entry);
            }
        }
    }
    auto ordered = entries.values();
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const auto& a, const auto& b) { return a.started_at < b.started_at; });
    return ordered;
}

std::optional<QVector<RecoveryManifestEntry>> RecoveryManifestStore::LoadLegacy() const {
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
        return std::nullopt;
    }

    const QByteArray data = file.readAll();
    if (file.error() != QFileDevice::NoError)
        return std::nullopt;
    file.close();

    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        diagnostics::AppLog::warning(
            QStringLiteral("recovery.manifest"),
            QStringLiteral("Corrupt legacy recovery manifest: %1").arg(parse_error.errorString()));
        return std::nullopt;
    }

    const QJsonObject root = doc.object();
    const int version = root.value(QStringLiteral("schema_version")).toInt(0);
    if (version < 1 || version > kSchemaVersion) {
        diagnostics::AppLog::warning(
            QStringLiteral("recovery.manifest"),
            QStringLiteral("Incompatible legacy recovery manifest schema version %1").arg(version));
        return std::nullopt;
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

QString RecoveryManifestStore::EntriesDirectory() const {
    return file_path_ + QStringLiteral(".entries");
}

QString RecoveryManifestStore::EntryPath(const QString& id) const {
    const auto digest = QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(EntriesDirectory()).filePath(QString::fromLatin1(digest) + QStringLiteral(".json"));
}

bool RecoveryManifestStore::CommitEntry(const QString& id, const QJsonObject& record) const {
    if (file_path_.isEmpty())
        return false;

    const auto path = EntryPath(id);
    const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
    QSaveFile file(path);
    if (!QDir().mkpath(EntriesDirectory()) || !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        diagnostics::AppLog::warning(QStringLiteral("recovery.manifest"),
                                     QStringLiteral("Cannot write recovery entry: %1").arg(path));
        return false;
    }

    if (before_commit_)
        before_commit_(id);
    if (file.commit())
        return true;
    diagnostics::AppLog::warning(QStringLiteral("recovery.manifest"),
                                 QStringLiteral("Atomic recovery entry commit failed: %1").arg(path));
    return false;
}

bool RecoveryManifestStore::Add(const RecoveryManifestEntry& entry) {
    if (entry.id.isEmpty())
        return false;
    const auto entry_lock = locks_->ForFile(EntryPath(entry.id));
    std::lock_guard lock(*entry_lock);
    return CommitEntry(entry.id, EntryToJson(entry));
}

bool RecoveryManifestStore::UpdateFinalized(const QString& id, bool finalized) {
    if (file_path_.isEmpty())
        return true;
    const auto path = EntryPath(id);
    const auto entry_lock = locks_->ForFile(path);
    std::lock_guard lock(*entry_lock);
    auto record = ReadEntry(path);
    if (!record) {
        if (QFile::exists(path))
            return false;
        const auto legacy = LoadLegacy();
        if (!legacy)
            return false;
        const auto found =
            std::find_if(legacy->begin(), legacy->end(), [&](const auto& entry) { return entry.id == id; });
        if (found == legacy->end()) {
            return true;
        }
        record = EntryToJson(*found);
    }
    if (record->value(QStringLiteral("removed")).toBool())
        return true;
    record->insert(QStringLiteral("finalized"), finalized);
    return CommitEntry(id, *record);
}

bool RecoveryManifestStore::Remove(const QString& id) {
    if (file_path_.isEmpty())
        return true; // not found — benign
    const auto path = EntryPath(id);
    const auto entry_lock = locks_->ForFile(path);
    std::lock_guard lock(*entry_lock);
    const auto legacy = LoadLegacy();
    if (!legacy)
        return false;
    if (std::any_of(legacy->begin(), legacy->end(), [&](const auto& entry) { return entry.id == id; })) {
        // Suppress the legacy entry without rewriting other recordings' recovery data.
        return CommitEntry(id, {{QStringLiteral("id"), id}, {QStringLiteral("removed"), true}});
    }
    if (!QFile::exists(path) || QFile::remove(path))
        return true;
    diagnostics::AppLog::warning(QStringLiteral("recovery.manifest"),
                                 QStringLiteral("Cannot remove recovery entry: %1").arg(path));
    return false;
}

QVector<RecoveryManifestEntry> RecoveryManifestStore::Entries() const {
    return Load();
}

const QString& RecoveryManifestStore::StorePath() const {
    return file_path_;
}

} // namespace exosnap
