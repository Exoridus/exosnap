#pragma once

#include "models/RecordingPreset.h"

#include <QString>
#include <QVector>

#include <optional>
#include <string>
#include <vector>

namespace exosnap {

// ---------------------------------------------------------------------------
// PersistedPresetState
// ---------------------------------------------------------------------------

struct PersistedPresetState {
    std::vector<RecordingPreset> user_presets; // built-ins are code-defined, never persisted
    std::string selected_id;                   // repaired to kDefaultPresetId when unknown
    std::optional<RecordingPresetConfig> live; // nullopt: [live] missing/unreadable -> boot Default
    // True when the file needed field-wise repair on load (parse failure,
    // schema mismatch, or a dropped/clamped item). False on a first run
    // (missing file) — there is nothing to repair yet.
    bool repaired = false;
};

// ---------------------------------------------------------------------------
// RecordingPresetStore
//
// Reads/writes the live config and named preset snapshots to a TOML file.
// Thread-safety: instances are not thread-safe; use from a single thread.
// ---------------------------------------------------------------------------

class RecordingPresetStore {
  public:
    // Default path: QStandardPaths::AppConfigLocation + "/presets.toml".
    RecordingPresetStore();

    // Explicit path — intended for tests.  An empty path causes Load() to
    // return defaults and Save() to be a no-op.
    explicit RecordingPresetStore(QString file_path);

    // Load the persisted state from the file.  A parse failure or schema
    // mismatch is repaired field by field instead of resetting the whole
    // file — see PersistedPresetState::repaired.  Individual malformed items
    // are silently skipped.
    [[nodiscard]] PersistedPresetState Load() const;

    // Persist the live config and the given (non-built-in) presets.  Built-in
    // ids are silently skipped — they are code-defined and never round-trip
    // through disk.  Creates parent directories as needed.
    // Empty file_path → no-op.
    void Save(const std::vector<RecordingPreset>& presets, const std::string& selected_id,
              const RecordingPresetConfig& live) const;

    [[nodiscard]] const QString& FilePath() const;

    // ---------------------------------------------------------------------------
    // Export / import helpers
    //
    // All three methods use the same IniFormat serialization as Save/Load so
    // there is exactly one serialization code path.  kPresetSchemaVersion is
    // embedded in every exported file so future Load() callers can reject
    // incompatible files.
    // ---------------------------------------------------------------------------

    // Write a single preset to a standalone .ini file.
    // Returns true on success; on failure writes a human-readable message into
    // *err (if non-null) and returns false.
    [[nodiscard]] static bool ExportPresetToFile(const RecordingPreset& preset, const QString& path, QString* err);

    // Write all given user presets to one .ini file using the same multi-item
    // array layout the live store uses for presets.ini.
    // Returns true on success.
    [[nodiscard]] static bool ExportAllUserPresetsToFile(const QVector<RecordingPreset>& presets, const QString& path,
                                                         QString* err);

    // Read one or more presets from a .ini file previously created by
    // ExportPresetToFile or ExportAllUserPresetsToFile.
    //
    // existing_ids: the caller supplies the current live preset ids so that
    // collision handling can assign fresh ids to any imported preset whose id
    // is already in use.  Name collisions are left to the registry's numeric
    // dedupe ("name (2)") at insert time — the name returned here is unchanged.
    //
    // On unrecoverable failure (file missing, garbage content, no valid
    // items) returns an empty vector and sets *err.
    // Schema version mismatch is treated as best-effort: the file is still
    // parsed and SanitizePreset is applied; if no valid items survive, *err is
    // set and an empty vector is returned.
    [[nodiscard]] static QVector<RecordingPreset>
    ImportPresetsFromFile(const QString& path, const std::vector<std::string>& existing_ids, QString* err);

  private:
    QString file_path_;
};

} // namespace exosnap
