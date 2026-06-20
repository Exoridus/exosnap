// test_audio_row_gain_handlers.cpp
// Focused headless tests for the RecordPage per-row gain/mute handler logic:
// onAudioRowGainChanged and onAudioRowMutedChanged.
//
// These tests exercise the handler contract at the view-model level (no widgets
// required). They mirror the style used in test_record_view_model_audio.cpp.

#include <gtest/gtest.h>

#include <algorithm>

#include "viewmodels/RecordViewModel.h"

namespace exosnap {
namespace {

// Helper: find a source row by kind.
recorder_core::AudioSourceRow* FindRow(capability::AudioUiState& s, recorder_core::AudioSourceKind k) {
    for (auto& r : s.source_rows)
        if (r.kind == k)
            return &r;
    return nullptr;
}

// Simulate onAudioRowGainChanged handler logic (mirrors RecordPage implementation).
void ApplyGainChanged(RecordViewModel& vm, int row_index, float gain_db) {
    if (row_index < 0 || row_index >= static_cast<int>(vm.audio_ui_state.source_rows.size()))
        return;
    vm.audio_ui_state.source_rows[static_cast<std::size_t>(row_index)].gain_db = gain_db;
    vm.RebuildAudioPlan();
}

// Simulate onAudioRowMutedChanged handler logic (mirrors RecordPage implementation).
void ApplyMutedChanged(RecordViewModel& vm, int row_index, bool muted) {
    if (row_index < 0 || row_index >= static_cast<int>(vm.audio_ui_state.source_rows.size()))
        return;
    vm.audio_ui_state.source_rows[static_cast<std::size_t>(row_index)].muted = muted;
    vm.RebuildAudioPlan();
}

// ---------------------------------------------------------------------------
// onAudioRowGainChanged
// ---------------------------------------------------------------------------

TEST(AudioRowGainHandlers, GainChanged_UpdatesSourceRow) {
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);

    // Row 0 is App by policy.
    ASSERT_FALSE(vm.audio_ui_state.source_rows.empty());
    const int app_index = 0;

    ApplyGainChanged(vm, app_index, 6.0f);

    EXPECT_NEAR(vm.audio_ui_state.source_rows[static_cast<std::size_t>(app_index)].gain_db, 6.0f, 1e-4f);
}

TEST(AudioRowGainHandlers, GainChanged_OutOfBoundsIndex_IsNoop) {
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);

    const float original_gain = vm.audio_ui_state.source_rows[0].gain_db;

    // Index out of bounds — must not crash and must not modify any row.
    ApplyGainChanged(vm, 999, 12.0f);

    EXPECT_NEAR(vm.audio_ui_state.source_rows[0].gain_db, original_gain, 1e-4f);
}

TEST(AudioRowGainHandlers, GainChanged_NegativeIndex_IsNoop) {
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);

    const float original_gain = vm.audio_ui_state.source_rows[0].gain_db;

    ApplyGainChanged(vm, -1, 3.0f);

    EXPECT_NEAR(vm.audio_ui_state.source_rows[0].gain_db, original_gain, 1e-4f);
}

TEST(AudioRowGainHandlers, GainChanged_RebuildsPlan_TrackCountUnchanged) {
    // Changing gain must not alter the track topology — only the gain values.
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    const std::size_t initial_tracks = vm.audio_track_preview.size();

    ApplyGainChanged(vm, 0, -12.0f);

    EXPECT_EQ(vm.audio_track_preview.size(), initial_tracks);
}

TEST(AudioRowGainHandlers, GainChanged_PropagatesToResolvedPlan) {
    // After a gain change the resolved plan's source_gain_linear must reflect the new dB.
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    // Row 0 = App (enabled); gain → +6 dB → linear = pow(10, 6/20) ≈ 1.9953.
    ApplyGainChanged(vm, 0, 6.0f);

    ASSERT_FALSE(vm.audio_plan.plan.tracks.empty());
    const auto& track = vm.audio_plan.plan.tracks[0];
    ASSERT_FALSE(track.source_gain_linear.empty());
    EXPECT_NEAR(track.source_gain_linear[0], std::powf(10.0f, 6.0f / 20.0f), 1e-3f);
}

// ---------------------------------------------------------------------------
// onAudioRowMutedChanged
// ---------------------------------------------------------------------------

