#pragma once

#include <QString>
#include <QVector>

#include <mutex>

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
// Thread-safety: every public method is safe to call from any thread. The store
// owns a mutex that covers each mutation as one transaction, because every
// mutation is a load → mutate → save sequence and QSaveFile only makes the
// single file replacement atomic, not the read-modify-write around it. Two
// unsynchronized mutations from different threads would each load the same
// snapshot and the second save would silently discard the first one's entry.
//
// This is not theoretical: Add/UpdateFinalized/Remove are called from the Qt main
// thread (start, cancel unwind), the recording thread (finalize, stop) and the mux
// worker + segment-remux threads (split boundaries, per-segment remux completion).
//
// The mutex is in-process only — it does not coordinate with a second ExoSnap
// process. Writes still flush immediately so a crash between Add and the matching
// Remove leaves a recoverable entry.
class RecoveryManifestStore {
  public:
    RecoveryManifestStore();
    explicit RecoveryManifestStore(QString file_path);

    // Load entries from disk. Returns empty on missing or corrupt file.
    // Never observes a partially-applied mutation.
    [[nodiscard]] QVector<RecoveryManifestEntry> Load() const;

    // Persist the given list, replacing everything on disk. Exposed for the
    // recovery scan, which rewrites the manifest wholesale.
    bool Save(const QVector<RecoveryManifestEntry>& entries) const;

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

    [[nodiscard]] const QString& StorePath() const;

  private:
    // Unlocked cores. Callers must already hold mutex_ — this is what lets a
    // mutation hold the lock across its whole load → mutate → save sequence
    // instead of taking it three separate times.
    [[nodiscard]] QVector<RecoveryManifestEntry> LoadLocked() const;
    bool SaveLocked(const QVector<RecoveryManifestEntry>& entries) const;

    // Guards every read and every load → mutate → save transaction. Public methods
    // take it exactly once and then work through the *Locked cores, so no path can
    // re-enter it. Contention is a handful of writes per recording.
    mutable std::mutex mutex_;
    QString file_path_;
    static constexpr int kSchemaVersion = 1;
};

} // namespace exosnap
