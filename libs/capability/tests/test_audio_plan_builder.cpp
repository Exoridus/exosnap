#include <gtest/gtest.h>

#include <capability/audio_track_preview.h>
#include <capability/audio_ui_state.h>

#include <optional>
#include <vector>

namespace exosnap::capability {
namespace {

using K = recorder_core::AudioSourceKind;

void ExpectSingleSourceTrack(const AudioPlanResult& result, size_t track_pos, K expected_kind) {
    ASSERT_LT(track_pos, result.plan.tracks.size());
    const auto& track = result.plan.tracks[track_pos];
    ASSERT_EQ(track.sources.size(), 1u);
    EXPECT_EQ(track.sources.front(), expected_kind);
}

// Helper: Window state with all-separate rows.
AudioUiState WindowSep(std::optional<uint32_t> pid = std::nullopt) {
    AudioUiState s;
    s.target_kind = CaptureTargetKind::Window;
    s.selected_window_pid = pid;
    return s;
}

// Helper: Display state with all-separate rows.
AudioUiState DisplaySep() {
    AudioUiState s;
    s.target_kind = CaptureTargetKind::Display;
    return s;
}

TEST(AudioPlanBuilderTest, BuildAudioPlan_PropagatesSelectedMicDeviceId) {
    AudioUiState state;
    state.source_rows = {{K::Mic, true, false}};
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

TEST(AudioPlanBuilderTest, BuildAudioPlan_PropagatesMicGainLinear) {
    AudioUiState state;
    state.source_rows = {{K::Mic, true, false}};
    state.mic_gain_linear = 4.0f;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_FLOAT_EQ(result.mic_gain_linear, 4.0f);
}

TEST(AudioPlanBuilderTest, BuildAudioPlan_DefaultMicGainIsUnity) {
    AudioUiState state;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_FLOAT_EQ(result.mic_gain_linear, 1.0f);
}

TEST(AudioPlanBuilderTest, BuildAudioPlan_DefaultLimiterEnabledAtZeroDb) {
    AudioUiState state;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.limiter_enabled);
    EXPECT_FLOAT_EQ(result.limiter_ceiling_db, 0.0f);
}

TEST(AudioPlanBuilderTest, BuildAudioPlan_PropagatesLimiterSettings) {
    AudioUiState state;
    state.limiter_enabled = false;
    state.limiter_ceiling_db = -3.0f;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_FALSE(result.limiter_enabled);
    EXPECT_FLOAT_EQ(result.limiter_ceiling_db, -3.0f);
}

TEST(AudioPlanBuilderTest, BuildAudioPlan_DefaultMicHpfDisabledAt80Hz) {
    AudioUiState state;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_FALSE(result.mic_hpf_enabled);
    EXPECT_FLOAT_EQ(result.mic_hpf_cutoff_hz, 80.0f);
}

TEST(AudioPlanBuilderTest, BuildAudioPlan_PropagatesMicHpfSettings) {
    AudioUiState state;
    state.mic_hpf_enabled = true;
    state.mic_hpf_cutoff_hz = 120.0f;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.mic_hpf_enabled);
    EXPECT_FLOAT_EQ(result.mic_hpf_cutoff_hz, 120.0f);
}

TEST(AudioPlanBuilderTest, BuildAudioPlan_DefaultMicGateDisabledAtMinus45Db) {
    AudioUiState state;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_FALSE(result.mic_gate_enabled);
    EXPECT_FLOAT_EQ(result.mic_gate_threshold_db, -45.0f);
}

TEST(AudioPlanBuilderTest, BuildAudioPlan_PropagatesMicGateSettings) {
    AudioUiState state;
    state.mic_gate_enabled = true;
    state.mic_gate_threshold_db = -30.0f;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.mic_gate_enabled);
    EXPECT_FLOAT_EQ(result.mic_gate_threshold_db, -30.0f);
}

TEST(AudioPlanBuilderTest, BuildAudioPlan_DefaultMicAgcDisabledAtMinus18Db) {
    AudioUiState state;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_FALSE(result.mic_agc_enabled);
    EXPECT_FLOAT_EQ(result.mic_agc_target_db, -18.0f);
}

