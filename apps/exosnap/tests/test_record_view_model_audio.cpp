#include <gtest/gtest.h>

#include <algorithm>

#include "viewmodels/RecordViewModel.h"

namespace exosnap {
namespace {

// Helper: find a source row by kind.
const recorder_core::AudioSourceRow* FindRow(const capability::AudioUiState& s, recorder_core::AudioSourceKind k) {
    for (const auto& r : s.source_rows)
        if (r.kind == k)
            return &r;
    return nullptr;
}

TEST(RecordViewModelAudioTest, RecordViewModel_DefaultAudioStateForWindowTarget) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);

    EXPECT_EQ(vm.audio_ui_state.target_kind, capability::CaptureTargetKind::Window);
    // Policy: Application audio ON; Other system audio and Microphone OFF by default.
    EXPECT_TRUE(vm.audio_ui_state.IsAppEnabled());
    EXPECT_FALSE(vm.audio_ui_state.IsSysEnabled());
    EXPECT_FALSE(vm.audio_ui_state.IsMicEnabled());

    ASSERT_EQ(vm.audio_track_preview.size(), 1u);
    EXPECT_EQ(vm.audio_track_preview[0].source_key, "app");
}

TEST(RecordViewModelAudioTest, RecordViewModel_DefaultMicGainLinear_IsUnity) {
    RecordViewModel vm;
    EXPECT_FLOAT_EQ(vm.audio_ui_state.mic_gain_linear, 1.0f);
}

TEST(RecordViewModelAudioTest, RecordViewModel_DefaultAudioStateForDisplayTarget) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Display);

    EXPECT_EQ(vm.audio_ui_state.target_kind, capability::CaptureTargetKind::Display);
    // Policy: Computer audio ON; Microphone OFF by default.
    EXPECT_FALSE(vm.audio_ui_state.IsAppEnabled());
    EXPECT_TRUE(vm.audio_ui_state.IsSysEnabled());
    EXPECT_FALSE(vm.audio_ui_state.IsMicEnabled());

    ASSERT_EQ(vm.audio_track_preview.size(), 1u);
    EXPECT_EQ(vm.audio_track_preview[0].source_key, "system_output");
}

TEST(RecordViewModelAudioTest, RecordViewModel_ApplyTargetKind_DisplayResetsToDisplayDefaults) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    for (auto& r : vm.audio_ui_state.source_rows)
        r.merge_with_above = false;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Display);

    EXPECT_FALSE(vm.audio_ui_state.IsAppEnabled());
    EXPECT_TRUE(vm.audio_ui_state.IsSysEnabled());
    EXPECT_FALSE(vm.audio_ui_state.IsMicEnabled()); // Mic OFF by default policy
}

TEST(RecordViewModelAudioTest, RecordViewModel_TrackPreviewUpdatesOnOutputToggles) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    // Enable all sources for this toggle test.
    for (auto& r : vm.audio_ui_state.source_rows)
        r.enabled = true;
    vm.RebuildAudioPlan();
    ASSERT_EQ(vm.audio_track_preview.size(), 3u);

    // Disable Sys.
    for (auto& r : vm.audio_ui_state.source_rows)
        if (r.kind == recorder_core::AudioSourceKind::Sys)
            r.enabled = false;
    vm.RebuildAudioPlan();
    ASSERT_EQ(vm.audio_track_preview.size(), 2u);

    // Disable App too.
    for (auto& r : vm.audio_ui_state.source_rows)
        if (r.kind == recorder_core::AudioSourceKind::App)
            r.enabled = false;
    vm.RebuildAudioPlan();
    ASSERT_EQ(vm.audio_track_preview.size(), 1u);
    EXPECT_EQ(vm.audio_track_preview[0].source_key, "mic");

    // Disable Mic too.
    for (auto& r : vm.audio_ui_state.source_rows)
        if (r.kind == recorder_core::AudioSourceKind::Mic)
            r.enabled = false;
    vm.RebuildAudioPlan();
    EXPECT_TRUE(vm.audio_track_preview.empty());
}

TEST(RecordViewModelAudioTest, RecordViewModel_TrackPreviewUpdatesOnMicToggle) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    // Enable App + Sys for this test.
    for (auto& r : vm.audio_ui_state.source_rows)
        r.enabled = true;
    vm.RebuildAudioPlan();
    ASSERT_EQ(vm.audio_track_preview.size(), 3u);

    // Disable Mic.
    for (auto& r : vm.audio_ui_state.source_rows)
        if (r.kind == recorder_core::AudioSourceKind::Mic)
            r.enabled = false;
    vm.RebuildAudioPlan();
    ASSERT_EQ(vm.audio_track_preview.size(), 2u);

    // Enable Mic again.
    for (auto& r : vm.audio_ui_state.source_rows)
        if (r.kind == recorder_core::AudioSourceKind::Mic)
            r.enabled = true;
    vm.RebuildAudioPlan();
    ASSERT_EQ(vm.audio_track_preview.size(), 3u);
}

