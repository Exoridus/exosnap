#include "PresentationStateBuilder.h"

#include <recorder_core/audio_track_model.h>

namespace exosnap {

namespace {

// Locate a source row by kind in an AudioUiState.  For Sys queries, also
// accepts SystemOutput (the full-loopback row used in Display mode).
const recorder_core::AudioSourceRow* FindRow(const capability::AudioUiState& state,
                                             recorder_core::AudioSourceKind kind) {
    for (const auto& row : state.source_rows) {
        if (row.kind == kind)
            return &row;
    }
    if (kind == recorder_core::AudioSourceKind::Sys) {
        for (const auto& row : state.source_rows) {
            if (row.kind == recorder_core::AudioSourceKind::SystemOutput)
                return &row;
        }
    }
    return nullptr;
}

AudioSourcePresentationState DeriveSource(const recorder_core::AudioSourceRow* row, bool visible,
                                          bool controls_locked) {
    AudioSourcePresentationState s;
    s.visible = visible;
    s.available = (row != nullptr);
    s.enabled = row ? row->enabled : false;
    // Required invariant: controls_enabled = visible && available && !controls_locked
    s.controls_enabled = s.visible && s.available && !controls_locked;
    s.separate_track = row ? !row->merge_with_above : false;
    return s;
}

} // namespace

AudioConfigurationSnapshot
PresentationStateBuilder::BuildAudioConfiguration(const capability::AudioUiState& audio_state, bool controls_locked) {
    const bool is_window = (audio_state.target_kind == capability::CaptureTargetKind::Window);

    const auto* app_row = FindRow(audio_state, recorder_core::AudioSourceKind::App);
    const auto* sys_row = FindRow(audio_state, recorder_core::AudioSourceKind::Sys);
    const auto* mic_row = FindRow(audio_state, recorder_core::AudioSourceKind::Mic);

    AudioConfigurationSnapshot snap;
    snap.target_kind = audio_state.target_kind;
    snap.controls_locked = controls_locked;

    // App is visible only for Window targets.
    snap.app = DeriveSource(app_row, /*visible=*/is_window, controls_locked);

    // Sys is always visible (relabelled per target kind in the consumer).
    snap.system = DeriveSource(sys_row, /*visible=*/true, controls_locked);

    // Mic is always visible.
    snap.mic = DeriveSource(mic_row, /*visible=*/true, controls_locked);

    snap.selected_mic_device_id = audio_state.selected_mic_device_id;

    return snap;
}

} // namespace exosnap
