#include <gtest/gtest.h>

#include <algorithm>

#include "viewmodels/RecordViewModel.h"

namespace exosnap {
namespace {

TEST(RecordViewModelAudioTest, RecordViewModel_DefaultAudioStateForWindowTarget) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);

    EXPECT_EQ(vm.audio_ui_state.target_kind, capability::CaptureTargetKind::Window);
    EXPECT_TRUE(vm.audio_ui_state.record_application_audio);
    EXPECT_TRUE(vm.audio_ui_state.record_system_audio);
    EXPECT_FALSE(vm.audio_ui_state.separate_output_tracks); // merge-first default
    EXPECT_FALSE(vm.audio_ui_state.record_microphone);

    // Merge-first: one merged track {App, Sys}
    ASSERT_EQ(vm.audio_track_preview.size(), 1u);
    EXPECT_EQ(vm.audio_track_preview[0].source_key, "merged");
}

TEST(RecordViewModelAudioTest, RecordViewModel_DefaultMicGainLinear_IsUnity) {
    RecordViewModel vm;
    EXPECT_FLOAT_EQ(vm.audio_ui_state.mic_gain_linear, 1.0f);
}

TEST(RecordViewModelAudioTest, RecordViewModel_DefaultAudioStateForDisplayTarget) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Display);

    EXPECT_EQ(vm.audio_ui_state.target_kind, capability::CaptureTargetKind::Display);
    EXPECT_FALSE(vm.audio_ui_state.record_application_audio);
    EXPECT_TRUE(vm.audio_ui_state.record_system_audio);
    EXPECT_FALSE(vm.audio_ui_state.separate_output_tracks);
    EXPECT_FALSE(vm.audio_ui_state.record_microphone);

    ASSERT_EQ(vm.audio_track_preview.size(), 1u);
    EXPECT_EQ(vm.audio_track_preview[0].source_key, "system_output");
}

TEST(RecordViewModelAudioTest, RecordViewModel_ApplyTargetKind_DisplayClearsAppState) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    vm.audio_ui_state.record_application_audio = true;
    vm.audio_ui_state.separate_output_tracks = true;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Display);

    EXPECT_FALSE(vm.audio_ui_state.record_application_audio);
    EXPECT_FALSE(vm.audio_ui_state.separate_output_tracks);
    EXPECT_TRUE(vm.audio_ui_state.record_system_audio);
}

TEST(RecordViewModelAudioTest, RecordViewModel_TrackPreviewUpdatesOnOutputToggles) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    // Merge-first: 1 merged track {App, Sys}
    ASSERT_EQ(vm.audio_track_preview.size(), 1u);
    EXPECT_EQ(vm.audio_track_preview[0].source_key, "merged");

    // Disable sys: 1 track {App} -> single-source -> "app" key
    vm.audio_ui_state.record_system_audio = false;
    vm.RebuildAudioPlan();
    ASSERT_EQ(vm.audio_track_preview.size(), 1u);
    EXPECT_EQ(vm.audio_track_preview[0].source_key, "app");

    // Disable app too: 0 tracks
    vm.audio_ui_state.record_application_audio = false;
    vm.RebuildAudioPlan();
    EXPECT_TRUE(vm.audio_track_preview.empty());
}

TEST(RecordViewModelAudioTest, RecordViewModel_TrackPreviewUpdatesOnMicToggle) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    // Merge-first: 1 merged track {App, Sys}
    ASSERT_EQ(vm.audio_track_preview.size(), 1u);
    EXPECT_EQ(vm.audio_track_preview[0].source_key, "merged");

    // Enable mic: merged track becomes {App, Sys, Mic} -> still 1 merged preview row
    vm.audio_ui_state.record_microphone = true;
    vm.RebuildAudioPlan();
    ASSERT_EQ(vm.audio_track_preview.size(), 1u);
    EXPECT_EQ(vm.audio_track_preview.back().source_key, "merged");

    // Disable mic: back to {App, Sys} -> 1 merged preview row
    vm.audio_ui_state.record_microphone = false;
    vm.RebuildAudioPlan();
    ASSERT_EQ(vm.audio_track_preview.size(), 1u);
    EXPECT_EQ(vm.audio_track_preview[0].source_key, "merged");
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
    // Merge-first: merged track -> active flags derived from ui state
    EXPECT_TRUE(vm.audio_active_app);
    EXPECT_TRUE(vm.audio_active_sys);
    EXPECT_FALSE(vm.audio_active_mic);

    vm.audio_ui_state.record_microphone = true;
    vm.RebuildAudioPlan();
    EXPECT_TRUE(vm.audio_active_mic);
}

