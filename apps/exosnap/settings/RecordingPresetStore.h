#pragma once

#include "models/RecordingPreset.h"

#include <QString>

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

  private:
    QString file_path_;
};

} // namespace exosnap
