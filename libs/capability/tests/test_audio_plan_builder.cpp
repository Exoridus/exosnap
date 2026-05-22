#include <gtest/gtest.h>

#include <capability/audio_track_preview.h>
#include <capability/audio_ui_state.h>

#include <optional>
#include <vector>

namespace exosnap::capability {
namespace {

using recorder_core::AudioSourceKind;

void ExpectSingleSourceTrack(const AudioPlanResult& result, size_t track_pos, AudioSourceKind expected_kind) {
    ASSERT_LT(track_pos, result.plan.tracks.size());
    const auto& track = result.plan.tracks[track_pos];
    ASSERT_EQ(track.sources.size(), 1u);
    EXPECT_EQ(track.sources.front(), expected_kind);
}

TEST(AudioPlanBuilderTest, BuildAudioPlan_PropagatesSelectedMicDeviceId) {
    AudioUiState state;
    state.record_microphone = true;
    state.selected_mic_device_id = "test-device-id";

    const AudioPlanResult result = BuildAudioPlan(state);
    ASSERT_TRUE(result.mic_device_id.has_value());
    EXPECT_EQ(result.mic_device_id.value(), "test-device-id");
}

TEST(AudioPlanBuilderTest, BuildAudioPlan_DefaultMicDeviceIdIsNullopt) {
    AudioUiState state;
    state.selected_mic_device_id = std::nullopt;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_FALSE(result.mic_device_id.has_value());
}

TEST(AudioPlanBuilderTest, WindowTarget_AppOnly) {
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Window;
    state.record_application_audio = true;
    state.record_system_audio = false;
    state.record_microphone = false;
    state.selected_window_pid = 1001u;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 1u);
    ExpectSingleSourceTrack(result, 0, AudioSourceKind::App);
    ASSERT_TRUE(result.audio_target_process_id.has_value());
    EXPECT_EQ(result.audio_target_process_id.value(), 1001u);
}

TEST(AudioPlanBuilderTest, WindowTarget_SysOnly) {
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Window;
    state.record_application_audio = false;
    state.record_system_audio = true;
    state.record_microphone = false;
    state.selected_window_pid = 1002u;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 1u);
    ExpectSingleSourceTrack(result, 0, AudioSourceKind::Sys);
    ASSERT_TRUE(result.audio_target_process_id.has_value());
    EXPECT_EQ(result.audio_target_process_id.value(), 1002u);
}

TEST(AudioPlanBuilderTest, WindowTarget_AppAndSys_Separate) {
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Window;
    state.record_application_audio = true;
    state.record_system_audio = true;
    state.separate_output_tracks = true;
    state.selected_window_pid = 1003u;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 2u);
    ExpectSingleSourceTrack(result, 0, AudioSourceKind::App);
    ExpectSingleSourceTrack(result, 1, AudioSourceKind::Sys);
    ASSERT_TRUE(result.audio_target_process_id.has_value());
    EXPECT_EQ(result.audio_target_process_id.value(), 1003u);
}

TEST(AudioPlanBuilderTest, WindowTarget_AppAndSys_Combined) {
    // Merge-first: {App, Sys} as one merged track with PID set.
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Window;
    state.record_application_audio = true;
    state.record_system_audio = true;
    state.separate_output_tracks = false;
    state.selected_window_pid = 1004u;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 1u);
    const auto& track = result.plan.tracks[0];
    ASSERT_EQ(track.sources.size(), 2u);
    EXPECT_EQ(track.sources[0], AudioSourceKind::App);
    EXPECT_EQ(track.sources[1], AudioSourceKind::Sys);
    ASSERT_TRUE(result.audio_target_process_id.has_value());
    EXPECT_EQ(result.audio_target_process_id.value(), 1004u);
}