TEST(AudioPlanBuilderTest, BuildAudioPlan_PropagatesMicAgcSettings) {
    AudioUiState state;
    state.mic_agc_enabled = true;
    state.mic_agc_target_db = -24.0f;

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.mic_agc_enabled);
    EXPECT_FLOAT_EQ(result.mic_agc_target_db, -24.0f);
}

TEST(AudioPlanBuilderTest, WindowTarget_AppOnly) {
    AudioUiState state = WindowSep(1001u);
    state.source_rows = {{K::App, true, false}};

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 1u);
    ExpectSingleSourceTrack(result, 0, K::App);
    ASSERT_TRUE(result.audio_target_process_id.has_value());
    EXPECT_EQ(result.audio_target_process_id.value(), 1001u);
}

TEST(AudioPlanBuilderTest, WindowTarget_SysOnly) {
    AudioUiState state = WindowSep(1002u);
    state.source_rows = {{K::Sys, true, false}};

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 1u);
    ExpectSingleSourceTrack(result, 0, K::Sys);
    // Sys alone without App → no PID needed.
    EXPECT_FALSE(result.audio_target_process_id.has_value());
}

TEST(AudioPlanBuilderTest, WindowTarget_AppAndSys_Separate) {
    AudioUiState state = WindowSep(1003u);
    state.source_rows = {
        {K::App, true, false},
        {K::Sys, true, false},
    };

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 2u);
    ExpectSingleSourceTrack(result, 0, K::App);
    ExpectSingleSourceTrack(result, 1, K::Sys);
    ASSERT_TRUE(result.audio_target_process_id.has_value());
    EXPECT_EQ(result.audio_target_process_id.value(), 1003u);
}

TEST(AudioPlanBuilderTest, WindowTarget_AppAndSys_Merged) {
    AudioUiState state = WindowSep(1004u);
    state.source_rows = {
        {K::App, true, false},
        {K::Sys, true, true},
    };

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 1u);
    const auto& track = result.plan.tracks[0];
    ASSERT_EQ(track.sources.size(), 2u);
    EXPECT_EQ(track.sources[0], K::App);
    EXPECT_EQ(track.sources[1], K::Sys);
    ASSERT_TRUE(result.audio_target_process_id.has_value());
    EXPECT_EQ(result.audio_target_process_id.value(), 1004u);
}

TEST(AudioPlanBuilderTest, WindowTarget_AppAndMic_Separate) {
    AudioUiState state = WindowSep(1005u);
    state.source_rows = {
        {K::App, true, false},
        {K::Mic, true, false},
    };

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 2u);
    ExpectSingleSourceTrack(result, 0, K::App);
    ExpectSingleSourceTrack(result, 1, K::Mic);
    ASSERT_TRUE(result.audio_target_process_id.has_value());
    EXPECT_EQ(result.audio_target_process_id.value(), 1005u);
}

TEST(AudioPlanBuilderTest, WindowTarget_SysAndMic_Separate) {
    AudioUiState state = WindowSep(1006u);
    state.source_rows = {
        {K::Sys, true, false},
        {K::Mic, true, false},
    };

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 2u);
    ExpectSingleSourceTrack(result, 0, K::Sys);
    ExpectSingleSourceTrack(result, 1, K::Mic);
    EXPECT_FALSE(result.audio_target_process_id.has_value());
}

TEST(AudioPlanBuilderTest, WindowTarget_AppSysMic_AllMerged) {
    AudioUiState state = WindowSep(1007u);
    state.source_rows = {
        {K::App, true, false},
        {K::Mic, true, true},
        {K::Sys, true, true},
    };

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 1u);
    ASSERT_EQ(result.plan.tracks[0].sources.size(), 3u);
    ASSERT_TRUE(result.audio_target_process_id.has_value());
    EXPECT_EQ(result.audio_target_process_id.value(), 1007u);
}

TEST(AudioPlanBuilderTest, WindowTarget_MicOnly) {
    AudioUiState state = WindowSep();
    state.source_rows = {{K::Mic, true, false}};

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 1u);
    ExpectSingleSourceTrack(result, 0, K::Mic);
    EXPECT_FALSE(result.audio_target_process_id.has_value());
}

