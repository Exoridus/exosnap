#pragma once

#include "RecordingPreset.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace exosnap {

// ---------------------------------------------------------------------------
// RecordingPresetRegistry
//
// Qt-independent in-memory registry for the user's saved presets.
// Holds the saved list plus the currently selected and startup-default ids.
// The registry does NOT hold the live working config — callers pass the live
// RecordingPresetConfig explicitly to operations that compare or snapshot it.
//
// Invariants maintained at all times:
//   - presets_ is non-empty.
//   - selected_id_ refers to a preset in presets_.
//   - default_id_  refers to a preset in presets_.
// ---------------------------------------------------------------------------

class RecordingPresetRegistry {
  public:
    // Seeds presets_ with [MakeDefaultPreset()]; selected = default = kDefaultPresetId.
    RecordingPresetRegistry();

    // Replace state from a loaded snapshot, repairing invariants:
    //   - Dedup ids (keep first occurrence).
    //   - Sanitize each preset via SanitizePreset().
    //   - If presets empty after dedup/sanitize, seed a fresh default.
    //   - Repair selected_id / default_id if they point to non-existent presets.
    void LoadState(std::vector<RecordingPreset> presets, std::string selected_id, std::string default_id);

    // -----------------------------------------------------------------------
    // Observers
    // -----------------------------------------------------------------------

    [[nodiscard]] const std::vector<RecordingPreset>& Presets() const noexcept;
    [[nodiscard]] std::size_t Count() const noexcept;
    [[nodiscard]] const std::string& SelectedId() const noexcept;
    [[nodiscard]] const std::string& DefaultId() const noexcept;
    [[nodiscard]] const RecordingPreset* FindById(std::string_view id) const;
    [[nodiscard]] const RecordingPreset& SelectedPreset() const; // always valid (invariant)

    // -----------------------------------------------------------------------
    // Selection / default
    // -----------------------------------------------------------------------

    // Returns false if id is not found.
    bool SetSelected(std::string id);

    // Returns false if id is not found.
    bool SetDefault(std::string id);

    // -----------------------------------------------------------------------
    // Mutations
    // -----------------------------------------------------------------------

    // Sanitize config, dedup name, generate new id, select the new preset,
    // return its id.
    std::string AddPreset(RecordingPresetConfig config, const std::string& name);

    // Adds a fresh MakeDefaultPreset() config under a unique name ("New preset"),
    // new id, selects it, returns its id.
    std::string AddDefaultPreset();

    // Overwrite the SELECTED preset's config with the supplied (sanitized) config;
    // id and name are unchanged.  Returns false if nothing is selected (impossible
    // via invariant, but provided for defensive use).
    bool SaveSelected(RecordingPresetConfig config);

    // Copy the selected preset's SAVED config to a new preset with a deduped
    // "<name> (copy)" name, selects the copy, returns its id.
    std::string DuplicateSelected();

    // Rename the selected preset.  Returns false for empty/whitespace names or
    // when the normalized name already exists among other presets.
    bool RenameSelected(const std::string& new_name);

    // Remove the selected preset.  Returns false when Count()==1.
    // Fallback selection: the element AFTER the deleted one; if none, the one
    // BEFORE; if neither (impossible given Count>1), index 0.
    // If the deleted preset was the default, default falls back to
    // kDefaultPresetId if present, else the new selected id.
    bool DeleteSelected();

    // Returns the selected preset's saved config (for "Reset changes").
    [[nodiscard]] RecordingPresetConfig SelectedSavedConfig() const;

    // Clear to a single MakeDefaultPreset(); selected = default = kDefaultPresetId.
    void ResetAllToDefault();

    // Returns true when live_config differs from the selected preset's saved config
    // (!NormalizedConfigEquals).
    [[nodiscard]] bool IsSelectedDirty(const RecordingPresetConfig& live_config) const;

  private:
    // Returns a name that does not collide with any existing preset name.
    // If `base` exists, tries "base (2)", "base (3)", ... until free.
    [[nodiscard]] std::string DeduplicateName(const std::string& base) const;

    // Finds the index of the preset with `id` in presets_, or npos-equivalent.
    [[nodiscard]] std::size_t IndexById(std::string_view id) const;

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    std::vector<RecordingPreset> presets_;
    std::string selected_id_;
    std::string default_id_;
};

} // namespace exosnap