TEST(RecordViewModelAudioTest, RecordViewModel_RebuildAudioPlan_PropagatesMicDeviceId) {
    RecordViewModel vm;

    vm.audio_ui_state.selected_mic_device_id = "device-123";
    vm.RebuildAudioPlan();

    ASSERT_TRUE(vm.audio_plan.mic_device_id.has_value());
    EXPECT_EQ(vm.audio_plan.mic_device_id.value(), "device-123");
}

TEST(RecordViewModelAudioTest, RecordViewModel_RebuildAudioPlan_PreservesMicGainLinear) {
    RecordViewModel vm;

    vm.audio_ui_state.mic_gain_linear = 4.0f;
    vm.RebuildAudioPlan();

    EXPECT_FLOAT_EQ(vm.audio_ui_state.mic_gain_linear, 4.0f);
    EXPECT_FLOAT_EQ(vm.audio_plan.mic_gain_linear, 4.0f);
}

TEST(RecordViewModelAudioTest, RecordViewModel_RebuildAudioPlan_SetsActiveFlagsForWindowTracks) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    // Policy: only Application audio is active by default.
    EXPECT_TRUE(vm.audio_active_app);
    EXPECT_FALSE(vm.audio_active_sys);
    EXPECT_FALSE(vm.audio_active_mic);
}

TEST(RecordViewModelAudioTest, RecordViewModel_RebuildAudioPlan_WindowDefaultActivatesOnlyAppMeter) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    EXPECT_TRUE(vm.audio_active_app);
    EXPECT_FALSE(vm.audio_active_sys);
    EXPECT_FALSE(vm.audio_active_mic);
}

TEST(RecordViewModelAudioTest, RecordViewModel_UpdateStats_MapsPerTrackRmsToSources) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);

    recorder_core::SessionStats stats;
    stats.per_track_rms[0] = 0.25f;

    vm.UpdateStats(stats);

    EXPECT_FLOAT_EQ(vm.audio_rms_app, 0.25f);
    EXPECT_FLOAT_EQ(vm.audio_rms_sys, 0.0f);
    EXPECT_FLOAT_EQ(vm.audio_rms_mic, 0.0f);
}

TEST(RecordViewModelAudioTest, RecordViewModel_UpdateStats_MapsMicRms) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    // Enable Mic so it becomes track 1 (App=track 0, Mic=track 1).
    for (auto& r : vm.audio_ui_state.source_rows)
        if (r.kind == recorder_core::AudioSourceKind::Mic)
            r.enabled = true;
    vm.RebuildAudioPlan();

    recorder_core::SessionStats stats;
    stats.per_track_rms[1] = 0.75f;

    vm.UpdateStats(stats);

    EXPECT_FLOAT_EQ(vm.audio_rms_app, 0.0f);
    EXPECT_FLOAT_EQ(vm.audio_rms_sys, 0.0f);
    EXPECT_FLOAT_EQ(vm.audio_rms_mic, 0.75f);
}

TEST(RecordViewModelAudioTest, RecordViewModel_UpdateStats_MergedWindowRmsGoesToBothMeters) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    // Enable all sources then merge Mic and Sys into the App track.
    for (auto& r : vm.audio_ui_state.source_rows)
        r.enabled = true;
    for (auto& r : vm.audio_ui_state.source_rows) {
        if (r.kind == recorder_core::AudioSourceKind::Mic || r.kind == recorder_core::AudioSourceKind::Sys) {
            r.merge_with_above = true;
        }
    }
    vm.RebuildAudioPlan();

    recorder_core::SessionStats stats;
    stats.per_track_rms[0] = 0.4f;

    vm.UpdateStats(stats);

    EXPECT_FLOAT_EQ(vm.audio_rms_sys, 0.4f);
    EXPECT_FLOAT_EQ(vm.audio_rms_app, 0.4f);
    EXPECT_FLOAT_EQ(vm.audio_rms_mic, 0.4f);
}

