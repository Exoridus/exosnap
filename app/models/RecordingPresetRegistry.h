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
// Holds the saved list plus the currently selected preset id. The registry
// does NOT hold the live working config — callers pass the live
// RecordingPresetConfig explicitly to operations that compare or snapshot it.
// There is no startup-default preset: the live config is the persisted
// truth, and kDefaultPresetId (the built-in Default, always present) is the
// only fallback selection ever needs.
//
// Invariants maintained at all times:
//   - presets_ is non-empty.
//   - selected_id_ refers to a preset in presets_.
// ---------------------------------------------------------------------------

class RecordingPresetRegistry {
  public:
    // Seeds presets_ with MakeBuiltInPresets(); selected = kDefaultPresetId.
    RecordingPresetRegistry();

    // Replace state from a loaded snapshot, repairing invariants:
    //   - The four built-ins are always seeded first; a persisted preset whose
    //     id matches a built-in id is dropped (the built-in wins).
    //   - Dedup ids among the remaining (user) presets (keep first occurrence).
    //   - Sanitize each accepted preset via SanitizePreset(); names colliding
    //     with a built-in or an already-accepted name are deduped ("(2)", "(3)").
    //   - Repair selected_id to kDefaultPresetId if it points to a non-existent
    //     preset (kDefaultPresetId is a built-in and therefore always present).
    void LoadState(std::vector<RecordingPreset> presets, std::string selected_id);

    // -----------------------------------------------------------------------
    // Observers
    // -----------------------------------------------------------------------

    [[nodiscard]] const std::vector<RecordingPreset>& Presets() const noexcept;
    [[nodiscard]] std::size_t Count() const noexcept;
    [[nodiscard]] const std::string& SelectedId() const noexcept;
    [[nodiscard]] const RecordingPreset* FindById(std::string_view id) const;
    [[nodiscard]] const RecordingPreset& SelectedPreset() const; // always valid (invariant)

    // -----------------------------------------------------------------------
    // Selection
    // -----------------------------------------------------------------------

    // Returns false if id is not found.
    bool SetSelected(std::string id);

    // -----------------------------------------------------------------------
    // Mutations
    // -----------------------------------------------------------------------

    // Strips environment fields from `config`, dedups name, generates a new
    // id, selects the new preset, returns its id.
    std::string AddPreset(RecordingPresetConfig config, const std::string& name);

    // Rename the selected preset. Returns false for a built-in preset, for
    // empty/whitespace names, or when the folded name already names another
    // preset (built-in names are reserved).
    bool RenameSelected(const std::string& new_name);

    // Remove the selected preset. Returns false for a built-in preset.
    // Selection always falls back to kDefaultPresetId (always present).
    bool DeleteSelected();

    // Returns the selected preset's saved config (for "Reset changes").
    [[nodiscard]] RecordingPresetConfig SelectedSavedConfig() const;

    // Insert a fully-formed preset produced by the import flow. Environment
    // fields are stripped (an older export may still carry them).
    // The id must already be unique (caller is responsible for collision resolution).
    // The name is deduplicated (fold-aware) if it collides with an existing
    // preset name, including built-in names.
    // The newly imported preset is NOT auto-selected (unlike AddPreset).
    void ImportPreset(RecordingPreset preset);

    // Returns true when live_config differs from the selected preset's saved config
    // (!NormalizedConfigEquals).
    [[nodiscard]] bool IsSelectedDirty(const RecordingPresetConfig& live_config) const;

    // True when `id` names one of the shipped read-only presets.
    [[nodiscard]] static bool IsBuiltIn(std::string_view id);

    // True when a preset other than `exclude_id` already uses `name`
    // (trimmed, case-insensitive). Built-in names are always taken.
    [[nodiscard]] bool IsNameTaken(std::string_view name, std::string_view exclude_id = {}) const;

  private:
    // Returns a name that does not collide with any existing preset name
    // (fold-aware — see FoldPresetName). If `base` exists, tries "base (2)",
    // "base (3)", ... until free.
    [[nodiscard]] std::string DeduplicateName(const std::string& base) const;

    // Finds the index of the preset with `id` in presets_, or npos-equivalent.
    [[nodiscard]] std::size_t IndexById(std::string_view id) const;

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    std::vector<RecordingPreset> presets_;
    std::string selected_id_;
};

} // namespace exosnap
