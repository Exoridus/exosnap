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

AudioSourcePresentationState DeriveSource(const recorder_core::AudioSourceRow* row, bool controls_locked) {
    AudioSourcePresentationState s;
    s.available = (row != nullptr);
    s.enabled = row ? row->enabled : false;
    // Required invariant: controls_enabled = available && !controls_locked
    s.controls_enabled = s.available && !controls_locked;
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

    // The App row is permanently present; it is "live" only while a specific
    // application window is the capture target, and recedes otherwise.
    snap.app = DeriveSource(app_row, controls_locked);
    snap.app.active = is_window;

    // Sys is always live (relabelled per target kind in the consumer).
    snap.system = DeriveSource(sys_row, controls_locked);
    snap.system.active = true;

    // Mic is always live.
    snap.mic = DeriveSource(mic_row, controls_locked);
    snap.mic.active = true;

    snap.selected_mic_device_id = audio_state.selected_mic_device_id;

    return snap;
}

} // namespace exosnap