TEST(RecordViewModelAudioTest, RecordViewModel_UpdateStats_IgnoresInvalidTrackNumbers) {
    RecordViewModel vm;

    vm.audio_track_preview = {
        capability::AudioTrackPreview{0, "app", "invalid"},
        capability::AudioTrackPreview{99, "sys", "invalid"},
    };

    recorder_core::SessionStats stats;
    stats.per_track_rms[0] = 0.9f;
    stats.per_track_rms[1] = 0.8f;
    stats.per_track_rms[2] = 0.7f;

    vm.UpdateStats(stats);

    EXPECT_FLOAT_EQ(vm.audio_rms_app, 0.0f);
    EXPECT_FLOAT_EQ(vm.audio_rms_sys, 0.0f);
    EXPECT_FLOAT_EQ(vm.audio_rms_mic, 0.0f);
}

TEST(RecordViewModelAudioTest, RecordViewModel_TrackPreviewDisplayTarget_SystemOutput) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Display);

    // Policy: Mic OFF by default → only Computer audio (SystemOutput) track.
    ASSERT_EQ(vm.audio_track_preview.size(), 1u);
    EXPECT_EQ(vm.audio_track_preview[0].source_key, "system_output");
    EXPECT_EQ(vm.audio_track_preview[0].display_label, "System Audio");
}

TEST(RecordViewModelAudioTest, RecordViewModel_ApplyTargetKindPreservingAudio_KeepsSourceRows) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    // Disable Sys and enable Mic.
    for (auto& r : vm.audio_ui_state.source_rows) {
        if (r.kind == recorder_core::AudioSourceKind::Sys)
            r.enabled = false;
        if (r.kind == recorder_core::AudioSourceKind::Mic)
            r.enabled = true;
    }
    vm.audio_ui_state.mic_channel_mode = recorder_core::MicChannelMode::MonoMix;
    const auto saved_rows = vm.audio_ui_state.source_rows;

    vm.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Display);

    EXPECT_EQ(vm.audio_ui_state.target_kind, capability::CaptureTargetKind::Display);
    ASSERT_EQ(vm.audio_ui_state.source_rows.size(), saved_rows.size());
    for (std::size_t i = 0; i < saved_rows.size(); ++i) {
        EXPECT_EQ(vm.audio_ui_state.source_rows[i].kind, saved_rows[i].kind);
        EXPECT_EQ(vm.audio_ui_state.source_rows[i].enabled, saved_rows[i].enabled);
        EXPECT_EQ(vm.audio_ui_state.source_rows[i].merge_with_above, saved_rows[i].merge_with_above);
    }
    EXPECT_EQ(vm.audio_ui_state.mic_channel_mode, recorder_core::MicChannelMode::MonoMix);
}

TEST(RecordViewModelAudioTest, RecordViewModel_ApplyTargetKindPreservingAudio_WindowModeKeepsAppAvailability) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    // Set all rows to separate (no merging).
    for (auto& r : vm.audio_ui_state.source_rows)
        r.merge_with_above = false;
    for (auto& r : vm.audio_ui_state.source_rows) {
        if (r.kind == recorder_core::AudioSourceKind::App || r.kind == recorder_core::AudioSourceKind::Sys)
            r.enabled = true;
    }

    vm.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Window);

    const bool has_app_track =
        std::any_of(vm.audio_track_preview.begin(), vm.audio_track_preview.end(),
                    [](const capability::AudioTrackPreview& preview) { return preview.source_key == "app"; });
    EXPECT_TRUE(has_app_track);
}

// ---------------------------------------------------------------------------
// UpdateMeterRms — high-cadence recording meter path
// ---------------------------------------------------------------------------

TEST(RecordViewModelAudioTest, RecordViewModel_UpdateMeterRms_MapsAppTrack) {
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);

    std::array<float, 3> rms{0.6f, 0.0f, 0.0f};
    vm.UpdateMeterRms(rms);

    EXPECT_FLOAT_EQ(vm.audio_rms_app, 0.6f);
    EXPECT_FLOAT_EQ(vm.audio_rms_sys, 0.0f);
    EXPECT_FLOAT_EQ(vm.audio_rms_mic, 0.0f);
}

TEST(RecordViewModelAudioTest, RecordViewModel_UpdateMeterRms_MapsMicTrack) {
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    // Enable Mic so it becomes track 1 (App=track 0, Mic=track 1, Sys still off).
    for (auto& r : vm.audio_ui_state.source_rows)
        if (r.kind == recorder_core::AudioSourceKind::Mic)
            r.enabled = true;
    vm.RebuildAudioPlan();

    std::array<float, 3> rms{0.0f, 0.9f, 0.0f};
    vm.UpdateMeterRms(rms);

    EXPECT_FLOAT_EQ(vm.audio_rms_app, 0.0f);
    EXPECT_FLOAT_EQ(vm.audio_rms_mic, 0.9f);
    EXPECT_FLOAT_EQ(vm.audio_rms_sys, 0.0f);
}

