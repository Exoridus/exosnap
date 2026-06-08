#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <capability/audio_ui_state.h>
#include <recorder_core/audio_track_model.h>
#include <recorder_core/recorder_session.h>

#include "models/RecordingPreset.h"
#include "models/RecordingPresetRegistry.h"

namespace exosnap {
namespace {

// ===========================================================================
// Helpers
// ===========================================================================

// Returns a RecordingPresetConfig that is distinguishable from the default.
RecordingPresetConfig MakeDistinctConfig() {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.countdown_seconds = 3;
    cfg.video.quality = recorder_core::NvencQualityPreset::Small;
    return cfg;
}

// ===========================================================================
// Constructor
// ===========================================================================

TEST(RecordingPresetRegistry, Constructor_SeedsDefault) {
    RecordingPresetRegistry reg;
    EXPECT_EQ(reg.Count(), 1u);
    EXPECT_EQ(reg.SelectedId(), std::string(kDefaultPresetId));
    EXPECT_EQ(reg.DefaultId(), std::string(kDefaultPresetId));
    EXPECT_EQ(reg.SelectedPreset().id, std::string(kDefaultPresetId));
}

// ===========================================================================
// AddPreset / AddDefaultPreset
// ===========================================================================

TEST(RecordingPresetRegistry, AddPreset_CreatesNewId_SelectsIt_CountGrows) {
    RecordingPresetRegistry reg;
    const std::string id = reg.AddPreset(MakeDistinctConfig(), "Custom Preset");
    EXPECT_EQ(reg.Count(), 2u);
    EXPECT_EQ(reg.SelectedId(), id);
    EXPECT_NE(id, std::string(kDefaultPresetId));
    // id starts with "preset."
    EXPECT_EQ(id.substr(0, 7), "preset.");
}

TEST(RecordingPresetRegistry, AddDefaultPreset_CreatesNewId_SelectsIt_CountGrows) {
    RecordingPresetRegistry reg;
    const std::string id = reg.AddDefaultPreset();
    EXPECT_EQ(reg.Count(), 2u);
    EXPECT_EQ(reg.SelectedId(), id);
    EXPECT_NE(id, std::string(kDefaultPresetId));
}

TEST(RecordingPresetRegistry, AddDefaultPreset_NameDedup) {
    RecordingPresetRegistry reg;
    const std::string id1 = reg.AddDefaultPreset(); // "New preset"
    const std::string id2 = reg.AddDefaultPreset(); // "New preset (2)"

    const RecordingPreset* p1 = reg.FindById(id1);
    const RecordingPreset* p2 = reg.FindById(id2);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(p1->name, "New preset");
    EXPECT_EQ(p2->name, "New preset (2)");
}

TEST(RecordingPresetRegistry, AddPreset_NameDedup_Suffix2Then3) {
    RecordingPresetRegistry reg;
    reg.AddPreset(MakeDefaultPreset().config, "Alpha");
    reg.AddPreset(MakeDefaultPreset().config, "Alpha");
    reg.AddPreset(MakeDefaultPreset().config, "Alpha");
    // Names should be "Alpha", "Alpha (2)", "Alpha (3)"
    bool found_alpha = false, found_alpha2 = false, found_alpha3 = false;
    for (const auto& p : reg.Presets()) {
        if (p.name == "Alpha")
            found_alpha = true;
        if (p.name == "Alpha (2)")
            found_alpha2 = true;
        if (p.name == "Alpha (3)")
            found_alpha3 = true;
    }
    EXPECT_TRUE(found_alpha);
    EXPECT_TRUE(found_alpha2);
    EXPECT_TRUE(found_alpha3);
}

// ===========================================================================
// SaveSelected
// ===========================================================================

TEST(RecordingPresetRegistry, SaveSelected_OverwritesSameId_ConfigChanges_IdStable_CountUnchanged) {
    RecordingPresetRegistry reg;
    const std::string orig_id = reg.SelectedId();
    const std::size_t orig_count = reg.Count();

    RecordingPresetConfig new_cfg = MakeDistinctConfig();
    EXPECT_TRUE(reg.SaveSelected(new_cfg));
    EXPECT_EQ(reg.Count(), orig_count);
    EXPECT_EQ(reg.SelectedId(), orig_id);
    EXPECT_TRUE(NormalizedConfigEquals(reg.SelectedPreset().config, new_cfg));
}

// ===========================================================================
// DuplicateSelected
// ===========================================================================

TEST(RecordingPresetRegistry, DuplicateSelected_NewId_CopyNameDeduped_ConfigEqual_SelectsCopy) {
    RecordingPresetRegistry reg;
    const std::string src_id = reg.SelectedId();
    const RecordingPresetConfig src_cfg = reg.SelectedPreset().config;
    const std::string src_name = reg.SelectedPreset().name;

    const std::string copy_id = reg.DuplicateSelected();
    EXPECT_NE(copy_id, src_id);
    EXPECT_EQ(reg.SelectedId(), copy_id);
    EXPECT_EQ(reg.Count(), 2u);

    const RecordingPreset* copy = reg.FindById(copy_id);
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->name, src_name + " (copy)");
    EXPECT_TRUE(NormalizedConfigEquals(copy->config, src_cfg));
}

TEST(RecordingPresetRegistry, DuplicateSelected_CopyNameDedup_WhenCopyAlreadyExists) {
    RecordingPresetRegistry reg;
    const std::string src_name = reg.SelectedPreset().name;
    reg.DuplicateSelected(); // "Default (copy)"
    reg.SetSelected(std::string(kDefaultPresetId));
    reg.DuplicateSelected(); // should become "Default (copy) (2)"

    bool found_copy = false, found_copy2 = false;
    for (const auto& p : reg.Presets()) {
        if (p.name == src_name + " (copy)")
            found_copy = true;
        if (p.name == src_name + " (copy) (2)")
            found_copy2 = true;
    }
    EXPECT_TRUE(found_copy);
    EXPECT_TRUE(found_copy2);
}

// ===========================================================================
// RenameSelected
// ===========================================================================

TEST(RecordingPresetRegistry, RenameSelected_EmptyName_ReturnsFalse_NameUnchanged) {
    RecordingPresetRegistry reg;
    const std::string old_name = reg.SelectedPreset().name;
    EXPECT_FALSE(reg.RenameSelected(""));
    EXPECT_EQ(reg.SelectedPreset().name, old_name);
}

TEST(RecordingPresetRegistry, RenameSelected_WhitespaceOnlyName_ReturnsFalse) {
    RecordingPresetRegistry reg;
    const std::string old_name = reg.SelectedPreset().name;
    EXPECT_FALSE(reg.RenameSelected("   "));
    EXPECT_EQ(reg.SelectedPreset().name, old_name);
}

TEST(RecordingPresetRegistry, RenameSelected_DuplicateNameOfOtherPreset_ReturnsFalse) {
    RecordingPresetRegistry reg;
    const std::string id2 = reg.AddDefaultPreset(); // "New preset"
    reg.SetSelected(std::string(kDefaultPresetId));

    // Try to rename the default preset to "New preset" (already taken by id2).
    EXPECT_FALSE(reg.RenameSelected("New preset"));
    // Name of default unchanged.
    EXPECT_NE(reg.SelectedPreset().name, "New preset");
    (void)id2;
}

TEST(RecordingPresetRegistry, RenameSelected_ValidUniqueName_ReturnsTrue) {
    RecordingPresetRegistry reg;
    EXPECT_TRUE(reg.RenameSelected("My New Name"));
    EXPECT_EQ(reg.SelectedPreset().name, "My New Name");
}

TEST(RecordingPresetRegistry, RenameSelected_SameNameAsSelected_ReturnsTrue) {
    RecordingPresetRegistry reg;
    const std::string current_name = reg.SelectedPreset().name;
    // Renaming to the same name is allowed.
    EXPECT_TRUE(reg.RenameSelected(current_name));
    EXPECT_EQ(reg.SelectedPreset().name, current_name);
}

// ===========================================================================
// DeleteSelected
// ===========================================================================

TEST(RecordingPresetRegistry, DeleteSelected_Count1_ReturnsFalse) {
    RecordingPresetRegistry reg;
    EXPECT_EQ(reg.Count(), 1u);
    EXPECT_FALSE(reg.DeleteSelected());
    EXPECT_EQ(reg.Count(), 1u);
}

TEST(RecordingPresetRegistry, DeleteSelected_With2Presets_RemovesSelected_SelectsFallback) {
    RecordingPresetRegistry reg;
    const std::string id2 = reg.AddDefaultPreset();
    EXPECT_EQ(reg.Count(), 2u);
    EXPECT_EQ(reg.SelectedId(), id2); // AddDefaultPreset selects new one.

    EXPECT_TRUE(reg.DeleteSelected());
    EXPECT_EQ(reg.Count(), 1u);
    EXPECT_EQ(reg.SelectedId(), std::string(kDefaultPresetId));
}

TEST(RecordingPresetRegistry, DeleteSelected_DefaultPreset_DefaultFallsBack) {
    RecordingPresetRegistry reg;
    const std::string id2 = reg.AddDefaultPreset();
    reg.SetSelected(std::string(kDefaultPresetId));

    // Now selected == kDefaultPresetId == DefaultId.
    EXPECT_EQ(reg.DefaultId(), std::string(kDefaultPresetId));
    EXPECT_TRUE(reg.DeleteSelected());

    // Default should have fallen back.
    EXPECT_NE(reg.DefaultId(), std::string(kDefaultPresetId));
    // Default is still a valid id.
    EXPECT_NE(reg.FindById(reg.DefaultId()), nullptr);
    (void)id2;
}

TEST(RecordingPresetRegistry, DeleteSelected_FallbackToNextElement) {
    RecordingPresetRegistry reg;
    // Two extra presets; select the first (kDefaultPresetId).
    const std::string id2 = reg.AddDefaultPreset(); // index 1
    const std::string id3 = reg.AddDefaultPreset(); // index 2
    reg.SetSelected(std::string(kDefaultPresetId)); // select index 0

    EXPECT_TRUE(reg.DeleteSelected());
    // Index 0 is gone; element that was at index 1 (id2) is now at index 0.
    EXPECT_EQ(reg.SelectedId(), id2);
    (void)id3;
}

TEST(RecordingPresetRegistry, DeleteSelected_FallbackToPreviousWhenNoNext) {
    RecordingPresetRegistry reg;
    const std::string id2 = reg.AddDefaultPreset(); // index 1
    // id2 is selected; it is the last element → fallback = previous = kDefaultPresetId.
    EXPECT_EQ(reg.SelectedId(), id2);
    EXPECT_TRUE(reg.DeleteSelected());
    EXPECT_EQ(reg.SelectedId(), std::string(kDefaultPresetId));
}

// ===========================================================================
// SetDefault
// ===========================================================================

TEST(RecordingPresetRegistry, SetDefault_InvalidId_ReturnsFalse) {
    RecordingPresetRegistry reg;
    EXPECT_FALSE(reg.SetDefault("preset.doesnotexist1234567890"));
}

TEST(RecordingPresetRegistry, SetDefault_ValidId_ReturnsTrueAndPersists) {
    RecordingPresetRegistry reg;
    const std::string id2 = reg.AddDefaultPreset();
    EXPECT_TRUE(reg.SetDefault(id2));
    EXPECT_EQ(reg.DefaultId(), id2);
}

// ===========================================================================
// SelectedSavedConfig
// ===========================================================================

TEST(RecordingPresetRegistry, SelectedSavedConfig_ReturnsSelectedPresetsConfig) {
    RecordingPresetRegistry reg;
    const RecordingPresetConfig saved_cfg = reg.SelectedPreset().config;
    EXPECT_TRUE(NormalizedConfigEquals(reg.SelectedSavedConfig(), saved_cfg));
}

// ===========================================================================
// ResetAllToDefault
// ===========================================================================

TEST(RecordingPresetRegistry, ResetAllToDefault_Count1_IdsReset) {
    RecordingPresetRegistry reg;
    reg.AddDefaultPreset();
    reg.AddDefaultPreset();
    EXPECT_EQ(reg.Count(), 3u);

    reg.ResetAllToDefault();
    EXPECT_EQ(reg.Count(), 1u);
    EXPECT_EQ(reg.SelectedId(), std::string(kDefaultPresetId));
    EXPECT_EQ(reg.DefaultId(), std::string(kDefaultPresetId));
}

// ===========================================================================
// IsSelectedDirty
// ===========================================================================

TEST(RecordingPresetRegistry, IsSelectedDirty_FalseWhenLiveEqualsSelected) {
    RecordingPresetRegistry reg;
    const RecordingPresetConfig live = reg.SelectedSavedConfig();
    EXPECT_FALSE(reg.IsSelectedDirty(live));
}

TEST(RecordingPresetRegistry, IsSelectedDirty_TrueAfterMutatingLive) {
    RecordingPresetRegistry reg;
    RecordingPresetConfig live = reg.SelectedSavedConfig();
    live.countdown_seconds = 5; // Mutate live config.
    EXPECT_TRUE(reg.IsSelectedDirty(live));
}

TEST(RecordingPresetRegistry, IsSelectedDirty_FalseAfterLiveResetToSaved) {
    RecordingPresetRegistry reg;
    RecordingPresetConfig live = reg.SelectedSavedConfig();
    live.countdown_seconds = 5; // Mutate live config.
    EXPECT_TRUE(reg.IsSelectedDirty(live));

    // Reset live to saved.
    live = reg.SelectedSavedConfig();
    EXPECT_FALSE(reg.IsSelectedDirty(live));
}

// ===========================================================================
// LoadState — repair invariants
// ===========================================================================

TEST(RecordingPresetRegistry, LoadState_EmptyList_SeedsDefault) {
    RecordingPresetRegistry reg;
    reg.LoadState({}, "", "");
    EXPECT_EQ(reg.Count(), 1u);
    EXPECT_EQ(reg.SelectedId(), std::string(kDefaultPresetId));
    EXPECT_EQ(reg.DefaultId(), std::string(kDefaultPresetId));
}

TEST(RecordingPresetRegistry, LoadState_InvalidSelectedFallsToDefault) {
    std::vector<RecordingPreset> presets;
    presets.push_back(MakeDefaultPreset());

    RecordingPresetRegistry reg;
    reg.LoadState(presets, "preset.doesnotexist1234", std::string(kDefaultPresetId));
    // selected should fall back to kDefaultPresetId.
    EXPECT_EQ(reg.SelectedId(), std::string(kDefaultPresetId));
}

TEST(RecordingPresetRegistry, LoadState_InvalidDefaultFallsToFirst) {
    std::vector<RecordingPreset> presets;
    RecordingPreset p;
    p.id = "preset.abc1234567890abcd";
    p.name = "A";
    presets.push_back(p);

    RecordingPresetRegistry reg;
    reg.LoadState(presets, "preset.abc1234567890abcd", "preset.doesnotexist1234");
    // No kDefaultPresetId in list; default should fall back to first preset.
    EXPECT_EQ(reg.DefaultId(), "preset.abc1234567890abcd");
}

TEST(RecordingPresetRegistry, LoadState_DuplicateIds_Deduped) {
    std::vector<RecordingPreset> presets;
    RecordingPreset p1;
    p1.id = "preset.aabbccddeeff0011";
    p1.name = "First";
    RecordingPreset p2;
    p2.id = "preset.aabbccddeeff0011"; // Same id — duplicate.
    p2.name = "Second";
    presets.push_back(p1);
    presets.push_back(p2);

    RecordingPresetRegistry reg;
    reg.LoadState(presets, "preset.aabbccddeeff0011", "preset.aabbccddeeff0011");
    EXPECT_EQ(reg.Count(), 1u);
    EXPECT_EQ(reg.SelectedPreset().name, "First"); // First kept.
}

TEST(RecordingPresetRegistry, LoadState_EachPresetSanitized) {
    std::vector<RecordingPreset> presets;
    RecordingPreset p;
    p.id = "preset.aabbccddeeff0011";
    p.name = "  Trimmed  ";          // Should be trimmed.
    p.config.countdown_seconds = 99; // Invalid, should be sanitized to 0.
    presets.push_back(p);

    RecordingPresetRegistry reg;
    reg.LoadState(presets, "preset.aabbccddeeff0011", "preset.aabbccddeeff0011");
    ASSERT_EQ(reg.Count(), 1u);
    EXPECT_EQ(reg.SelectedPreset().name, "Trimmed");
    EXPECT_EQ(reg.SelectedPreset().config.countdown_seconds, 0);
}

} // namespace
} // namespace exosnap
