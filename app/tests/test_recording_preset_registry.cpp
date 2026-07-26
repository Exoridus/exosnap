#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>
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
    cfg.video.cq = recorder_core::CanonicalCq(recorder_core::QualityPreset::Efficient);
    return cfg;
}

// ===========================================================================
// Constructor
// ===========================================================================

TEST(RecordingPresetRegistry, Constructor_SeedsDefault) {
    RecordingPresetRegistry reg;
    EXPECT_EQ(reg.Count(), 4u);
    EXPECT_EQ(reg.SelectedId(), std::string(kDefaultPresetId));
    EXPECT_EQ(reg.SelectedPreset().id, std::string(kDefaultPresetId));
}

// ===========================================================================
// AddPreset
// ===========================================================================

TEST(RecordingPresetRegistry, AddPreset_CreatesNewId_SelectsIt_CountGrows) {
    RecordingPresetRegistry reg;
    const std::string id = reg.AddPreset(MakeDistinctConfig(), "Custom Preset");
    EXPECT_EQ(reg.Count(), 5u);
    EXPECT_EQ(reg.SelectedId(), id);
    EXPECT_NE(id, std::string(kDefaultPresetId));
    // id starts with "preset."
    EXPECT_EQ(id.substr(0, 7), "preset.");
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
    const std::string id1 = reg.AddPreset(MakeDefaultPreset().config, "Streaming");
    const std::string id2 = reg.AddPreset(MakeDefaultPreset().config, "Other");
    ASSERT_TRUE(reg.SetSelected(id2));

    // Try to rename id2 to "Streaming" (already taken by id1).
    EXPECT_FALSE(reg.RenameSelected("Streaming"));
    // Name of id2 unchanged.
    EXPECT_NE(reg.SelectedPreset().name, "Streaming");
    (void)id1;
}

TEST(RecordingPresetRegistry, RenameSelected_ValidUniqueName_ReturnsTrue) {
    RecordingPresetRegistry reg;
    reg.AddPreset(MakeDefaultPreset().config, "Mine"); // selects a non-built-in preset
    EXPECT_TRUE(reg.RenameSelected("My New Name"));
    EXPECT_EQ(reg.SelectedPreset().name, "My New Name");
}

TEST(RecordingPresetRegistry, RenameSelected_SameNameAsSelected_ReturnsTrue) {
    RecordingPresetRegistry reg;
    reg.AddPreset(MakeDefaultPreset().config, "Mine"); // selects a non-built-in preset
    const std::string current_name = reg.SelectedPreset().name;
    // Renaming to the same name is allowed.
    EXPECT_TRUE(reg.RenameSelected(current_name));
    EXPECT_EQ(reg.SelectedPreset().name, current_name);
}

// ===========================================================================
// DeleteSelected
// ===========================================================================

TEST(RecordingPresetRegistry, DeleteSelected_DefaultBuiltInSelected_ReturnsFalse) {
    RecordingPresetRegistry reg;
    EXPECT_EQ(reg.Count(), 4u);
    EXPECT_FALSE(reg.DeleteSelected());
    EXPECT_EQ(reg.Count(), 4u);
}

TEST(RecordingPresetRegistry, DeleteSelected_UserPreset_RemovesIt_FallsBackToDefault) {
    RecordingPresetRegistry reg;
    const std::string id2 = reg.AddPreset(MakeDefaultPreset().config, "Mine");
    EXPECT_EQ(reg.Count(), 5u);
    EXPECT_EQ(reg.SelectedId(), id2); // AddPreset selects the new one.

    EXPECT_TRUE(reg.DeleteSelected());
    EXPECT_EQ(reg.Count(), 4u);
    EXPECT_EQ(reg.SelectedId(), std::string(kDefaultPresetId));
    EXPECT_EQ(reg.FindById(id2), nullptr);
}