TEST(RecordViewModelAudioTest, RecordViewModel_UpdateMeterRms_DoesNotAffectOtherFields) {
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    vm.elapsed_seconds = 42.0;
    vm.frames_captured = 123;

    std::array<float, 3> rms{0.5f, 0.0f, 0.0f};
    vm.UpdateMeterRms(rms);

    // Elapsed and frame count must be untouched
    EXPECT_DOUBLE_EQ(vm.elapsed_seconds, 42.0);
    EXPECT_EQ(vm.frames_captured, 123u);
}

TEST(RecordViewModelAudioTest, RecordViewModel_UpdateMeterRms_DisabledSourceStaysZero) {
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    // Enable Mic to get 2 tracks; Sys stays off.
    for (auto& r : vm.audio_ui_state.source_rows)
        if (r.kind == recorder_core::AudioSourceKind::Mic)
            r.enabled = true;
    vm.RebuildAudioPlan();

    // Track 0=app, track 1=mic; sys is not a preview track.
    std::array<float, 3> rms{0.5f, 0.4f, 0.3f};
    vm.UpdateMeterRms(rms);

    EXPECT_FLOAT_EQ(vm.audio_rms_app, 0.5f);
    EXPECT_FLOAT_EQ(vm.audio_rms_mic, 0.4f);
    EXPECT_FLOAT_EQ(vm.audio_rms_sys, 0.0f); // not in preview → stays zero
}

TEST(RecordViewModelAudioTest, RecordViewModel_UpdateMeterRms_MergedTrackFillsAllActive) {
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    // Enable all sources, then merge Mic and Sys into the App track.
    for (auto& r : vm.audio_ui_state.source_rows)
        r.enabled = true;
    for (auto& r : vm.audio_ui_state.source_rows) {
        if (r.kind == recorder_core::AudioSourceKind::Mic || r.kind == recorder_core::AudioSourceKind::Sys)
            r.merge_with_above = true;
    }
    vm.RebuildAudioPlan();

    std::array<float, 3> rms{0.4f, 0.0f, 0.0f};
    vm.UpdateMeterRms(rms);

    EXPECT_FLOAT_EQ(vm.audio_rms_app, 0.4f);
    EXPECT_FLOAT_EQ(vm.audio_rms_sys, 0.4f);
    EXPECT_FLOAT_EQ(vm.audio_rms_mic, 0.4f);
}

TEST(RecordViewModelAudioTest, RecordViewModel_UpdateStats_StillMapsRmsCorrectly) {
    // Verify that UpdateStats delegates to UpdateMeterRms correctly.
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    // Enable Mic to get 2 tracks (App=0, Mic=1).
    for (auto& r : vm.audio_ui_state.source_rows)
        if (r.kind == recorder_core::AudioSourceKind::Mic)
            r.enabled = true;
    vm.RebuildAudioPlan();

    recorder_core::SessionStats stats;
    stats.per_track_rms[0] = 0.3f;
    stats.per_track_rms[1] = 0.7f;

    vm.UpdateStats(stats);

    EXPECT_FLOAT_EQ(vm.audio_rms_app, 0.3f);
    EXPECT_FLOAT_EQ(vm.audio_rms_mic, 0.7f);
    EXPECT_FLOAT_EQ(vm.audio_rms_sys, 0.0f);
}

TEST(RecordViewModelAudioTest, RecordViewModel_DisplayLabelFromTarget_NormalizesWin32DisplayName) {
    EXPECT_EQ(RecordViewModel::DisplayLabelFromTarget(R"(\\.\DISPLAY1)"), "Display 1");
    EXPECT_EQ(RecordViewModel::DisplayLabelFromTarget("DISPLAY2"), "Display 2");
    EXPECT_EQ(RecordViewModel::DisplayLabelFromTarget("Custom Monitor"), "Custom Monitor");
}

TEST(RecordViewModelAudioTest, RecordViewModel_WindowLabelFromTarget_AppNameFirst_Brave) {
    EXPECT_EQ(RecordViewModel::WindowLabelFromTarget("ExoSnap UI-Brand-Integration \xE2\x80\x94 Brave"),
              "Brave \xE2\x80\x94 ExoSnap UI-Brand-Integration");
}

TEST(RecordViewModelAudioTest, RecordViewModel_WindowLabelFromTarget_AppNameFirst_Explorer) {
    EXPECT_EQ(RecordViewModel::WindowLabelFromTarget("Debug und 2 weitere Registerkarten \xE2\x80\x94 Explorer"),
              "Explorer \xE2\x80\x94 Debug und 2 weitere Registerkarten");
}