TEST(AudioPlanBuilderTest, WindowTarget_AppAndMic) {
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Window;
    state.record_application_audio = true;
    state.record_system_audio = false;
    state.record_microphone = true;
    state.selected_window_pid = 1005u;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 2u);
    ExpectSingleSourceTrack(result, 0, AudioSourceKind::App);
    ExpectSingleSourceTrack(result, 1, AudioSourceKind::Mic);
    ASSERT_TRUE(result.audio_target_process_id.has_value());
    EXPECT_EQ(result.audio_target_process_id.value(), 1005u);
}

TEST(AudioPlanBuilderTest, WindowTarget_SysAndMic) {
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Window;
    state.record_application_audio = false;
    state.record_system_audio = true;
    state.record_microphone = true;
    state.selected_window_pid = 1006u;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 2u);
    ExpectSingleSourceTrack(result, 0, AudioSourceKind::Sys);
    ExpectSingleSourceTrack(result, 1, AudioSourceKind::Mic);
    ASSERT_TRUE(result.audio_target_process_id.has_value());
    EXPECT_EQ(result.audio_target_process_id.value(), 1006u);
}

TEST(AudioPlanBuilderTest, WindowTarget_AppSysMic_Merged) {
    // Merge-first: {App, Sys, Mic} as one merged track, Mic last, PID set.
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Window;
    state.record_application_audio = true;
    state.record_system_audio = true;
    state.separate_output_tracks = false;
    state.record_microphone = true;
    state.selected_window_pid = 1007u;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 1u);
    const auto& track = result.plan.tracks[0];
    ASSERT_EQ(track.sources.size(), 3u);
    EXPECT_EQ(track.sources[0], AudioSourceKind::App);
    EXPECT_EQ(track.sources[1], AudioSourceKind::Sys);
    EXPECT_EQ(track.sources[2], AudioSourceKind::Mic);
    ASSERT_TRUE(result.audio_target_process_id.has_value());
    EXPECT_EQ(result.audio_target_process_id.value(), 1007u);
}

TEST(AudioPlanBuilderTest, WindowTarget_MicOnly) {
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Window;
    state.record_application_audio = false;
    state.record_system_audio = false;
    state.record_microphone = true;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 1u);
    ExpectSingleSourceTrack(result, 0, AudioSourceKind::Mic);
    EXPECT_FALSE(result.audio_target_process_id.has_value());
}

TEST(AudioPlanBuilderTest, WindowTarget_AllOff) {
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Window;
    state.record_application_audio = false;
    state.record_system_audio = false;
    state.record_microphone = false;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_FALSE(result.record_audio);
    EXPECT_TRUE(result.plan.tracks.empty());
    EXPECT_FALSE(result.audio_target_process_id.has_value());
}

TEST(AudioPlanBuilderTest, WindowTarget_AppRequestedMissingPid_KeepsAppTrackWithoutPid) {
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Window;
    state.record_application_audio = true;
    state.record_system_audio = false;
    state.record_microphone = false;
    state.selected_window_pid.reset();

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 1u);
    ExpectSingleSourceTrack(result, 0, AudioSourceKind::App);
    EXPECT_FALSE(result.audio_target_process_id.has_value());
}

TEST(AudioPlanBuilderTest, WindowTarget_AppSysRequestedMissingPid_KeepsAppSysTracksWithoutPid) {
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Window;
    state.record_application_audio = true;
    state.record_system_audio = true;
    state.separate_output_tracks = true;
    state.record_microphone = false;
    state.selected_window_pid.reset();

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 2u);
    ExpectSingleSourceTrack(result, 0, AudioSourceKind::App);
    ExpectSingleSourceTrack(result, 1, AudioSourceKind::Sys);
    EXPECT_FALSE(result.audio_target_process_id.has_value());
}

TEST(AudioPlanBuilderTest, DisplayTarget_SystemOnly) {
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Display;
    state.record_system_audio = true;
    state.record_microphone = false;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 1u);
    ExpectSingleSourceTrack(result, 0, AudioSourceKind::SystemOutput);
    EXPECT_FALSE(result.audio_target_process_id.has_value());
}