TEST(AudioRowGainHandlers, MutedChanged_UpdatesSourceRow) {
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);

    const int app_index = 0;
    ASSERT_FALSE(vm.audio_ui_state.source_rows[static_cast<std::size_t>(app_index)].muted);

    ApplyMutedChanged(vm, app_index, true);

    EXPECT_TRUE(vm.audio_ui_state.source_rows[static_cast<std::size_t>(app_index)].muted);
}

TEST(AudioRowGainHandlers, MutedChanged_Unmute_UpdatesSourceRow) {
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);

    const int app_index = 0;
    vm.audio_ui_state.source_rows[static_cast<std::size_t>(app_index)].muted = true;

    ApplyMutedChanged(vm, app_index, false);

    EXPECT_FALSE(vm.audio_ui_state.source_rows[static_cast<std::size_t>(app_index)].muted);
}

TEST(AudioRowGainHandlers, MutedChanged_OutOfBoundsIndex_IsNoop) {
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);

    ApplyMutedChanged(vm, 999, true);

    // All rows still unmuted.
    for (const auto& r : vm.audio_ui_state.source_rows)
        EXPECT_FALSE(r.muted);
}

TEST(AudioRowGainHandlers, MutedChanged_PropagatesToResolvedPlan_ZeroLinearGain) {
    // After muting App row, its source_gain_linear in the resolved track is 0.
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    ApplyMutedChanged(vm, 0, true);

    ASSERT_FALSE(vm.audio_plan.plan.tracks.empty());
    const auto& track = vm.audio_plan.plan.tracks[0];
    ASSERT_FALSE(track.source_gain_linear.empty());
    EXPECT_FLOAT_EQ(track.source_gain_linear[0], 0.0f);
}

TEST(AudioRowGainHandlers, MutedChanged_RebuildsPlan_TrackCountUnchanged) {
    // Muting does not remove the track from the plan — the source is silence-padded.
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    const std::size_t initial_tracks = vm.audio_track_preview.size();

    ApplyMutedChanged(vm, 0, true);

    EXPECT_EQ(vm.audio_track_preview.size(), initial_tracks);
}

// ---------------------------------------------------------------------------
// Mic row: per-row gain is hidden in the UI (has_gain_control=false) and the
// row stays at the default 0 dB / not muted, so mic_gain_linear is unaffected.
// We verify the composition here at the model level.
// ---------------------------------------------------------------------------

TEST(AudioRowGainHandlers, MicRow_DefaultGain_PreservesMicGainLinear) {
    // Mic row at 0 dB, not muted: effective = mic_gain_linear * 1.0.
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);
    vm.audio_ui_state.mic_gain_linear = 2.0f;

    // Enable Mic so it appears in the plan.
    auto* mic_row = FindRow(vm.audio_ui_state, recorder_core::AudioSourceKind::Mic);
    ASSERT_NE(mic_row, nullptr);
    mic_row->enabled = true;
    mic_row->gain_db = 0.0f;
    mic_row->muted = false;
    vm.RebuildAudioPlan();

    EXPECT_NEAR(vm.audio_plan.mic_gain_linear, 2.0f, 1e-4f);
}

TEST(AudioRowGainHandlers, MicRow_MutedViaRowModel_ProducesZeroLinearGain) {
    // Even though the UI hides the mic per-row gain slider, the mute button is
    // still present. If the mic row is muted, the resolved track gain is 0.
    RecordViewModel vm;
    vm.ApplyTargetKind(capability::CaptureTargetKind::Window);

    auto* mic_row = FindRow(vm.audio_ui_state, recorder_core::AudioSourceKind::Mic);
    ASSERT_NE(mic_row, nullptr);
    mic_row->enabled = true;
    mic_row->muted = true;
    vm.RebuildAudioPlan();

    // Find the Mic track in the plan.
    bool found = false;
    for (const auto& track : vm.audio_plan.plan.tracks) {
        for (std::size_t i = 0; i < track.sources.size(); ++i) {
            if (track.sources[i] == recorder_core::AudioSourceKind::Mic) {
                ASSERT_LT(i, track.source_gain_linear.size());
                EXPECT_FLOAT_EQ(track.source_gain_linear[i], 0.0f);
                found = true;
            }
        }
    }
    EXPECT_TRUE(found) << "Mic track not found in audio plan";
}

} // namespace
} // namespace exosnap