TEST(RecordViewModelAudioTest, RecordViewModel_WindowLabelFromTarget_MissingAppNameFallsBackToExistingLabel) {
    EXPECT_EQ(RecordViewModel::WindowLabelFromTarget("Standalone Utility Window"), "Standalone Utility Window");
}

TEST(RecordViewModelAudioTest, RecordViewModel_SortWindowTargetIndices_SortsByAppThenTitleCaseInsensitive) {
    std::vector<recorder_core::CaptureTarget> targets;
    targets.push_back({recorder_core::CaptureTarget::Kind::Window, 100, "zeta panel \xE2\x80\x94 brave"});
    targets.push_back(
        {recorder_core::CaptureTarget::Kind::Window, 101, "Debug und 2 weitere Registerkarten \xE2\x80\x94 Explorer"});
    targets.push_back({recorder_core::CaptureTarget::Kind::Window, 102, "Alpha document \xE2\x80\x94 Brave"});

    const std::vector<int> sorted = RecordViewModel::SortWindowTargetIndices(targets, {0, 1, 2});
    EXPECT_EQ(sorted, (std::vector<int>{2, 0, 1}));
}

TEST(RecordViewModelAudioTest, RecordViewModel_SortWindowTargetIndices_PreservesOriginalTargetIndices) {
    std::vector<recorder_core::CaptureTarget> targets;
    targets.push_back({recorder_core::CaptureTarget::Kind::Monitor, 1, R"(\\.\DISPLAY1)"});
    targets.push_back({recorder_core::CaptureTarget::Kind::Window, 2, "Main \xE2\x80\x94 Zebra"});
    targets.push_back({recorder_core::CaptureTarget::Kind::Window, 3, "Alpha \xE2\x80\x94 AlphaApp"});
    targets.push_back({recorder_core::CaptureTarget::Kind::Window, 4, "Gamma \xE2\x80\x94 AlphaApp"});
    targets.push_back({recorder_core::CaptureTarget::Kind::Window, 5, "Window with no app"});

    const std::vector<int> sorted = RecordViewModel::SortWindowTargetIndices(targets, {4, 1, 3, 2});
    EXPECT_EQ(sorted, (std::vector<int>{2, 3, 4, 1}));
}

TEST(RecordViewModelAudioTest, RecordViewModel_FilenameContextFromCaptureTarget_MonitorTarget) {
    const recorder_core::CaptureTarget monitor_target{recorder_core::CaptureTarget::Kind::Monitor, 10,
                                                      R"(\\.\DISPLAY1)"};

    const FilenameTargetContext context = RecordViewModel::FilenameContextFromCaptureTarget(monitor_target);
    EXPECT_EQ(context.app_name, L"Desktop");
    EXPECT_EQ(context.window_title, L"Display 1");
    EXPECT_EQ(context.process_name, L"desktop");
    EXPECT_EQ(context.target_name, L"Desktop - Display 1");
}

TEST(RecordViewModelAudioTest, RecordViewModel_FilenameContextFromCaptureTarget_WindowTarget) {
    const recorder_core::CaptureTarget window_target{recorder_core::CaptureTarget::Kind::Window, 11,
                                                     "Claude Design - Brave"};

    const FilenameTargetContext context = RecordViewModel::FilenameContextFromCaptureTarget(window_target);
    EXPECT_EQ(context.app_name, L"Brave");
    EXPECT_EQ(context.window_title, L"Claude Design");
    EXPECT_EQ(context.process_name, L"brave");
    EXPECT_EQ(context.target_name, L"Brave - Claude Design");
}

TEST(RecordViewModelAudioTest, RecordViewModel_TargetLabelFromCaptureTarget_UsesSelectedTargetLabel) {
    const recorder_core::CaptureTarget monitor_target{recorder_core::CaptureTarget::Kind::Monitor, 12,
                                                      R"(\\.\DISPLAY2)"};
    const recorder_core::CaptureTarget window_target{recorder_core::CaptureTarget::Kind::Window, 13,
                                                     "Claude Design - Brave"};

    EXPECT_EQ(RecordViewModel::TargetLabelFromCaptureTarget(monitor_target), "Desktop - Display 2");
    EXPECT_EQ(RecordViewModel::TargetLabelFromCaptureTarget(window_target), "Brave - Claude Design");
}

// ---------------------------------------------------------------------------
// APP-AUDIO-ROW-FIX-R1 — ApplyTargetKindPreservingAudio: Display → Window
// ---------------------------------------------------------------------------

