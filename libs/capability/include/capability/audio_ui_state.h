#pragma once

#include <cstdint>
#include <optional>

#include <recorder_core/audio_track_model.h>
#include <recorder_core/recorder_session.h>

namespace exosnap::capability {

enum class CaptureTargetKind {
    Window,
    Display,
};

struct AudioUiState {
    CaptureTargetKind target_kind = CaptureTargetKind::Display;

    // App/Window target controls. Ignored for Display target.
    bool record_application_audio = true;
    bool record_system_audio = true;
    bool separate_output_tracks = true;

    // Common controls.
    bool record_microphone = false;
    recorder_core::MicChannelMode mic_channel_mode = recorder_core::MicChannelMode::Auto;

    // Set for Window target. Null for Display or unresolved Window PID.
    std::optional<uint32_t> selected_window_pid;
};

struct AudioPlanResult {
    // Derived result: true if at least one audio track should be recorded.
    bool record_audio = false;

    recorder_core::AudioTrackPlan plan;
    std::optional<uint32_t> audio_target_process_id;
    recorder_core::MicChannelMode mic_channel_mode = recorder_core::MicChannelMode::Auto;
};

[[nodiscard]] AudioPlanResult BuildAudioPlan(const AudioUiState& state);

} // namespace exosnap::capability