// Undo restores the previous selection; when that preset has since been
// deleted, the selection falls to Default (the fallback the dispatch uses).
TEST(RecordingPresetRegistry, UndoSelectionFallback_MissingPrevious_FallsToDefault) {
    RecordingPresetRegistry reg;
    const std::string mine = reg.AddPreset(MakeDefaultPreset().config, "Mine");
    ASSERT_TRUE(reg.SetSelected(std::string(kQualityPresetId)));
    ASSERT_TRUE(reg.SetSelected(mine)); // undo target exists -> restored
    ASSERT_TRUE(reg.DeleteSelected());  // now it is gone; selection sits on Default
    EXPECT_FALSE(reg.SetSelected(mine));
    EXPECT_EQ(reg.SelectedId(), kDefaultPresetId);
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

// Capture identity mutations must NOT make the preset dirty (per spec).
TEST(RecordingPresetRegistry, IsSelectedDirty_MutatingCapturKind_NotDirty) {
    RecordingPresetRegistry reg;
    RecordingPresetConfig live = reg.SelectedSavedConfig();
    live.capture.kind = PresetCaptureKind::Window;
    EXPECT_FALSE(reg.IsSelectedDirty(live));
}

TEST(RecordingPresetRegistry, IsSelectedDirty_MutatingCaptureDisplayKey_NotDirty) {
    RecordingPresetRegistry reg;
    RecordingPresetConfig live = reg.SelectedSavedConfig();
    live.capture.display_id.device_path = "MONITOR-001"; // Resolved on startup
    EXPECT_FALSE(reg.IsSelectedDirty(live));
}

TEST(RecordingPresetRegistry, IsSelectedDirty_MutatingCaptureRegion_NotDirty) {
    RecordingPresetRegistry reg;
    RecordingPresetConfig live = reg.SelectedSavedConfig();
    live.capture.kind = PresetCaptureKind::Region;
    live.capture.has_region = true;
    live.capture.region_x_norm = 0.1f;
    live.capture.region_y_norm = 0.1f;
    live.capture.region_w_norm = 0.5f;
    live.capture.region_h_norm = 0.5f;
    live.capture.region_display_id.device_path = "\\\\?\\DISPLAY#SAM#001";
    EXPECT_FALSE(reg.IsSelectedDirty(live));
}

// Non-capture mutations MUST make the preset dirty.
TEST(RecordingPresetRegistry, IsSelectedDirty_MutatingAudio_IsDirty) {
    RecordingPresetRegistry reg;
    RecordingPresetConfig live = reg.SelectedSavedConfig();
    // Flip the SystemOutput row from enabled to disabled.
    live.audio.source_rows[0].enabled = false;
    EXPECT_TRUE(reg.IsSelectedDirty(live));
}

TEST(RecordingPresetRegistry, IsSelectedDirty_MutatingVideo_IsDirty) {
    RecordingPresetRegistry reg;
    RecordingPresetConfig live = reg.SelectedSavedConfig();
    live.video.cq = recorder_core::CanonicalCq(recorder_core::QualityPreset::Efficient);
    EXPECT_TRUE(reg.IsSelectedDirty(live));
}

TEST(RecordingPresetRegistry, IsSelectedDirty_MutatingWebcam_IsDirty) {
    RecordingPresetRegistry reg;
    RecordingPresetConfig live = reg.SelectedSavedConfig();
    live.webcam.mirror = !live.webcam.mirror;
    EXPECT_TRUE(reg.IsSelectedDirty(live));
}

TEST(RecordingPresetRegistry, IsSelectedDirty_MutatingWebcamOpacity_IsDirty) {
    RecordingPresetRegistry reg;
    RecordingPresetConfig live = reg.SelectedSavedConfig();
    live.webcam.opacity = 0.5f;
    EXPECT_TRUE(reg.IsSelectedDirty(live));
}

TEST(RecordingPresetRegistry, IsSelectedDirty_MutatingOutput_IsDirty) {
    RecordingPresetRegistry reg;
    RecordingPresetConfig live = reg.SelectedSavedConfig();
    live.output.container = capability::Container::Mp4;
    ReconcileContainerCodecs(live.output);
    EXPECT_TRUE(reg.IsSelectedDirty(live));
}

TEST(RecordingPresetRegistry, IsSelectedDirty_RevertingCaptureMutation_StillClean) {
    RecordingPresetRegistry reg;
    RecordingPresetConfig live = reg.SelectedSavedConfig();
    // Mutate capture (simulates startup resolution).
    live.capture.display_id.device_path = "MONITOR-001";
    EXPECT_FALSE(reg.IsSelectedDirty(live));
    // Mutate a real field.
    live.countdown_seconds = 10;
    EXPECT_TRUE(reg.IsSelectedDirty(live));
    // Revert the real field.
    live.countdown_seconds = reg.SelectedSavedConfig().countdown_seconds;
    EXPECT_FALSE(reg.IsSelectedDirty(live));
}

// ===========================================================================
// LoadState — repair invariants
// ===========================================================================

TEST(RecordingPresetRegistry, LoadState_EmptyList_SeedsDefault) {
    RecordingPresetRegistry reg;
    reg.LoadState({}, "");
    EXPECT_EQ(reg.Count(), 4u);
    EXPECT_EQ(reg.SelectedId(), std::string(kDefaultPresetId));
}

TEST(RecordingPresetRegistry, LoadState_InvalidSelectedFallsToDefault) {
    std::vector<RecordingPreset> presets;
    presets.push_back(MakeDefaultPreset());

    RecordingPresetRegistry reg;
    reg.LoadState(presets, "preset.doesnotexist1234");
    // selected should fall back to kDefaultPresetId.
    EXPECT_EQ(reg.SelectedId(), std::string(kDefaultPresetId));
}

// Case (c) of the spec: the selected preset vanished -> selection falls to
// Default; live config (held by MainWindow) is untouched and now compares
// against Default, typically showing (changed).
TEST(RecordingPresetRegistry, LoadState_SelectedMissing_FallsToDefault) {
    RecordingPresetRegistry reg;
    reg.LoadState({}, "preset.feedbeefdeadbeef");
    EXPECT_EQ(reg.SelectedId(), kDefaultPresetId);
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
    reg.LoadState(presets, "preset.aabbccddeeff0011");
    EXPECT_EQ(reg.Count(), 5u);                    // 4 built-ins + the kept user preset
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
    reg.LoadState(presets, "preset.aabbccddeeff0011");
    ASSERT_EQ(reg.Count(), 5u); // 4 built-ins + the user preset
    EXPECT_EQ(reg.SelectedPreset().name, "Trimmed");
    EXPECT_EQ(reg.SelectedPreset().config.countdown_seconds, 0);
}

// ===========================================================================
// Built-in presets
// ===========================================================================

// Production call site: MainWindow ctor -> RecordingPresetRegistry() and
// LoadState() after RecordingPresetStore::Load().
TEST(RecordingPresetRegistry, Constructor_SeedsFourBuiltIns_DefaultSelected) {
    RecordingPresetRegistry reg;
    EXPECT_EQ(reg.Count(), 4u);
    EXPECT_EQ(reg.SelectedId(), kDefaultPresetId);
    EXPECT_NE(reg.FindById(kQualityPresetId), nullptr);
    EXPECT_NE(reg.FindById(kEfficiencyPresetId), nullptr);
    EXPECT_NE(reg.FindById(kCompatibilityPresetId), nullptr);
}

// Production call sites: onRenamePreset, onDeletePreset — the registry is the
// enforcement layer, the UI disable is only cosmetics.
TEST(RecordingPresetRegistry, BuiltIn_RenameDelete_Refused) {
    RecordingPresetRegistry reg;
    ASSERT_TRUE(reg.SetSelected(std::string(kQualityPresetId)));

    EXPECT_FALSE(reg.RenameSelected("My Quality"));
    EXPECT_EQ(reg.SelectedPreset().name, "Quality");

    EXPECT_FALSE(reg.DeleteSelected());
    EXPECT_EQ(reg.Count(), 4u);
}

TEST(RecordingPresetRegistry, RenameSelected_CaseInsensitiveCollision_Refused) {
    RecordingPresetRegistry reg;
    reg.AddPreset(MakeDefaultPreset().config, "Streaming");
    reg.AddPreset(MakeDefaultPreset().config, "Other");
    EXPECT_FALSE(reg.RenameSelected(" streaming "));
    EXPECT_FALSE(reg.RenameSelected("quality")); // built-in names are reserved
    EXPECT_TRUE(reg.RenameSelected("Recording"));
}

TEST(RecordingPresetRegistry, IsNameTaken_FoldsAndExcludesSelf) {
    RecordingPresetRegistry reg;
    const std::string id = reg.AddPreset(MakeDefaultPreset().config, "Streaming");
    EXPECT_TRUE(reg.IsNameTaken("STREAMING "));
    EXPECT_TRUE(reg.IsNameTaken("default"));        // reserved
    EXPECT_FALSE(reg.IsNameTaken("Streaming", id)); // renaming to own name is fine
    EXPECT_FALSE(reg.IsNameTaken("Fresh"));
}

// Production call site: MainWindow::onDeletePreset — after delete the
// selection falls to Default. This test covers only the registry's own
// contract (selection + count); MainWindow no longer re-applies the newly
// selected preset's saved config, but that is a MainWindow-level guarantee
// this test cannot observe.
TEST(RecordingPresetRegistry, DeleteUserPreset_SelectionFallsToDefault) {
    RecordingPresetRegistry reg;
    reg.AddPreset(MakeDefaultPreset().config, "Mine");
    ASSERT_TRUE(reg.DeleteSelected());
    EXPECT_EQ(reg.SelectedId(), kDefaultPresetId);
    EXPECT_EQ(reg.Count(), 4u);
}

// Production call site: MainWindow ctor LoadState — a stale persisted copy of
// a built-in id is dropped; a user preset named like a built-in is deduped.
TEST(RecordingPresetRegistry, LoadState_DropsPersistedBuiltInIds_DedupesReservedNames) {
    RecordingPreset stale_default = MakeDefaultPreset();
    stale_default.config.video.cq = 51; // must not survive

    RecordingPreset user;
    user.id = GeneratePresetId();
    user.name = "quality"; // collides with built-in name (folded)
    user.config = MakeDefaultPreset().config;

    RecordingPresetRegistry reg;
    reg.LoadState({stale_default, user}, user.id);
    EXPECT_EQ(reg.Count(), 5u);
    EXPECT_EQ(reg.FindById(kDefaultPresetId)->config.video.cq, 19u);
    EXPECT_EQ(reg.FindById(user.id)->name, "quality (2)");
    EXPECT_EQ(reg.SelectedId(), user.id);
}

// Production call site: RecordingPresetRegistry::AddPreset via
// MainWindow::onSavePresetAs — snapshots never carry environment fields.
TEST(RecordingPresetRegistry, AddPreset_StripsEnvironmentFields) {
    RecordingPresetRegistry reg;
    RecordingPresetConfig live = MakeDefaultPreset().config;
    live.output.video_codec = capability::VideoCodec::Hevc;
    live.output.bit_depth = capability::BitDepth::Bit10;
    live.capture.display_id.device_path = "monitor-1";
    const std::string id = reg.AddPreset(live, "Mine");
    const OutputSettingsModel defaults = OutputSettingsModel::Defaults();
    EXPECT_EQ(reg.FindById(id)->config.output.bit_depth, defaults.bit_depth);
    EXPECT_TRUE(reg.FindById(id)->config.capture.display_id.empty());
}

// ===========================================================================
// Selecting a built-in never reports (changed)
// ===========================================================================

// Regression guard: selecting a built-in preset must leave the live config
// clean — the "(changed)" hint must not appear without a user edit.
//
// This reproduces MainWindow::onPresetSelected → applyPresetConfig →
// captureLiveConfig exactly: the applied live config is the selected preset's
// saved config carried through WithEnvironmentFields (the previous selection's
// environment) and SanitizePresetConfig, and the dirty check compares that
// against the newly-selected preset via IsSelectedDirty. It runs the full
// from→to matrix so every built-in is verified both as the switch source and
// the switch target (Efficiency's cq 30 = CanonicalCq(Efficient) is the case
// that historically snapped to a canonical value and produced a spurious
// dirty).
TEST(RecordingPresetRegistry, SelectingBuiltIn_NeverReportsDirty_AllTransitions) {
    const std::array<std::string_view, 4> ids = {kDefaultPresetId, kQualityPresetId, kEfficiencyPresetId,
                                                 kCompatibilityPresetId};
    for (const auto from_id : ids) {
        for (const auto to_id : ids) {
            RecordingPresetRegistry reg;
            ASSERT_TRUE(reg.SetSelected(std::string(from_id)));
            // Environment carried from the previous selection's live config.
            const RecordingPresetConfig env = reg.SelectedSavedConfig();

            ASSERT_TRUE(reg.SetSelected(std::string(to_id)));
            // applyPresetConfig stages the mirror as SanitizePresetConfig(...);
            // captureLiveConfig sanitizes once more before the dirty compare.
            const RecordingPresetConfig applied =
                SanitizePresetConfig(WithEnvironmentFields(reg.SelectedSavedConfig(), env));
            const RecordingPresetConfig live = SanitizePresetConfig(applied);

            EXPECT_FALSE(reg.IsSelectedDirty(live)) << "selecting built-in '" << to_id << "' (from '" << from_id
                                                    << "') must not report (changed) without a user edit";
        }
    }
}

} // namespace
} // namespace exosnap