TEST(RecordViewModelAudioTest, RecordViewModel_RebuildAudioPlan_MergedWindowActivatesBothMeters) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    // Default: sep=false, app=true, sys=true -> merged track {App, Sys}
    // Both app and sys meters are active via merged track.
    EXPECT_TRUE(vm.audio_active_sys);
    EXPECT_TRUE(vm.audio_active_app);
    EXPECT_FALSE(vm.audio_active_mic);

    const bool has_merged =
        std::any_of(vm.audio_track_preview.begin(), vm.audio_track_preview.end(),
                    [](const capability::AudioTrackPreview& preview) { return preview.source_key == "merged"; });
    EXPECT_TRUE(has_merged);
}

TEST(RecordViewModelAudioTest, RecordViewModel_UpdateStats_MapsPerTrackRmsToSources) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    // Merge-first: 1 merged track {App, Sys}. Single RMS value distributed to both meters.

    recorder_core::SessionStats stats;
    stats.per_track_rms[0] = 0.25f;

    vm.UpdateStats(stats);

    EXPECT_FLOAT_EQ(vm.audio_rms_app, 0.25f);
    EXPECT_FLOAT_EQ(vm.audio_rms_sys, 0.25f);
    EXPECT_FLOAT_EQ(vm.audio_rms_mic, 0.0f);
}

TEST(RecordViewModelAudioTest, RecordViewModel_UpdateStats_MapsMicRms) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    vm.audio_ui_state.record_microphone = true;
    vm.RebuildAudioPlan();
    // Merge-first: 1 merged track {App, Sys, Mic}. RMS at index 0 goes to all active meters.

    recorder_core::SessionStats stats;
    stats.per_track_rms[0] = 0.75f;

    vm.UpdateStats(stats);

    EXPECT_FLOAT_EQ(vm.audio_rms_app, 0.75f);
    EXPECT_FLOAT_EQ(vm.audio_rms_sys, 0.75f);
    EXPECT_FLOAT_EQ(vm.audio_rms_mic, 0.75f);
}

TEST(RecordViewModelAudioTest, RecordViewModel_UpdateStats_MergedWindowRmsGoesToBothMeters) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    // Default: sep=false, app=true, sys=true -> merged {App, Sys}

    recorder_core::SessionStats stats;
    stats.per_track_rms[0] = 0.4f;

    vm.UpdateStats(stats);

    EXPECT_FLOAT_EQ(vm.audio_rms_sys, 0.4f);
    EXPECT_FLOAT_EQ(vm.audio_rms_app, 0.4f); // both meters get the merged RMS
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

    ASSERT_EQ(vm.audio_track_preview.size(), 1u);
    EXPECT_EQ(vm.audio_track_preview[0].source_key, "system_output");
    EXPECT_EQ(vm.audio_track_preview[0].display_label, "System Audio");
}

TEST(RecordViewModelAudioTest, RecordViewModel_ApplyTargetKindPreservingAudio_SameKindAndSwitchKeepUserState) {
    RecordViewModel vm;

    vm.audio_ui_state.record_application_audio = true;
    vm.audio_ui_state.record_system_audio = false;
    vm.audio_ui_state.separate_output_tracks = false;
    vm.audio_ui_state.record_microphone = true;
    vm.audio_ui_state.mic_channel_mode = recorder_core::MicChannelMode::MonoMix;

    vm.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Display);

    EXPECT_EQ(vm.audio_ui_state.target_kind, capability::CaptureTargetKind::Display);
    EXPECT_TRUE(vm.audio_ui_state.record_application_audio);
    EXPECT_FALSE(vm.audio_ui_state.record_system_audio);
    EXPECT_FALSE(vm.audio_ui_state.separate_output_tracks);
    EXPECT_TRUE(vm.audio_ui_state.record_microphone);
    EXPECT_EQ(vm.audio_ui_state.mic_channel_mode, recorder_core::MicChannelMode::MonoMix);

    vm.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Window);

    EXPECT_EQ(vm.audio_ui_state.target_kind, capability::CaptureTargetKind::Window);
    EXPECT_TRUE(vm.audio_ui_state.record_application_audio);
    EXPECT_FALSE(vm.audio_ui_state.record_system_audio);
    EXPECT_FALSE(vm.audio_ui_state.separate_output_tracks);
    EXPECT_TRUE(vm.audio_ui_state.record_microphone);
    EXPECT_EQ(vm.audio_ui_state.mic_channel_mode, recorder_core::MicChannelMode::MonoMix);
}

TEST(RecordViewModelAudioTest, RecordViewModel_ApplyTargetKindPreservingAudio_WindowModeKeepsAppAvailability) {
    RecordViewModel vm;

    vm.audio_ui_state.record_application_audio = true;
    vm.audio_ui_state.record_system_audio = true;
    vm.audio_ui_state.separate_output_tracks = true;

    vm.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Window);

    const bool has_app_track =
        std::any_of(vm.audio_track_preview.begin(), vm.audio_track_preview.end(),
                    [](const capability::AudioTrackPreview& preview) { return preview.source_key == "app"; });
    EXPECT_TRUE(has_app_track);
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

} // namespace
} // namespace exosnap