TEST(AudioPlanBuilderTest, DisplayTarget_MicOnly) {
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Display;
    state.record_system_audio = false;
    state.record_microphone = true;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 1u);
    ExpectSingleSourceTrack(result, 0, AudioSourceKind::Mic);
    EXPECT_FALSE(result.audio_target_process_id.has_value());
}

TEST(AudioPlanBuilderTest, DisplayTarget_SystemAndMic) {
    // Merge-first: {SystemOutput, Mic} as one merged track.
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

TEST(AudioPlanBuilderTest, DisplayTarget_AllOff) {
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Display;
    state.record_system_audio = false;
    state.record_microphone = false;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_FALSE(result.record_audio);
    EXPECT_TRUE(result.plan.tracks.empty());
    EXPECT_FALSE(result.audio_target_process_id.has_value());
}

TEST(AudioPlanBuilderTest, Preview_LabelsAreCorrect) {
    AudioPlanResult result;
    result.record_audio = true;

    recorder_core::ResolvedAudioTrack t0;
    t0.track_index = 0;
    t0.sources = {AudioSourceKind::App};
    result.plan.tracks.push_back(t0);

    recorder_core::ResolvedAudioTrack t1;
    t1.track_index = 1;
    t1.sources = {AudioSourceKind::Sys};
    result.plan.tracks.push_back(t1);

    recorder_core::ResolvedAudioTrack t2;
    t2.track_index = 2;
    t2.sources = {AudioSourceKind::SystemOutput};
    result.plan.tracks.push_back(t2);

    recorder_core::ResolvedAudioTrack t3;
    t3.track_index = 3;
    t3.sources = {AudioSourceKind::Mic};
    result.plan.tracks.push_back(t3);

    const std::vector<AudioTrackPreview> preview = BuildAudioTrackPreview(result);
    ASSERT_EQ(preview.size(), 4u);

    EXPECT_EQ(preview[0].track_number, 1u);
    EXPECT_EQ(preview[0].source_key, "app");
    EXPECT_EQ(preview[0].display_label, "Application Audio");

    EXPECT_EQ(preview[1].track_number, 2u);
    EXPECT_EQ(preview[1].source_key, "sys");
    EXPECT_EQ(preview[1].display_label, "Other System Audio");

    EXPECT_EQ(preview[2].track_number, 3u);
    EXPECT_EQ(preview[2].source_key, "system_output");
    EXPECT_EQ(preview[2].display_label, "System Audio");

    EXPECT_EQ(preview[3].track_number, 4u);
    EXPECT_EQ(preview[3].source_key, "mic");
    EXPECT_EQ(preview[3].display_label, "Microphone");
}

TEST(AudioPlanBuilderTest, Preview_NoAudio_ReturnsEmpty) {
    AudioPlanResult result;
    result.record_audio = false;
    recorder_core::ResolvedAudioTrack track;
    track.track_index = 0;
    track.sources = {AudioSourceKind::App};
    result.plan.tracks.push_back(track);

    const std::vector<AudioTrackPreview> preview = BuildAudioTrackPreview(result);
    EXPECT_TRUE(preview.empty());
}

TEST(AudioPlanBuilderTest, TrackIndices_AreSequential) {
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Window;
    state.record_application_audio = true;
    state.record_system_audio = true;
    state.separate_output_tracks = true;
    state.record_microphone = true;
    state.selected_window_pid = 1010u;

    const AudioPlanResult result = BuildAudioPlan(state);
    ASSERT_EQ(result.plan.tracks.size(), 3u);
    EXPECT_EQ(result.plan.tracks[0].track_index, 0u);
    EXPECT_EQ(result.plan.tracks[1].track_index, 1u);
    EXPECT_EQ(result.plan.tracks[2].track_index, 2u);
}

} // namespace
} // namespace exosnap::capability
