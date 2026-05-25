#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <recorder_core/audio_track_model.h>
#include <recorder_core/recorder_session.h>

namespace exosnap::capability {

enum class CaptureTargetKind {
    Window,
    Display,
};

struct AudioUiState {
    CaptureTargetKind target_kind = CaptureTargetKind::Display;

    // Ordered list of audio sources. The first enabled row always starts a new
    // track; subsequent rows merge into the previous track when merge_with_above=true.
    std::vector<recorder_core::AudioSourceRow> source_rows;

    // Mic-specific settings (not part of the row model).
    recorder_core::MicChannelMode mic_channel_mode = recorder_core::MicChannelMode::Auto;
    std::optional<std::string> selected_mic_device_id;
    float mic_gain_linear = 1.0f;

    // Set for Window target. Null for Display or unresolved Window PID.
    std::optional<uint32_t> selected_window_pid;

    // Convenience predicates.
    [[nodiscard]] bool IsAppEnabled() const noexcept;
    [[nodiscard]] bool IsSysEnabled() const noexcept;
    [[nodiscard]] bool IsMicEnabled() const noexcept;
};

struct AudioPlanResult {
    bool record_audio = false;
    recorder_core::AudioTrackPlan plan;
    std::optional<uint32_t> audio_target_process_id;
    recorder_core::MicChannelMode mic_channel_mode = recorder_core::MicChannelMode::Auto;
    std::optional<std::string> mic_device_id;
    float mic_gain_linear = 1.0f;
};

[[nodiscard]] AudioPlanResult BuildAudioPlan(const AudioUiState& state);

} // namespace exosnap::capability
