#include <gtest/gtest.h>

#include <capability/audio_track_preview.h>
#include <capability/audio_ui_state.h>

namespace exosnap::capability {
namespace {

using K = recorder_core::AudioSourceKind;

// Build a Window state where App+Sys are merged and Mic is disabled.
AudioUiState WindowMergedState(std::optional<uint32_t> pid = std::nullopt) {
    AudioUiState s;
    s.target_kind = CaptureTargetKind::Window;
    s.selected_window_pid = pid;
    s.source_rows = {
        {K::App, true, false},
        {K::Mic, false, true},
        {K::Sys, true, true},
    };
    return s;
}

TEST(AudioUiStateTest, BuildAudioPlan_WindowMerged_NoMic_SingleTrackAppSys) {
    AudioUiState state = WindowMergedState(1000u);

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 1u);
    const auto& track = result.plan.tracks[0];
    ASSERT_EQ(track.sources.size(), 2u);
    EXPECT_EQ(track.sources[0], K::App);
    EXPECT_EQ(track.sources[1], K::Sys);
    ASSERT_TRUE(result.audio_target_process_id.has_value());
    EXPECT_EQ(result.audio_target_process_id.value(), 1000u);
}

TEST(AudioUiStateTest, BuildAudioPlan_WindowMerged_WithMic_SingleTrackAppMicSys) {
    AudioUiState state = WindowMergedState(1001u);
    // Enable Mic (merge_with_above=true → folds into App track).
    state.source_rows[1].enabled = true;

    const AudioPlanResult result = BuildAudioPlan(state);
    ASSERT_EQ(result.plan.tracks.size(), 1u);
    const auto& track = result.plan.tracks[0];
    ASSERT_EQ(track.sources.size(), 3u);
    EXPECT_EQ(track.sources[0], K::App);
    ASSERT_TRUE(result.audio_target_process_id.has_value());
    EXPECT_EQ(result.audio_target_process_id.value(), 1001u);
}

TEST(AudioUiStateTest, BuildAudioPlan_WindowSeparate_WithMic_ThreeTracks) {
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Window;
    state.selected_window_pid = 1002u;
    // All rows separate, all enabled.
    state.source_rows = {
        {K::App, true, false},
        {K::Mic, true, false},
        {K::Sys, true, false},
    };

    const AudioPlanResult result = BuildAudioPlan(state);
    ASSERT_EQ(result.plan.tracks.size(), 3u);
    ASSERT_EQ(result.plan.tracks[0].sources.size(), 1u);
    EXPECT_EQ(result.plan.tracks[0].sources[0], K::App);
    ASSERT_EQ(result.plan.tracks[1].sources.size(), 1u);
    EXPECT_EQ(result.plan.tracks[1].sources[0], K::Mic);
    ASSERT_EQ(result.plan.tracks[2].sources.size(), 1u);
    EXPECT_EQ(result.plan.tracks[2].sources[0], K::Sys);
    ASSERT_TRUE(result.audio_target_process_id.has_value());
    EXPECT_EQ(result.audio_target_process_id.value(), 1002u);
}

TEST(AudioUiStateTest, BuildAudioPlan_Display_WithMic_SingleTrackSystemOutputMic) {
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Display;
    // SystemOutput + Mic merged.
    state.source_rows = {
        {K::SystemOutput, true, false},
        {K::Mic, true, true},
    };

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 1u);
    const auto& track = result.plan.tracks[0];
    ASSERT_EQ(track.sources.size(), 2u);
    EXPECT_EQ(track.sources[0], K::SystemOutput);
    EXPECT_EQ(track.sources[1], K::Mic);
    EXPECT_FALSE(result.audio_target_process_id.has_value());
}

TEST(AudioUiStateTest, BuildAudioPlan_Display_Separate_SysAndMic_TwoTracks) {
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Display;
    state.source_rows = {
        {K::SystemOutput, true, false},
        {K::Mic, true, false},
    };

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 2u);
    ASSERT_EQ(result.plan.tracks[0].sources.size(), 1u);
    EXPECT_EQ(result.plan.tracks[0].sources[0], K::SystemOutput);
    ASSERT_EQ(result.plan.tracks[1].sources.size(), 1u);
    EXPECT_EQ(result.plan.tracks[1].sources[0], K::Mic);
    EXPECT_FALSE(result.audio_target_process_id.has_value());
}

TEST(AudioUiStateTest, BuildAudioPlan_Display_NoMic_SingleSystemOutputTrack) {
    AudioUiState state;
    state.target_kind = CaptureTargetKind::Display;
    state.source_rows = {
        {K::SystemOutput, true, false},
        {K::Mic, false, true},
    };

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 1u);
    ASSERT_EQ(result.plan.tracks[0].sources.size(), 1u);
    EXPECT_EQ(result.plan.tracks[0].sources[0], K::SystemOutput);
    EXPECT_FALSE(result.audio_target_process_id.has_value());
}

TEST(AudioUiStateTest, BuildAudioPlan_Merged_PropagatesMicGain) {
    AudioUiState state = WindowMergedState();
    state.source_rows[1].enabled = true;
    state.mic_gain_linear = 1.5f;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_FLOAT_EQ(result.mic_gain_linear, 1.5f);
}

TEST(AudioUiStateTest, BuildAudioTrackPreview_MergedTrack_SinglePreviewEntry) {
    AudioPlanResult result;
    result.record_audio = true;

    recorder_core::ResolvedAudioTrack merged;
    merged.track_index = 0;
    merged.sources = {K::App, K::SystemOutput, K::Mic};
    result.plan.tracks.push_back(std::move(merged));

    const std::vector<AudioTrackPreview> preview = BuildAudioTrackPreview(result);
    ASSERT_EQ(preview.size(), 1u);
    EXPECT_EQ(preview[0].track_number, 1u);
    EXPECT_EQ(preview[0].source_key, "merged");
    EXPECT_EQ(preview[0].display_label, "Mixed Audio");
}

TEST(AudioUiStateTest, IsAppEnabled_ReturnsTrueOnlyWhenAppRowEnabled) {
    AudioUiState state;
    state.source_rows = {{K::App, false, false}, {K::Mic, true, false}};
    EXPECT_FALSE(state.IsAppEnabled());
    state.source_rows[0].enabled = true;
    EXPECT_TRUE(state.IsAppEnabled());
}

TEST(AudioUiStateTest, IsSysEnabled_MatchesBothSysAndSystemOutput) {
    AudioUiState state;
    state.source_rows = {{K::Sys, true, false}};
    EXPECT_TRUE(state.IsSysEnabled());
    state.source_rows[0].kind = K::SystemOutput;
    EXPECT_TRUE(state.IsSysEnabled());
    state.source_rows[0].enabled = false;
    EXPECT_FALSE(state.IsSysEnabled());
}

TEST(AudioUiStateTest, IsMicEnabled_CorrectlyDetectsMicRow) {
    AudioUiState state;
    state.source_rows = {{K::Mic, false, false}};
    EXPECT_FALSE(state.IsMicEnabled());
    state.source_rows[0].enabled = true;
    EXPECT_TRUE(state.IsMicEnabled());
}

} // namespace
} // namespace exosnap::capability
