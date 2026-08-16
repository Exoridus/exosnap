#include "RecordingPresetRegistry.h"

#include <algorithm>
#include <cassert>
#include <set>
#include <string>

namespace exosnap {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

RecordingPresetRegistry::RecordingPresetRegistry() : presets_(MakeBuiltInPresets()), selected_id_(kDefaultPresetId) {
}

// ---------------------------------------------------------------------------
// LoadState
// ---------------------------------------------------------------------------

void RecordingPresetRegistry::LoadState(std::vector<RecordingPreset> presets, std::string selected_id) {
    // 1. Seed the four built-ins first; they always win over any persisted
    //    copy of the same id (a stale/tampered built-in snapshot is ignored).
    presets_ = MakeBuiltInPresets();
    std::set<std::string> seen_ids;
    for (const auto& p : presets_) {
        seen_ids.insert(p.id);
    }

    // 2. Append the persisted user presets: drop empty/duplicate/built-in
    //    ids, sanitize, and dedupe names (fold-aware) against the built-ins
    //    and any already-accepted user preset.
    for (auto& p : presets) {
        if (p.id.empty()) {
            continue; // Caller should have set an id; skip empty-id items.
        }
        if (seen_ids.count(p.id) > 0) {
            continue; // Duplicate of a built-in or an earlier entry — drop.
        }
        seen_ids.insert(p.id);

        RecordingPreset sanitized = SanitizePreset(std::move(p));
        sanitized.name = DeduplicateName(sanitized.name);
        presets_.push_back(std::move(sanitized));
    }

    // 3. Repair selected_id: kDefaultPresetId is a built-in and therefore
    //    always present, so it is the only fallback selection ever needs.
    const bool selected_valid = (IndexById(selected_id) != std::string::npos);
    selected_id_ = selected_valid ? std::move(selected_id) : std::string(kDefaultPresetId);
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
    preset.config = StripEnvironmentFields(std::move(config));
    preset = SanitizePreset(std::move(preset));

    // Keep the deduped name (SanitizePreset may have changed it only if empty).
    // Not const: `id` is returned below and NRVO cannot apply (it is used in
    // between), so const would turn the return into a copy instead of a move.
    std::string id = preset.id;
    presets_.push_back(std::move(preset));
    selected_id_ = id;
    return id;
}

void RecordingPresetRegistry::ImportPreset(RecordingPreset preset) {
    // The caller has already resolved id collisions.  Deduplicate the name
    // (fold-aware, built-in names reserved) and strip environment fields (an
    // older export may still carry them).
    preset.name = DeduplicateName(NormalizePresetName(preset.name));
    if (preset.name.empty()) {
        preset.name = DeduplicateName("Imported preset");
    }
    preset.config = StripEnvironmentFields(std::move(preset.config));
    preset = SanitizePreset(std::move(preset));
    presets_.push_back(std::move(preset));
    // selected_id_ is intentionally NOT changed: the user selects explicitly.
}

bool RecordingPresetRegistry::RenameSelected(const std::string& new_name) {
    if (IsBuiltIn(selected_id_)) {
        return false;
    }

    const std::string normalized = NormalizePresetName(new_name);
    if (normalized.empty()) {
        return false;
    }

    if (IsNameTaken(normalized, selected_id_)) {
        return false;
    }

    const std::size_t idx = IndexById(selected_id_);
    if (idx == std::string::npos) {
        return false;
    }
    presets_[idx].name = normalized;
    return true;
}

bool RecordingPresetRegistry::DeleteSelected() {
    if (IsBuiltIn(selected_id_)) {
        return false;
    }

    const std::size_t del_idx = IndexById(selected_id_);
    if (del_idx == std::string::npos) {
        return false; // Invariant violation.
    }

    presets_.erase(presets_.begin() + static_cast<std::ptrdiff_t>(del_idx));

    // kDefaultPresetId is a built-in and therefore always present.
    selected_id_ = std::string(kDefaultPresetId);

    return true;
}

RecordingPresetConfig RecordingPresetRegistry::SelectedSavedConfig() const {
    return SelectedPreset().config;
}

bool RecordingPresetRegistry::IsSelectedDirty(const RecordingPresetConfig& live_config) const {
    // Use ConfigDirtyEquivalent (not NormalizedConfigEquals) so that capture
    // identity (display_key, window_key, region) does not contribute to dirty
    // state.  Capture is transient/auto-resolved and excluded from dirty per spec.
    return !ConfigDirtyEquivalent(live_config, SelectedPreset().config);
}

bool RecordingPresetRegistry::IsBuiltIn(std::string_view id) {
    return IsBuiltInPresetId(id);
}

bool RecordingPresetRegistry::IsNameTaken(std::string_view name, std::string_view exclude_id) const {
    const std::string folded = FoldPresetName(name);
    for (const auto& p : presets_) {
        if (!exclude_id.empty() && p.id == exclude_id) {
            continue;
        }
        if (FoldPresetName(p.name) == folded) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

std::string RecordingPresetRegistry::DeduplicateName(const std::string& base) const {
    // Check if `base` is already in use (trimmed + case-insensitive).
    const auto name_exists = [&](const std::string& folded_candidate) {
        for (const auto& p : presets_) {
            if (FoldPresetName(p.name) == folded_candidate) {
                return true;
            }
        }
        return false;
    };

    if (!name_exists(FoldPresetName(base))) {
        return base;
    }

    for (int suffix = 2;; ++suffix) {
        std::string candidate = base + " (" + std::to_string(suffix) + ")";
        if (!name_exists(FoldPresetName(candidate))) {
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
