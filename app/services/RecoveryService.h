#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include <QString>
#include <QVector>

#include "settings/RecoveryManifestStore.h"

namespace exosnap {

// Rich candidate info surfaced to the UI (superset of manifest entry).
struct RecoveryCandidate {
    RecoveryManifestEntry entry;
    qint64 artefact_size_bytes = 0; // file_size at scan time, 0 if unknown
};

// Result of a recovery action (Finish, Discard, etc.).
struct RecoveryActionResult {
    bool success = false;
    std::string message; // human-readable error on failure, empty on success
};

// UI-agnostic service for startup crash-recovery (ADR-0014, ADR-0015).
// Scan() must be called first; the action methods operate on individual
// entries returned by Scan(). All blocking remux calls happen on the calling
// thread — callers must dispatch to a worker thread themselves.
class RecoveryService {
  public:
    // `store` must outlive this object.
    explicit RecoveryService(RecoveryManifestStore& store);

    // Inject a fallback output folder used by Finish() when the stored folder
    // no longer exists on disk. Typically the current configured output directory.
    // Optional; when empty the artefact's parent folder is used as last-resort fallback.
    void SetFallbackOutputFolder(const QString& folder);

    // Load the manifest; remove entries whose artefact no longer exists on
    // disk; return surviving candidates enriched with file-size metadata.
    [[nodiscard]] QVector<RecoveryCandidate> Scan();

    // "Finish" action (ADR-0015): honour the manifest snapshot.
    //   target container = intended_container, destination = final_output_path.
    //   Fallback to the current configured output directory when the stored
    //   folder no longer exists on disk.
    //
    //   - MKV-intended + finalized=true  → collision-safe rename → .mkv
    //   - MKV-intended + finalized=false → RemuxToMkv (repair-remux) → .mkv
    //   - MP4-intended (any)             → RemuxToProgressiveMp4 → .mp4
    //
    // progress_cb follows the RemuxProgressCallback contract (return false = cancel).
    // On cancel or error the artefact and entry are preserved.
    RecoveryActionResult Finish(const RecoveryManifestEntry& entry, std::function<bool(float)> progress_cb = nullptr);

    // "Discard" action: deletes the artefact file and removes the manifest entry.
    // The caller must have already shown an inline confirmation before calling.
    RecoveryActionResult Discard(const RecoveryManifestEntry& entry);

    // --- Legacy action aliases kept for test compatibility ---
    // "Keep as MKV" — equivalent to Finish for MKV-intended entries.
    RecoveryActionResult KeepAsMkv(const RecoveryManifestEntry& entry,
                                   std::function<bool(float)> progress_cb = nullptr);
    // "Export as MP4" — equivalent to Finish for MP4-intended entries.
    RecoveryActionResult ExportAsMp4(const RecoveryManifestEntry& entry,
                                     std::function<bool(float)> progress_cb = nullptr);

  private:
    RecoveryManifestStore& store_;
    QString fallback_output_folder_;

    // Resolve the destination folder: use the stored folder when it exists on
    // disk; fall back to fallback_output_folder_; last-resort: artefact parent.
    std::filesystem::path ResolveDestinationFolder(const RecoveryManifestEntry& entry) const;

    // Returns a collision-safe path derived from `preferred`. If preferred does
    // not exist it is returned as-is; otherwise tries "name (2).ext" ... "(N).ext".
    static std::filesystem::path ResolveUniqueOutputPath(const std::filesystem::path& preferred);
};

} // namespace exosnap