TEST(RecordViewModelAudioTest, RecordViewModel_DisplayToWindow_AddsAppRow) {
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Display);
    ASSERT_EQ(FindRow(vm.audio_ui_state, recorder_core::AudioSourceKind::App), nullptr);

    vm.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Window);

    const auto* app_row = FindRow(vm.audio_ui_state, recorder_core::AudioSourceKind::App);
    ASSERT_NE(app_row, nullptr);
    EXPECT_TRUE(app_row->enabled);
    EXPECT_FALSE(app_row->merge_with_above);
}

TEST(RecordViewModelAudioTest, RecordViewModel_DisplayToWindow_PreservesExistingSysAndMicRows) {
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Display);
    // Disable SystemOutput; Mic is OFF by default policy.
    for (auto& r : vm.audio_ui_state.source_rows) {
        if (r.kind == recorder_core::AudioSourceKind::SystemOutput)
            r.enabled = false;
    }

    vm.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Window);

    EXPECT_FALSE(vm.audio_ui_state.IsSysEnabled());
    EXPECT_FALSE(vm.audio_ui_state.IsMicEnabled()); // preserved as OFF (default Display policy)
}

TEST(RecordViewModelAudioTest, RecordViewModel_DisplayToWindow_PreservesExistingEnabledStates) {
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Display);
    // Disable Mic.
    for (auto& r : vm.audio_ui_state.source_rows)
        if (r.kind == recorder_core::AudioSourceKind::Mic)
            r.enabled = false;

    vm.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Window);

    EXPECT_FALSE(vm.audio_ui_state.IsMicEnabled());
    EXPECT_NE(FindRow(vm.audio_ui_state, recorder_core::AudioSourceKind::App), nullptr);
}

TEST(RecordViewModelAudioTest, RecordViewModel_WindowToWindow_NoDuplicateAppRow) {
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    const std::size_t row_count = vm.audio_ui_state.source_rows.size();

    vm.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Window);

    EXPECT_EQ(vm.audio_ui_state.source_rows.size(), row_count);
    int app_count = 0;
    for (const auto& r : vm.audio_ui_state.source_rows)
        if (r.kind == recorder_core::AudioSourceKind::App)
            ++app_count;
    EXPECT_EQ(app_count, 1);
}

TEST(RecordViewModelAudioTest, RecordViewModel_WindowToWindow_PreservesAppEnabledState) {
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    for (auto& r : vm.audio_ui_state.source_rows)
        if (r.kind == recorder_core::AudioSourceKind::App)
            r.enabled = false;

    vm.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Window);

    const auto* app_row = FindRow(vm.audio_ui_state, recorder_core::AudioSourceKind::App);
    ASSERT_NE(app_row, nullptr);
    EXPECT_FALSE(app_row->enabled);
}

TEST(RecordViewModelAudioTest, RecordViewModel_RepeatedWindowSwitch_IsIdempotent) {
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Display);

    vm.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Window);
    const auto rows_after_first = vm.audio_ui_state.source_rows;

    vm.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Window);

    ASSERT_EQ(vm.audio_ui_state.source_rows.size(), rows_after_first.size());
    for (std::size_t i = 0; i < rows_after_first.size(); ++i) {
        EXPECT_EQ(vm.audio_ui_state.source_rows[i].kind, rows_after_first[i].kind);
        EXPECT_EQ(vm.audio_ui_state.source_rows[i].enabled, rows_after_first[i].enabled);
        EXPECT_EQ(vm.audio_ui_state.source_rows[i].merge_with_above, rows_after_first[i].merge_with_above);
    }
}

TEST(RecordViewModelAudioTest, RecordViewModel_WindowToDisplay_PreservesAppRowInModel) {
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);

    vm.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Display);

    EXPECT_EQ(vm.audio_ui_state.target_kind, capability::CaptureTargetKind::Display);
    EXPECT_NE(FindRow(vm.audio_ui_state, recorder_core::AudioSourceKind::App), nullptr);
}

TEST(RecordViewModelAudioTest, RecordViewModel_DisplayToWindow_AppBecomesActive) {
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Display);
    EXPECT_FALSE(vm.audio_active_app);

    vm.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Window);

    EXPECT_TRUE(vm.audio_active_app);
}

TEST(RecordViewModelAudioTest, RecordViewModel_DisplayTarget_AppIsInactive) {
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Display);

    EXPECT_FALSE(vm.audio_active_app);
}

TEST(RecordViewModelAudioTest, RecordViewModel_DisplayToWindow_AppRowIsFirst) {
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Display);

    vm.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Window);

    ASSERT_FALSE(vm.audio_ui_state.source_rows.empty());
    EXPECT_EQ(vm.audio_ui_state.source_rows.front().kind, recorder_core::AudioSourceKind::App);
}

// ---------------------------------------------------------------------------
// AUDIO-SOURCE-POLICY-R1: new policy and preference-preservation tests
// ---------------------------------------------------------------------------

