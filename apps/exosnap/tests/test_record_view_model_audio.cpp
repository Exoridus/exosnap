#include <gtest/gtest.h>

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

TEST(RecordViewModelAudioTest, RecordViewModel_TrackPreviewDisplayTarget_SystemOutput) {
    RecordViewModel vm;

    vm.ApplyTargetKind(capability::CaptureTargetKind::Display);

    ASSERT_EQ(vm.audio_track_preview.size(), 1u);
    EXPECT_EQ(vm.audio_track_preview[0].source_key, "system_output");
    EXPECT_EQ(vm.audio_track_preview[0].display_label, "System Audio");
}

} // namespace
} // namespace exosnap
