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

// Result of a recovery action (KeepAsMkv, ExportAsMp4, Discard).
struct RecoveryActionResult {
    bool success = false;
    std::string message; // human-readable error on failure, empty on success
};

// UI-agnostic service for startup crash-recovery (ADR-0014).
// Scan() must be called first; the action methods operate on individual
// entries returned by Scan(). All blocking remux calls happen on the calling
// thread — callers must dispatch to a worker thread themselves.
class RecoveryService {
  public:
    // `store` must outlive this object.
    explicit RecoveryService(RecoveryManifestStore& store);

    // Load the manifest; remove entries whose artefact no longer exists on
    // disk; return surviving candidates enriched with file-size metadata.
    [[nodiscard]] QVector<RecoveryCandidate> Scan();

    // "Keep as MKV" action:
    //   - finalized=true  → collision-safe rename of artefact_path → .mkv output;
    //                        removes manifest entry on success.
    //   - finalized=false → Repair-Remux via RemuxToMkv to a collision-safe
    //                        .mkv output; deletes artefact on success;
    //                        removes manifest entry on success.
    // progress_cb follows the RemuxProgressCallback contract (return false = cancel).
    // On cancel or error the artefact and entry are preserved.
    RecoveryActionResult KeepAsMkv(const RecoveryManifestEntry& entry,
                                   std::function<bool(float)> progress_cb = nullptr);

    // "Export as MP4" action:
    //   RemuxToProgressiveMp4 to a collision-safe .mp4 output; deletes artefact
    //   on success; removes manifest entry on success.
    // On cancel or error artefact and entry are preserved.
    RecoveryActionResult ExportAsMp4(const RecoveryManifestEntry& entry,
                                     std::function<bool(float)> progress_cb = nullptr);

    // "Discard" action: deletes the artefact file and removes the manifest entry.
    // The caller must have already shown an inline confirmation before calling.
    RecoveryActionResult Discard(const RecoveryManifestEntry& entry);

  private:
    RecoveryManifestStore& store_;

    // Returns a collision-safe path derived from `preferred`. If preferred does
    // not exist it is returned as-is; otherwise tries "name (2).ext" ... "(N).ext".
    static std::filesystem::path ResolveUniqueOutputPath(const std::filesystem::path& preferred);
};

} // namespace exosnap
