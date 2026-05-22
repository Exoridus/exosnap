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
    EXPECT_TRUE(vm.audio_ui_state.separate_output_tracks);
    EXPECT_FALSE(vm.audio_ui_state.record_microphone);

    ASSERT_EQ(vm.audio_track_preview.size(), 2u);
    EXPECT_EQ(vm.audio_track_preview[0].source_key, "app");
    EXPECT_EQ(vm.audio_track_preview[1].source_key, "sys");
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
    ASSERT_EQ(vm.audio_track_preview.size(), 2u);

    vm.audio_ui_state.record_system_audio = false;
    vm.RebuildAudioPlan();
    ASSERT_EQ(vm.audio_track_preview.size(), 1u);
    EXPECT_EQ(vm.audio_track_preview[0].source_key, "app");

    vm.audio_ui_state.record_application_audio = false;
    vm.RebuildAudioPlan();
    EXPECT_TRUE(vm.audio_track_preview.empty());
}

TEST(RecordViewModelAudioTest, RecordViewModel_TrackPreviewUpdatesOnMicToggle) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    ASSERT_EQ(vm.audio_track_preview.size(), 2u);

    vm.audio_ui_state.record_microphone = true;
    vm.RebuildAudioPlan();
    ASSERT_EQ(vm.audio_track_preview.size(), 3u);
    EXPECT_EQ(vm.audio_track_preview.back().source_key, "mic");

    vm.audio_ui_state.record_microphone = false;
    vm.RebuildAudioPlan();
    ASSERT_EQ(vm.audio_track_preview.size(), 2u);
}

TEST(RecordViewModelAudioTest, RecordViewModel_RebuildAudioPlan_PropagatesMicDeviceId) {
    RecordViewModel vm;

    vm.audio_ui_state.selected_mic_device_id = "device-123";
    vm.RebuildAudioPlan();

    ASSERT_TRUE(vm.audio_plan.mic_device_id.has_value());
    EXPECT_EQ(vm.audio_plan.mic_device_id.value(), "device-123");
}

TEST(RecordViewModelAudioTest, RecordViewModel_RebuildAudioPlan_SetsActiveFlagsForWindowTracks) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);

    EXPECT_TRUE(vm.audio_active_app);
    EXPECT_TRUE(vm.audio_active_sys);
    EXPECT_FALSE(vm.audio_active_mic);

    vm.audio_ui_state.record_microphone = true;
    vm.RebuildAudioPlan();

    EXPECT_TRUE(vm.audio_active_mic);
}

TEST(RecordViewModelAudioTest, RecordViewModel_RebuildAudioPlan_SystemOutputActivatesSysMeter) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    vm.audio_ui_state.record_application_audio = true;
    vm.audio_ui_state.record_system_audio = true;
    vm.audio_ui_state.separate_output_tracks = false;
    vm.RebuildAudioPlan();

    EXPECT_TRUE(vm.audio_active_sys);
    EXPECT_FALSE(vm.audio_active_app);
    EXPECT_FALSE(vm.audio_active_mic);

    const bool has_system_output =
        std::any_of(vm.audio_track_preview.begin(), vm.audio_track_preview.end(),
                    [](const capability::AudioTrackPreview& preview) { return preview.source_key == "system_output"; });
    EXPECT_TRUE(has_system_output);
}

TEST(RecordViewModelAudioTest, RecordViewModel_UpdateStats_MapsPerTrackRmsToSources) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);

    recorder_core::SessionStats stats;
    stats.per_track_rms[0] = 0.25f;
    stats.per_track_rms[1] = 0.50f;

    vm.UpdateStats(stats);

    EXPECT_FLOAT_EQ(vm.audio_rms_app, 0.25f);
    EXPECT_FLOAT_EQ(vm.audio_rms_sys, 0.50f);
    EXPECT_FLOAT_EQ(vm.audio_rms_mic, 0.0f);
}

TEST(RecordViewModelAudioTest, RecordViewModel_UpdateStats_MapsMicRms) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    vm.audio_ui_state.record_microphone = true;
    vm.RebuildAudioPlan();

    recorder_core::SessionStats stats;
    stats.per_track_rms[2] = 0.75f;

    vm.UpdateStats(stats);

    EXPECT_FLOAT_EQ(vm.audio_rms_mic, 0.75f);
}

TEST(RecordViewModelAudioTest, RecordViewModel_UpdateStats_SystemOutputMapsToSys) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    vm.audio_ui_state.record_application_audio = true;
    vm.audio_ui_state.record_system_audio = true;
    vm.audio_ui_state.separate_output_tracks = false;
    vm.RebuildAudioPlan();

    recorder_core::SessionStats stats;
    stats.per_track_rms[0] = 0.4f;

    vm.UpdateStats(stats);

    EXPECT_FLOAT_EQ(vm.audio_rms_sys, 0.4f);
    EXPECT_FLOAT_EQ(vm.audio_rms_app, 0.0f);
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

} // namespace
} // namespace exosnap
