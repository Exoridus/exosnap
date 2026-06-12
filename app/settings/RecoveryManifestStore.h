#pragma once

#include <QString>
#include <QVector>

namespace exosnap {

// A single recovery manifest entry. Written before session start and removed
// on clean completion. Surviving entries at startup indicate interrupted
// sessions whose artefacts may need repair or export.
struct RecoveryManifestEntry {
    QString id;                 // UUID (random, stable for entry lifetime)
    QString artefact_path;      // actual file on disk (.mkv or .mkv.tmp)
    QString intended_container; // "mkv" or "mp4"
    QString final_output_path;  // desired output path (may differ from artefact)
    QString started_at;         // ISO-8601 timestamp
    bool finalized = false;     // true when MKV engine finished cleanly
                                // (artefact is ready to rename/remux without repair)
};

// Crash-manifest store — recovery-manifest.json in the app config dir.
// Analogous to RecordingHistoryStore in structure; minimal in scope.
//
// Thread-safety: all methods must be called from the same thread (Qt main
// thread). Writes flush immediately so a crash between Add and the matching
// Remove still leaves a recoverable entry.
class RecoveryManifestStore {
  public:
    RecoveryManifestStore();
    explicit RecoveryManifestStore(QString file_path);

    // Load entries from disk. Returns empty on missing or corrupt file.
    [[nodiscard]] QVector<RecoveryManifestEntry> Load() const;

    // Persist the current in-memory list. Called internally after mutations.
    bool Save(const QVector<RecoveryManifestEntry>& entries) const;

    // Add a new entry and immediately flush to disk.
    bool Add(const RecoveryManifestEntry& entry);

    // Mark an existing entry as finalized=true and flush.
    // No-op (returns true) when id is not found.
    bool UpdateFinalized(const QString& id, bool finalized);

    // Remove the entry with the given id and flush.
    // No-op (returns true) when id is not found.
    bool Remove(const QString& id);

    // Return all current entries (loads from disk each time).
    [[nodiscard]] QVector<RecoveryManifestEntry> Entries() const;

    [[nodiscard]] const QString& StorePath() const;

  private:
    QString file_path_;
    static constexpr int kSchemaVersion = 1;
};

} // namespace exosnap