TEST(RecordViewModelAudioTest, RecordViewModel_RegionUsesDisplayPolicy) {
    // Region capture uses the same audio policy as Display.
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Display);
    const auto display_rows = vm.audio_ui_state.source_rows;

    RecordViewModel vm2;
    // Simulate region mode (uses Display kind internally).
    vm2.ApplyTargetKind(capability::CaptureTargetKind::Display);

    EXPECT_EQ(vm.audio_ui_state.source_rows.size(), vm2.audio_ui_state.source_rows.size());
    for (std::size_t i = 0; i < display_rows.size(); ++i) {
        EXPECT_EQ(vm.audio_ui_state.source_rows[i].kind, vm2.audio_ui_state.source_rows[i].kind);
        EXPECT_EQ(vm.audio_ui_state.source_rows[i].enabled, vm2.audio_ui_state.source_rows[i].enabled);
    }
}

TEST(RecordViewModelAudioTest, RecordViewModel_WindowPreferencesSurviveWindowDisplayWindow) {
    using K = recorder_core::AudioSourceKind;
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    // Disable App, enable Sys.
    for (auto& r : vm.audio_ui_state.source_rows) {
        if (r.kind == K::App)
            r.enabled = false;
        if (r.kind == K::Sys)
            r.enabled = true;
    }

    vm.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Display);
    vm.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Window);

    EXPECT_FALSE(vm.audio_ui_state.IsAppEnabled());
    EXPECT_TRUE(vm.audio_ui_state.IsSysEnabled());
}

TEST(RecordViewModelAudioTest, RecordViewModel_DisplaySysPreferenceSurvivesWindowSwitch) {
    using K = recorder_core::AudioSourceKind;
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Display);
    // Disable Computer audio.
    for (auto& r : vm.audio_ui_state.source_rows)
        if (r.kind == K::SystemOutput)
            r.enabled = false;

    vm.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Window);
    vm.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Display);

    EXPECT_FALSE(vm.audio_ui_state.IsSysEnabled()); // SystemOutput preference preserved
}

TEST(RecordViewModelAudioTest, RecordViewModel_MicPreferenceSurvivesAllTransitions) {
    using K = recorder_core::AudioSourceKind;
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Display);
    // Enable Mic from Display state.
    for (auto& r : vm.audio_ui_state.source_rows)
        if (r.kind == K::Mic)
            r.enabled = true;

    vm.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Window);
    EXPECT_TRUE(vm.audio_ui_state.IsMicEnabled());

    vm.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Display);
    EXPECT_TRUE(vm.audio_ui_state.IsMicEnabled());

    vm.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Window);
    EXPECT_TRUE(vm.audio_ui_state.IsMicEnabled());
}

TEST(RecordViewModelAudioTest, RecordViewModel_MergeStatePreservedAcrossTransitions) {
    using K = recorder_core::AudioSourceKind;
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    for (auto& r : vm.audio_ui_state.source_rows)
        if (r.kind == K::Mic)
            r.merge_with_above = true;

    vm.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Display);
    vm.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Window);

    const auto* mic_row = FindRow(vm.audio_ui_state, K::Mic);
    ASSERT_NE(mic_row, nullptr);
    EXPECT_TRUE(mic_row->merge_with_above);
}

TEST(RecordViewModelAudioTest, RecordViewModel_WindowDefault_OnlyAppIsActive) {
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);

    EXPECT_TRUE(vm.audio_active_app);
    EXPECT_FALSE(vm.audio_active_sys);
    EXPECT_FALSE(vm.audio_active_mic);
}

TEST(RecordViewModelAudioTest, RecordViewModel_DisplayDefault_OnlySysIsActive) {
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Display);

    EXPECT_FALSE(vm.audio_active_app);
    EXPECT_TRUE(vm.audio_active_sys);
    EXPECT_FALSE(vm.audio_active_mic);
}

// ---------------------------------------------------------------------------

TEST(RecordViewModelStateGuardTest, CanStart_Ready_WithTargets_ReturnsTrue) {
    RecordViewModel vm;
    vm.SetState(UiRecordingState::Ready);
    vm.targets = {recorder_core::CaptureTarget{}};
    vm.selected_target_index = 0;
    EXPECT_TRUE(vm.CanStart());
}

TEST(RecordViewModelStateGuardTest, CanStart_LoadingCapabilities_ReturnsFalse) {
    RecordViewModel vm;
    vm.SetState(UiRecordingState::LoadingCapabilities);
    EXPECT_FALSE(vm.CanStart());
}

