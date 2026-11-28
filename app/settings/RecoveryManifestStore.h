#pragma once

#include <QString>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

class QJsonObject;

namespace exosnap {

// A single recovery manifest entry. Written before session start and removed
// on clean completion. Surviving entries at startup indicate interrupted
// sessions whose artefacts may need repair or export.
struct RecoveryManifestEntry {
    QString id;                 // UUID (random, stable for entry lifetime)
    QString artefact_path;      // actual live artifact on disk; legacy .mkv.tmp remains readable
    QString intended_container; // "mkv" or "mp4"
    QString final_output_path;  // desired output path (may differ from artefact)
    QString started_at;         // ISO-8601 timestamp
    bool finalized = false;     // true when MKV engine finished cleanly
                                // (artefact is ready to rename/remux without repair)
    // A generated path conveys no ownership of a file at that path.
    bool final_output_is_preferred = false;
};

// Each entry commits independently, so a stalled split cannot pin the manifest
// transaction of another recording or an earlier segment's remux. Copies
// serialize mutations of the same entry within the process.
class RecoveryManifestStore {
  public:
    RecoveryManifestStore();
    explicit RecoveryManifestStore(QString file_path);

    // Copies retain the same path and serialize updates with the original store.
    RecoveryManifestStore(const RecoveryManifestStore&) = default;
    RecoveryManifestStore& operator=(const RecoveryManifestStore&) = delete;

    // Load committed entries, including entries from the original JSON manifest.
    [[nodiscard]] QVector<RecoveryManifestEntry> Load() const;

    // Add a new entry and immediately flush to disk. Returns false when the
    // entry did NOT reach disk — the caller must not treat the recording as
    // recovery-protected in that case.
    bool Add(const RecoveryManifestEntry& entry);

    // Mark an existing entry as finalized=true and flush.
    // No-op (returns true) when id is not found.
    bool UpdateFinalized(const QString& id, bool finalized);

    // Remove the entry with the given id and flush.
    // No-op (returns true) when id is not found.
    bool Remove(const QString& id);

    // Return all current entries (loads from disk each time).
    [[nodiscard]] QVector<RecoveryManifestEntry> Entries() const;

    // The legacy manifest path also determines the directory for current entries.
    [[nodiscard]] const QString& StorePath() const;

  private:
    friend class RecoveryManifestStoreTestPeer;
    struct EntryLocks;
    std::shared_ptr<EntryLocks> locks_;
    std::function<void(const QString&)> before_commit_;
    [[nodiscard]] std::optional<QVector<RecoveryManifestEntry>> LoadLegacy() const;
    [[nodiscard]] QString EntriesDirectory() const;
    [[nodiscard]] QString EntryPath(const QString& id) const;
    bool CommitEntry(const QString& id, const QJsonObject& record) const;
    QString file_path_;
};

} // namespace exosnap
