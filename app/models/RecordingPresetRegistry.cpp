#include "RecordingPresetRegistry.h"

#include <algorithm>
#include <cassert>
#include <set>
#include <string>

namespace exosnap {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

RecordingPresetRegistry::RecordingPresetRegistry() {
    presets_.push_back(MakeDefaultPreset());
    selected_id_ = std::string(kDefaultPresetId);
    default_id_ = std::string(kDefaultPresetId);
}

// ---------------------------------------------------------------------------
// LoadState
// ---------------------------------------------------------------------------

void RecordingPresetRegistry::LoadState(std::vector<RecordingPreset> presets, std::string selected_id,
                                        std::string default_id) {
    // 1. Dedup ids: keep first occurrence; sanitize each accepted preset.
    std::vector<RecordingPreset> accepted;
    std::set<std::string> seen_ids;

    for (auto& p : presets) {
        if (p.id.empty()) {
            continue; // Caller should have set an id; skip empty-id items.
        }
        if (seen_ids.count(p.id) > 0) {
            continue; // Duplicate — drop later occurrence.
        }
        seen_ids.insert(p.id);
        accepted.push_back(SanitizePreset(std::move(p)));
    }

    // 2. If empty after dedup/sanitize, seed a fresh default.
    if (accepted.empty()) {
        accepted.push_back(MakeDefaultPreset());
        selected_id = std::string(kDefaultPresetId);
        default_id = std::string(kDefaultPresetId);
    }

    presets_ = std::move(accepted);

    // 3. Repair selected_id.
    const bool selected_valid = (IndexById(selected_id) != std::string::npos);
    if (!selected_valid) {
        // Try default_id as fallback.
        const bool default_valid = (IndexById(default_id) != std::string::npos);
        if (default_valid) {
            selected_id = default_id;
        } else {
            selected_id = presets_.front().id;
        }
    }
    selected_id_ = std::move(selected_id);

    // 4. Repair default_id.
    const bool default_valid = (IndexById(default_id) != std::string::npos);
    if (!default_valid) {
        // Prefer kDefaultPresetId if it is in the list.
        const bool canonical_present = (IndexById(kDefaultPresetId) != std::string::npos);
        if (canonical_present) {
            default_id = std::string(kDefaultPresetId);
        } else {
            default_id = presets_.front().id;
        }
    }
    default_id_ = std::move(default_id);
}

// ---------------------------------------------------------------------------
// Observers
// ---------------------------------------------------------------------------

const std::vector<RecordingPreset>& RecordingPresetRegistry::Presets() const noexcept {
    return presets_;
}

std::size_t RecordingPresetRegistry::Count() const noexcept {
    return presets_.size();
}

const std::string& RecordingPresetRegistry::SelectedId() const noexcept {
    return selected_id_;
}

const std::string& RecordingPresetRegistry::DefaultId() const noexcept {
    return default_id_;
}

const RecordingPreset* RecordingPresetRegistry::FindById(std::string_view id) const {
    const std::size_t idx = IndexById(id);
    if (idx == std::string::npos) {
        return nullptr;
    }
    return &presets_[idx];
}

const RecordingPreset& RecordingPresetRegistry::SelectedPreset() const {
    const RecordingPreset* p = FindById(selected_id_);
    assert(p != nullptr && "selected_id_ must always refer to a valid preset (invariant violation)");
    return *p;
}

// ---------------------------------------------------------------------------
// Selection / default
// ---------------------------------------------------------------------------

bool RecordingPresetRegistry::SetSelected(std::string id) {
    if (IndexById(id) == std::string::npos) {
        return false;
    }
    selected_id_ = std::move(id);
    return true;
}

bool RecordingPresetRegistry::SetDefault(std::string id) {
    if (IndexById(id) == std::string::npos) {
        return false;
    }
    default_id_ = std::move(id);
    return true;
}

// ---------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------

std::string RecordingPresetRegistry::AddPreset(RecordingPresetConfig config, const std::string& name) {
    RecordingPreset preset;
    preset.id = GeneratePresetId();
    preset.name = DeduplicateName(NormalizePresetName(name));
    if (preset.name.empty()) {
        preset.name = DeduplicateName("New preset");
    }
    preset.config = std::move(config);
    preset = SanitizePreset(std::move(preset));

    // Keep the deduped name (SanitizePreset may have changed it only if empty).
    const std::string id = preset.id;
    presets_.push_back(std::move(preset));
    selected_id_ = id;
    return id;
}

std::string RecordingPresetRegistry::AddDefaultPreset() {
    RecordingPreset preset = MakeDefaultPreset();
    preset.id = GeneratePresetId();
    preset.name = DeduplicateName("New preset");

    const std::string id = preset.id;
    presets_.push_back(std::move(preset));
    selected_id_ = id;
    return id;
}

void RecordingPresetRegistry::ImportPreset(RecordingPreset preset) {
    // The caller has already resolved id collisions.  Deduplicate the name only.
    preset.name = DeduplicateName(NormalizePresetName(preset.name));
    if (preset.name.empty()) {
        preset.name = DeduplicateName("Imported preset");
    }
    preset = SanitizePreset(std::move(preset));
    presets_.push_back(std::move(preset));
    // selected_id_ is intentionally NOT changed: the user selects explicitly.
}

bool RecordingPresetRegistry::SaveSelected(RecordingPresetConfig config) {
    const std::size_t idx = IndexById(selected_id_);
    if (idx == std::string::npos) {
        return false; // Should not happen given invariant.
    }
    presets_[idx].config = SanitizePresetConfig(std::move(config));
    return true;
}

std::string RecordingPresetRegistry::DuplicateSelected() {
    const RecordingPreset& src = SelectedPreset();

    RecordingPreset copy;
    copy.id = GeneratePresetId();
    copy.name = DeduplicateName(src.name + " (copy)");
    copy.config = src.config;

    const std::string id = copy.id;
    presets_.push_back(std::move(copy));
    selected_id_ = id;
    return id;
}

bool RecordingPresetRegistry::RenameSelected(const std::string& new_name) {
    const std::string normalized = NormalizePresetName(new_name);
    if (normalized.empty()) {
        return false;
    }

    // Check for duplicate name among OTHER presets (same-id is allowed to keep
    // its own name unchanged).
    for (const auto& p : presets_) {
        if (p.id != selected_id_ && NormalizePresetName(p.name) == normalized) {
            return false;
        }
    }

    const std::size_t idx = IndexById(selected_id_);
    if (idx == std::string::npos) {
        return false;
    }
    presets_[idx].name = normalized;
    return true;
}

bool RecordingPresetRegistry::DeleteSelected() {
    if (presets_.size() == 1) {
        return false;
    }

    const std::size_t del_idx = IndexById(selected_id_);
    if (del_idx == std::string::npos) {
        return false; // Invariant violation.
    }

    const bool was_default = (selected_id_ == default_id_);

    presets_.erase(presets_.begin() + static_cast<std::ptrdiff_t>(del_idx));

    // Determine the new selected id.
    // Preference: element AFTER the deleted position; else BEFORE; else index 0.
    std::size_t new_idx = 0;
    if (del_idx < presets_.size()) {
        new_idx = del_idx; // The next element is now at del_idx.
    } else {
        new_idx = del_idx - 1; // del_idx was the last element.
    }
    selected_id_ = presets_[new_idx].id;

    // Repair default if it pointed at the deleted preset.
    if (was_default) {
        // Prefer kDefaultPresetId if it still exists.
        const bool canonical_present = (IndexById(kDefaultPresetId) != std::string::npos);
        if (canonical_present) {
            default_id_ = std::string(kDefaultPresetId);
        } else {
            default_id_ = selected_id_;
        }
    }

    return true;
}

RecordingPresetConfig RecordingPresetRegistry::SelectedSavedConfig() const {
    return SelectedPreset().config;
}

void RecordingPresetRegistry::ResetAllToDefault() {
    presets_.clear();
    presets_.push_back(MakeDefaultPreset());
    selected_id_ = std::string(kDefaultPresetId);
    default_id_ = std::string(kDefaultPresetId);
}

bool RecordingPresetRegistry::IsSelectedDirty(const RecordingPresetConfig& live_config) const {
    // Use ConfigDirtyEquivalent (not NormalizedConfigEquals) so that capture
    // identity (display_key, window_key, region) does not contribute to dirty
    // state.  Capture is transient/auto-resolved and excluded from dirty per spec.
    return !ConfigDirtyEquivalent(live_config, SelectedPreset().config);
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

std::string RecordingPresetRegistry::DeduplicateName(const std::string& base) const {
    // Check if `base` is already in use.
    const auto name_exists = [&](const std::string& name) {
        for (const auto& p : presets_) {
            if (NormalizePresetName(p.name) == name) {
                return true;
            }
        }
        return false;
    };

    if (!name_exists(base)) {
        return base;
    }

    for (int suffix = 2;; ++suffix) {
        const std::string candidate = base + " (" + std::to_string(suffix) + ")";
        if (!name_exists(candidate)) {
            return candidate;
        }
    }
}

std::size_t RecordingPresetRegistry::IndexById(std::string_view id) const {
    for (std::size_t i = 0; i < presets_.size(); ++i) {
        if (presets_[i].id == id) {
            return i;
        }
    }
    return std::string::npos;
}

} // namespace exosnap