TEST(AudioPlanBuilderTest, WindowTarget_AllOff) {
    AudioUiState state = WindowSep();
    state.source_rows = {
        {K::App, false, false},
        {K::Mic, false, false},
        {K::Sys, false, false},
    };

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_FALSE(result.record_audio);
    EXPECT_TRUE(result.plan.tracks.empty());
    EXPECT_FALSE(result.audio_target_process_id.has_value());
}

TEST(AudioPlanBuilderTest, WindowTarget_AppOnly_NoPid_NoTargetProcessId) {
    AudioUiState state = WindowSep(); // no PID
    state.source_rows = {{K::App, true, false}};

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 1u);
    ExpectSingleSourceTrack(result, 0, K::App);
    EXPECT_FALSE(result.audio_target_process_id.has_value());
}

TEST(AudioPlanBuilderTest, WindowTarget_AppSys_Separate_NoPid) {
    AudioUiState state = WindowSep(); // no PID
    state.source_rows = {
        {K::App, true, false},
        {K::Sys, true, false},
    };

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 2u);
    ExpectSingleSourceTrack(result, 0, K::App);
    ExpectSingleSourceTrack(result, 1, K::Sys);
    EXPECT_FALSE(result.audio_target_process_id.has_value());
}

TEST(AudioPlanBuilderTest, DisplayTarget_SystemOnly) {
    AudioUiState state = DisplaySep();
    state.source_rows = {{K::SystemOutput, true, false}};

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 1u);
    ExpectSingleSourceTrack(result, 0, K::SystemOutput);
    EXPECT_FALSE(result.audio_target_process_id.has_value());
}

TEST(AudioPlanBuilderTest, DisplayTarget_MicOnly) {
    AudioUiState state = DisplaySep();
    state.source_rows = {{K::Mic, true, false}};

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 1u);
    ExpectSingleSourceTrack(result, 0, K::Mic);
    EXPECT_FALSE(result.audio_target_process_id.has_value());
}

TEST(AudioPlanBuilderTest, DisplayTarget_SystemAndMic_Merged) {
    AudioUiState state = DisplaySep();
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

TEST(AudioPlanBuilderTest, DisplayTarget_SystemAndMic_Separate) {
    AudioUiState state = DisplaySep();
    state.source_rows = {
        {K::SystemOutput, true, false},
        {K::Mic, true, false},
    };

    const AudioPlanResult result = BuildAudioPlan(state);
    EXPECT_TRUE(result.record_audio);
    ASSERT_EQ(result.plan.tracks.size(), 2u);
    ExpectSingleSourceTrack(result, 0, K::SystemOutput);
    ExpectSingleSourceTrack(result, 1, K::Mic);
    EXPECT_FALSE(result.audio_target_process_id.has_value());
}

TEST(AudioPlanBuilderTest, DisplayTarget_AllOff) {
    AudioUiState state = DisplaySep();
    state.source_rows = {
        {K::SystemOutput, false, false},
        {K::Mic, false, false},
    };

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
    t0.sources = {K::App};
    result.plan.tracks.push_back(t0);

    recorder_core::ResolvedAudioTrack t1;
    t1.track_index = 1;
    t1.sources = {K::Sys};
    result.plan.tracks.push_back(t1);

    recorder_core::ResolvedAudioTrack t2;
    t2.track_index = 2;
    t2.sources = {K::SystemOutput};
    result.plan.tracks.push_back(t2);

    recorder_core::ResolvedAudioTrack t3;
    t3.track_index = 3;
    t3.sources = {K::Mic};
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
    track.sources = {K::App};
    result.plan.tracks.push_back(track);

    const std::vector<AudioTrackPreview> preview = BuildAudioTrackPreview(result);
    EXPECT_TRUE(preview.empty());
}

TEST(AudioPlanBuilderTest, TrackIndices_AreSequential) {
    AudioUiState state = WindowSep(1010u);
    state.source_rows = {
        {K::App, true, false},
        {K::Mic, true, false},
        {K::Sys, true, false},
    };

    const AudioPlanResult result = BuildAudioPlan(state);
    ASSERT_EQ(result.plan.tracks.size(), 3u);
    EXPECT_EQ(result.plan.tracks[0].track_index, 0u);
    EXPECT_EQ(result.plan.tracks[1].track_index, 1u);
    EXPECT_EQ(result.plan.tracks[2].track_index, 2u);
}

} // namespace
} // namespace exosnap::capability