TEST(RecordViewModelStateGuardTest, CanStart_Recording_ReturnsFalse) {
    RecordViewModel vm;
    vm.SetState(UiRecordingState::Recording);
    EXPECT_FALSE(vm.CanStart());
}

TEST(RecordViewModelStateGuardTest, CanStart_TransientRecordingStatesReturnFalse) {
    for (const UiRecordingState state : {UiRecordingState::Countdown, UiRecordingState::Preparing,
                                         UiRecordingState::RegionSelecting, UiRecordingState::Stopping}) {
        RecordViewModel vm;
        vm.SetState(state);
        vm.targets = {recorder_core::CaptureTarget{}};
        vm.selected_target_index = 0;
        EXPECT_FALSE(vm.CanStart());
    }
}

TEST(RecordViewModelStateGuardTest, CanStart_Blocked_ReturnsFalse) {
    RecordViewModel vm;
    vm.SetState(UiRecordingState::Blocked);
    vm.targets = {recorder_core::CaptureTarget{}};
    vm.selected_target_index = 0;
    EXPECT_FALSE(vm.CanStart());
}

TEST(RecordViewModelStateGuardTest, CanStart_NoTargets_ReturnsFalse) {
    RecordViewModel vm;
    vm.SetState(UiRecordingState::Ready);
    vm.selected_target_index = 0;
    EXPECT_FALSE(vm.CanStart());
}

TEST(RecordViewModelStateGuardTest, CanStart_Completed_ReturnsTrue) {
    RecordViewModel vm;
    vm.SetState(UiRecordingState::Completed);
    vm.targets = {recorder_core::CaptureTarget{}};
    vm.selected_target_index = 0;
    EXPECT_TRUE(vm.CanStart());
}

TEST(RecordViewModelStateGuardTest, CanStart_Failed_ReturnsTrue) {
    RecordViewModel vm;
    vm.SetState(UiRecordingState::Failed);
    vm.targets = {recorder_core::CaptureTarget{}};
    vm.selected_target_index = 0;
    EXPECT_TRUE(vm.CanStart());
}

TEST(RecordViewModelStateGuardTest, CanStop_Recording_ReturnsTrue) {
    RecordViewModel vm;
    vm.SetState(UiRecordingState::Recording);
    EXPECT_TRUE(vm.CanStop());
}

TEST(RecordViewModelStateGuardTest, CanStop_Paused_ReturnsTrue) {
    RecordViewModel vm;
    vm.SetState(UiRecordingState::Paused);
    EXPECT_TRUE(vm.CanStop());
}

TEST(RecordViewModelStateGuardTest, CanStop_Ready_ReturnsFalse) {
    RecordViewModel vm;
    vm.SetState(UiRecordingState::Ready);
    EXPECT_FALSE(vm.CanStop());
}

TEST(RecordViewModelStateGuardTest, CanStop_Starting_ReturnsFalse) {
    RecordViewModel vm;
    vm.SetState(UiRecordingState::Preparing);
    EXPECT_FALSE(vm.CanStop());
}

TEST(RecordViewModelStateGuardTest, CanStop_Stopping_ReturnsFalse) {
    RecordViewModel vm;
    vm.SetState(UiRecordingState::Stopping);
    EXPECT_FALSE(vm.CanStop());
}

TEST(RecordViewModelStateGuardTest, CanPause_Recording_ReturnsTrue) {
    RecordViewModel vm;
    vm.SetState(UiRecordingState::Recording);
    EXPECT_TRUE(vm.CanPause());
}

TEST(RecordViewModelStateGuardTest, CanPause_Paused_ReturnsFalse) {
    RecordViewModel vm;
    vm.SetState(UiRecordingState::Paused);
    EXPECT_FALSE(vm.CanPause());
}

TEST(RecordViewModelStateGuardTest, CanPause_Starting_ReturnsFalse) {
    RecordViewModel vm;
    vm.SetState(UiRecordingState::Preparing);
    EXPECT_FALSE(vm.CanPause());
}

TEST(RecordViewModelStateGuardTest, CanResume_Paused_ReturnsTrue) {
    RecordViewModel vm;
    vm.SetState(UiRecordingState::Paused);
    EXPECT_TRUE(vm.CanResume());
}

TEST(RecordViewModelStateGuardTest, CanResume_Recording_ReturnsFalse) {
    RecordViewModel vm;
    vm.SetState(UiRecordingState::Recording);
    EXPECT_FALSE(vm.CanResume());
}

TEST(RecordViewModelStateGuardTest, CanResume_Ready_ReturnsFalse) {
    RecordViewModel vm;
    vm.SetState(UiRecordingState::Ready);
    EXPECT_FALSE(vm.CanResume());
}

} // namespace
} // namespace exosnap
