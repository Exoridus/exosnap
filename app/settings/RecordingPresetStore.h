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
    std::vector<RecordingPreset> presets;
    std::string selected_id;
    std::string default_id;
    // True when defaults had to be seeded or repaired (caller should log one
    // Warning and immediately re-save to purge the bad data).
    bool was_reset = false;
};

// ---------------------------------------------------------------------------
// RecordingPresetStore
//
// Reads/writes the preset list to a QSettings IniFormat file.
// Thread-safety: instances are not thread-safe; use from a single thread.
// ---------------------------------------------------------------------------

class RecordingPresetStore {
  public:
    // Default path: QStandardPaths::AppConfigLocation + "/presets.ini".
    RecordingPresetStore();

    // Explicit path — intended for tests.  An empty path causes Load() to
    // return a seeded default and Save() to be a no-op.
    explicit RecordingPresetStore(QString file_path);

    // Load the preset state from the file.
    // On any parse failure or version mismatch, returns a seeded default with
    // was_reset=true.  Individual malformed items are silently skipped.
    [[nodiscard]] PersistedPresetState Load() const;

    // Persist the preset state.  Creates parent directories as needed.
    // Empty file_path → no-op.
    void Save(const std::vector<RecordingPreset>& presets, const std::string& selected_id,
              const std::string& default_id) const;

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
    // is already in use.  The imported preset's name is also suffixed with
    // " (imported)" on collision so the user can tell it apart.
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
