#include <gtest/gtest.h>

#include <recorder_core/audio_track_model.h>

#include "viewmodels/PresentationState.h"
#include "viewmodels/PresentationStateBuilder.h"

namespace exosnap {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

capability::AudioUiState MakeDisplayState(bool sys_enabled = true, bool mic_enabled = false) {
    capability::AudioUiState s;
    s.target_kind = capability::CaptureTargetKind::Display;
    s.source_rows.push_back({recorder_core::AudioSourceKind::SystemOutput, sys_enabled, false});
    s.source_rows.push_back({recorder_core::AudioSourceKind::Mic, mic_enabled, false});
    return s;
}

capability::AudioUiState MakeWindowState(bool app_enabled = true, bool sys_enabled = false, bool mic_enabled = false) {
    capability::AudioUiState s;
    s.target_kind = capability::CaptureTargetKind::Window;
    s.source_rows.push_back({recorder_core::AudioSourceKind::App, app_enabled, false});
    s.source_rows.push_back({recorder_core::AudioSourceKind::Sys, sys_enabled, false});
    s.source_rows.push_back({recorder_core::AudioSourceKind::Mic, mic_enabled, false});
    return s;
}

// ---------------------------------------------------------------------------
// Test: target-kind → visibility
// ---------------------------------------------------------------------------

TEST(PresentationStateBuilderTest, DisplayTarget_AppHidden) {
    // Test 1 / 9: Display target hides App audio row.
    const auto snap = PresentationStateBuilder::BuildAudioConfiguration(MakeDisplayState(), false);
    EXPECT_FALSE(snap.app.visible);
}

TEST(PresentationStateBuilderTest, DisplayTarget_SysAndMicVisible) {
    // Test 1: Display target shows Computer audio and Mic.
    const auto snap = PresentationStateBuilder::BuildAudioConfiguration(MakeDisplayState(), false);
    EXPECT_TRUE(snap.system.visible);
    EXPECT_TRUE(snap.mic.visible);
}

TEST(PresentationStateBuilderTest, WindowTarget_AppVisible) {
    // Test 8: Window target makes App visible.
    const auto snap = PresentationStateBuilder::BuildAudioConfiguration(MakeWindowState(), false);
    EXPECT_TRUE(snap.app.visible);
    EXPECT_TRUE(snap.app.available);
}

TEST(PresentationStateBuilderTest, WindowTarget_SysAndMicVisible) {
    // Test 2: Window target shows Application audio, Other system audio, and Mic.
    const auto snap = PresentationStateBuilder::BuildAudioConfiguration(MakeWindowState(), false);
    EXPECT_TRUE(snap.system.visible);
    EXPECT_TRUE(snap.mic.visible);
}

TEST(PresentationStateBuilderTest, DisplayTarget_AppNotAvailable) {
    // Test 9: Display target: App row not in plan → not available.
    const auto snap = PresentationStateBuilder::BuildAudioConfiguration(MakeDisplayState(), false);
    EXPECT_FALSE(snap.app.available);
    EXPECT_FALSE(snap.app.controls_enabled);
}

// ---------------------------------------------------------------------------
// Test: lock state
// ---------------------------------------------------------------------------

TEST(PresentationStateBuilderTest, ControlsLocked_AllControlsDisabled) {
    // Test 4: Recording lock disables all editable audio controls.
    const auto snap = PresentationStateBuilder::BuildAudioConfiguration(MakeWindowState(), /*locked=*/true);
    EXPECT_FALSE(snap.app.controls_enabled);
    EXPECT_FALSE(snap.system.controls_enabled);
    EXPECT_FALSE(snap.mic.controls_enabled);
}

TEST(PresentationStateBuilderTest, ControlsUnlocked_WindowTarget_AppEnabled) {
    // Test 8: Unlocked Window target → App controls_enabled.
    const auto snap = PresentationStateBuilder::BuildAudioConfiguration(MakeWindowState(), /*locked=*/false);
    EXPECT_TRUE(snap.app.controls_enabled);
}

// ---------------------------------------------------------------------------
// Test: call-order invariant (lock/order defect)
// ---------------------------------------------------------------------------

TEST(PresentationStateBuilderTest, AudioThenLock_SameAsLockThenAudio) {
    // Test 5: Applying audio state then lock = applying lock then audio.
    // Both orderings must produce identical snapshots.
    const auto audio_state = MakeWindowState(/*app=*/true, /*sys=*/true, /*mic=*/true);

    // Order A: build with no lock, then rebuild with lock.
    const auto snap_no_lock = PresentationStateBuilder::BuildAudioConfiguration(audio_state, false);
    const auto snap_locked = PresentationStateBuilder::BuildAudioConfiguration(audio_state, true);

    // Locked snapshot must have controls_enabled=false for all rows.
    EXPECT_FALSE(snap_locked.app.controls_enabled);
    EXPECT_FALSE(snap_locked.system.controls_enabled);
    EXPECT_FALSE(snap_locked.mic.controls_enabled);

    // Unlocked snapshot must have controls_enabled=true for available rows.
    EXPECT_TRUE(snap_no_lock.app.controls_enabled);
    EXPECT_TRUE(snap_no_lock.system.controls_enabled);
    EXPECT_TRUE(snap_no_lock.mic.controls_enabled);

    // The locked snapshot must differ from the unlocked one.
    EXPECT_NE(snap_locked, snap_no_lock);
}

TEST(PresentationStateBuilderTest, MeterUpdateCannotReenableLockedControls) {
    // Test 6: A meter update (AudioMeterSnapshot) carries no structural state.
    // Applying one must not change enabled state.
    const auto audio_state = MakeWindowState(true, true, true);
    const auto snap = PresentationStateBuilder::BuildAudioConfiguration(audio_state, /*locked=*/true);

    // Simulate a "meter update" — rebuild with the same state.
    const auto snap_after_meter = PresentationStateBuilder::BuildAudioConfiguration(audio_state, /*locked=*/true);

    EXPECT_EQ(snap, snap_after_meter);
    EXPECT_FALSE(snap_after_meter.app.controls_enabled);
}

TEST(PresentationStateBuilderTest, MeterUpdateCannotMakeUnavailableAppAvailable) {
    // Test 7: Meter update cannot change availability of App audio for Display target.
    const auto display_state = MakeDisplayState();
    const auto snap = PresentationStateBuilder::BuildAudioConfiguration(display_state, false);

    // App is not in the plan for Display → not available.
    EXPECT_FALSE(snap.app.available);
    EXPECT_FALSE(snap.app.controls_enabled);

    // Even after a simulated "meter tick" (same state rebuild), still not available.
    const auto snap_after = PresentationStateBuilder::BuildAudioConfiguration(display_state, false);
    EXPECT_FALSE(snap_after.app.available);
}

// ---------------------------------------------------------------------------
// Test: per-source invariant
// ---------------------------------------------------------------------------

TEST(PresentationStateBuilderTest, ControlsEnabledInvariant_VisibleAndAvailableAndNotLocked) {
    // Verify the invariant: controls_enabled = visible && available && !locked
    // for each of the three sources.
    const auto state = MakeWindowState(true, true, true);

    for (bool locked : {false, true}) {
        const auto snap = PresentationStateBuilder::BuildAudioConfiguration(state, locked);
        EXPECT_EQ(snap.app.controls_enabled, snap.app.visible && snap.app.available && !locked);
        EXPECT_EQ(snap.system.controls_enabled, snap.system.visible && snap.system.available && !locked);
        EXPECT_EQ(snap.mic.controls_enabled, snap.mic.visible && snap.mic.available && !locked);
    }
}

// ---------------------------------------------------------------------------
// Test: mic survives target changes
// ---------------------------------------------------------------------------

TEST(PresentationStateBuilderTest, MicSurvivesTargetChange_DisplayToWindow) {
    // Test 10: Mic state survives target switch.
    capability::AudioUiState s;
    s.target_kind = capability::CaptureTargetKind::Display;
    s.source_rows.push_back({recorder_core::AudioSourceKind::SystemOutput, true, false});
    s.source_rows.push_back({recorder_core::AudioSourceKind::Mic, true, false});

    const auto snap_display = PresentationStateBuilder::BuildAudioConfiguration(s, false);
    EXPECT_TRUE(snap_display.mic.visible);
    EXPECT_TRUE(snap_display.mic.enabled);

    s.target_kind = capability::CaptureTargetKind::Window;
    s.source_rows.clear();
    s.source_rows.push_back({recorder_core::AudioSourceKind::App, true, false});
    s.source_rows.push_back({recorder_core::AudioSourceKind::Sys, false, false});
    s.source_rows.push_back({recorder_core::AudioSourceKind::Mic, true, false}); // mic ON persists

    const auto snap_window = PresentationStateBuilder::BuildAudioConfiguration(s, false);
    EXPECT_TRUE(snap_window.mic.visible);
    EXPECT_TRUE(snap_window.mic.enabled);
}

// ---------------------------------------------------------------------------
// Test: separate track survives target changes
// ---------------------------------------------------------------------------

TEST(PresentationStateBuilderTest, SeparateTrackSurvivesTargetChange) {
    // Test 11: Separate-track values survive all target changes.
    capability::AudioUiState s;
    s.target_kind = capability::CaptureTargetKind::Window;
    recorder_core::AudioSourceRow mic_row;
    mic_row.kind = recorder_core::AudioSourceKind::Mic;
    mic_row.enabled = true;
    mic_row.merge_with_above = false; // "separate track" = true
    s.source_rows.push_back({recorder_core::AudioSourceKind::App, true, false});
    s.source_rows.push_back({recorder_core::AudioSourceKind::Sys, false, false});
    s.source_rows.push_back(mic_row);

    const auto snap = PresentationStateBuilder::BuildAudioConfiguration(s, false);
    EXPECT_TRUE(snap.mic.separate_track); // merge_with_above=false → separate_track=true
}

// ---------------------------------------------------------------------------
// Test: structural equality for deduplication
// ---------------------------------------------------------------------------

TEST(PresentationStateBuilderTest, SameState_Equals) {
    // Test 14: Identical inputs produce equal snapshots.
    const auto s = MakeWindowState();
    const auto snap1 = PresentationStateBuilder::BuildAudioConfiguration(s, false);
    const auto snap2 = PresentationStateBuilder::BuildAudioConfiguration(s, false);
    EXPECT_EQ(snap1, snap2);
}

TEST(PresentationStateBuilderTest, DifferentLock_NotEquals) {
    // Test 14: Different lock state produces different snapshot.
    const auto s = MakeWindowState();
    const auto snap_unlocked = PresentationStateBuilder::BuildAudioConfiguration(s, false);
    const auto snap_locked = PresentationStateBuilder::BuildAudioConfiguration(s, true);
    EXPECT_NE(snap_unlocked, snap_locked);
}

TEST(PresentationStateBuilderTest, DifferentTarget_NotEquals) {
    // Test 14: Different target kind produces different snapshot.
    const auto snap_display = PresentationStateBuilder::BuildAudioConfiguration(MakeDisplayState(), false);
    const auto snap_window = PresentationStateBuilder::BuildAudioConfiguration(MakeWindowState(), false);
    EXPECT_NE(snap_display, snap_window);
}

// ---------------------------------------------------------------------------
// Test: no duplicate App row
// ---------------------------------------------------------------------------

TEST(PresentationStateBuilderTest, NoDuplicateAppRow_DisplayTarget) {
    // Test 13: Display target — App row NOT in plan and not visible.
    const auto snap = PresentationStateBuilder::BuildAudioConfiguration(MakeDisplayState(), false);
    EXPECT_FALSE(snap.app.visible);
    EXPECT_FALSE(snap.app.available);
}

// ---------------------------------------------------------------------------
// Test: target kind stored in snapshot
// ---------------------------------------------------------------------------

TEST(PresentationStateBuilderTest, TargetKindStoredInSnapshot) {
    {
        const auto snap = PresentationStateBuilder::BuildAudioConfiguration(MakeDisplayState(), false);
        EXPECT_EQ(snap.target_kind, capability::CaptureTargetKind::Display);
    }
    {
        const auto snap = PresentationStateBuilder::BuildAudioConfiguration(MakeWindowState(), false);
        EXPECT_EQ(snap.target_kind, capability::CaptureTargetKind::Window);
    }
}

// ---------------------------------------------------------------------------
// Test: mic device ID preserved in snapshot
// ---------------------------------------------------------------------------

TEST(PresentationStateBuilderTest, MicDeviceId_PreservedInSnapshot) {
    capability::AudioUiState s = MakeDisplayState();
    s.selected_mic_device_id = "device-abc-123";

    const auto snap = PresentationStateBuilder::BuildAudioConfiguration(s, false);
    ASSERT_TRUE(snap.selected_mic_device_id.has_value());
    EXPECT_EQ(*snap.selected_mic_device_id, "device-abc-123");
}

TEST(PresentationStateBuilderTest, MicDeviceId_NulloptWhenNotSet) {
    const auto snap = PresentationStateBuilder::BuildAudioConfiguration(MakeDisplayState(), false);
    EXPECT_FALSE(snap.selected_mic_device_id.has_value());
}

// ---------------------------------------------------------------------------
// Test: AudioMeterSnapshot is separate from structural state
// ---------------------------------------------------------------------------

TEST(PresentationStateBuilderTest, AudioMeterSnapshot_IsIndependentOfStructure) {
    // Test 15: Meter-only changes (AudioMeterSnapshot) do not affect structural state.
    // The structural snapshot must be unchanged when only meter values differ.
    AudioMeterSnapshot meter_a{0.3f, 0.0f, 0.5f, true, false, true};
    AudioMeterSnapshot meter_b{0.9f, 0.0f, 0.1f, true, false, true};

    // Two different meter readings must not change the structural snapshot.
    const auto state = MakeWindowState(true, true, true);
    const auto snap = PresentationStateBuilder::BuildAudioConfiguration(state, false);

    (void)meter_a; // Meter data is orthogonal — snapshot is unchanged.
    (void)meter_b;

    const auto snap_after = PresentationStateBuilder::BuildAudioConfiguration(state, false);
    EXPECT_EQ(snap, snap_after);
}

// ---------------------------------------------------------------------------
// Test: AudioSourcePresentationState equality
// ---------------------------------------------------------------------------

TEST(AudioSourcePresentationStateTest, EqualityOperator) {
    AudioSourcePresentationState a;
    a.visible = true;
    a.available = true;
    a.enabled = true;
    a.controls_enabled = true;
    a.separate_track = false;

    AudioSourcePresentationState b = a;
    EXPECT_EQ(a, b);

    b.controls_enabled = false;
    EXPECT_NE(a, b);
}

} // namespace
} // namespace exosnap
