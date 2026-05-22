#include <gtest/gtest.h>

#include <capability/audio_track_preview.h>
#include <capability/audio_ui_state.h>

namespace exosnap::capability {
namespace {

using recorder_core::AudioSourceKind;

TEST(AudioUiStateTest, BuildAudioPlan_WindowMerged_NoMic_SingleTrackAppSys) {
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Window;
    state.record_application_audio = true;
    state.record_system_audio = true;
    state.separate_output_tracks = false;
    state.record_microphone = false;
    state.selected_window_pid = 1000u;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 1u);
    const auto& track = result.plan.tracks[0];
    ASSERT_EQ(track.sources.size(), 2u);
    EXPECT_EQ(track.sources[0], AudioSourceKind::App);
    EXPECT_EQ(track.sources[1], AudioSourceKind::Sys);
    ASSERT_TRUE(result.audio_target_process_id.has_value());
    EXPECT_EQ(result.audio_target_process_id.value(), 1000u);
}

TEST(AudioUiStateTest, BuildAudioPlan_WindowMerged_WithMic_SingleTrackAppSysMic) {
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Window;
    state.record_application_audio = true;
    state.record_system_audio = true;
    state.separate_output_tracks = false;
    state.record_microphone = true;
    state.selected_window_pid = 1001u;

    const AudioPlanResult result = BuildAudioPlan(state);
    ASSERT_EQ(result.plan.tracks.size(), 1u);
    const auto& track = result.plan.tracks[0];
    ASSERT_EQ(track.sources.size(), 3u);
    EXPECT_EQ(track.sources[0], AudioSourceKind::App);
    EXPECT_EQ(track.sources[1], AudioSourceKind::Sys);
    EXPECT_EQ(track.sources[2], AudioSourceKind::Mic);
    ASSERT_TRUE(result.audio_target_process_id.has_value());
    EXPECT_EQ(result.audio_target_process_id.value(), 1001u);
}

TEST(AudioUiStateTest, BuildAudioPlan_WindowSeparate_WithMic_ThreeTracks) {
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Window;
    state.record_application_audio = true;
    state.record_system_audio = true;
    state.separate_output_tracks = true;
    state.record_microphone = true;
    state.selected_window_pid = 1002u;

    const AudioPlanResult result = BuildAudioPlan(state);
    ASSERT_EQ(result.plan.tracks.size(), 3u);
    ASSERT_EQ(result.plan.tracks[0].sources.size(), 1u);
    EXPECT_EQ(result.plan.tracks[0].sources[0], AudioSourceKind::App);
    ASSERT_EQ(result.plan.tracks[1].sources.size(), 1u);
    EXPECT_EQ(result.plan.tracks[1].sources[0], AudioSourceKind::Sys);
    ASSERT_EQ(result.plan.tracks[2].sources.size(), 1u);
    EXPECT_EQ(result.plan.tracks[2].sources[0], AudioSourceKind::Mic);
    ASSERT_TRUE(result.audio_target_process_id.has_value());
    EXPECT_EQ(result.audio_target_process_id.value(), 1002u);
}

TEST(AudioUiStateTest, BuildAudioPlan_Display_WithMic_SingleTrackSystemOutputMic) {
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Display;
    state.record_system_audio = true;
    state.record_microphone = true;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 1u);
    const auto& track = result.plan.tracks[0];
    ASSERT_EQ(track.sources.size(), 2u);
    EXPECT_EQ(track.sources[0], AudioSourceKind::SystemOutput);
    EXPECT_EQ(track.sources[1], AudioSourceKind::Mic);
    EXPECT_FALSE(result.audio_target_process_id.has_value());
}

TEST(AudioUiStateTest, BuildAudioPlan_Display_NoMic_SingleSystemOutputTrack) {
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Display;
    state.record_system_audio = true;
    state.record_microphone = false;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 1u);
    ASSERT_EQ(result.plan.tracks[0].sources.size(), 1u);
    EXPECT_EQ(result.plan.tracks[0].sources[0], AudioSourceKind::SystemOutput);
    EXPECT_FALSE(result.audio_target_process_id.has_value());
}

TEST(AudioUiStateTest, BuildAudioPlan_Merged_PropagatesMicGain) {
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Window;
    state.record_application_audio = true;
    state.record_system_audio = true;
    state.separate_output_tracks = false;
    state.record_microphone = true;
    state.mic_gain_linear = 1.5f;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_FLOAT_EQ(result.mic_gain_linear, 1.5f);
}

TEST(AudioUiStateTest, BuildAudioTrackPreview_MergedTrack_SinglePreviewEntry) {
    AudioPlanResult result;
    result.record_audio = true;

    recorder_core::ResolvedAudioTrack merged;
    merged.track_index = 0;
    merged.sources = {AudioSourceKind::App, AudioSourceKind::Sys, AudioSourceKind::Mic};
    result.plan.tracks.push_back(std::move(merged));

    const std::vector<AudioTrackPreview> preview = BuildAudioTrackPreview(result);
    ASSERT_EQ(preview.size(), 1u);
    EXPECT_EQ(preview[0].track_number, 1u);
    EXPECT_EQ(preview[0].source_key, "merged");
    EXPECT_EQ(preview[0].display_label, "Mixed Audio");
}

} // namespace
} // namespace exosnap::capability
