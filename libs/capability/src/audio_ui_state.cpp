#include <capability/audio_track_preview.h>
#include <capability/audio_ui_state.h>

#include <algorithm>

namespace exosnap::capability {

bool AudioUiState::IsAppEnabled() const noexcept {
    return std::any_of(source_rows.begin(), source_rows.end(), [](const recorder_core::AudioSourceRow& r) {
        return r.kind == recorder_core::AudioSourceKind::App && r.enabled;
    });
}

bool AudioUiState::IsSysEnabled() const noexcept {
    return std::any_of(source_rows.begin(), source_rows.end(), [](const recorder_core::AudioSourceRow& r) {
        return (r.kind == recorder_core::AudioSourceKind::Sys ||
                r.kind == recorder_core::AudioSourceKind::SystemOutput) &&
               r.enabled;
    });
}

bool AudioUiState::IsMicEnabled() const noexcept {
    return std::any_of(source_rows.begin(), source_rows.end(), [](const recorder_core::AudioSourceRow& r) {
        return r.kind == recorder_core::AudioSourceKind::Mic && r.enabled;
    });
}

AudioPlanResult BuildAudioPlan(const AudioUiState& state) {
    AudioPlanResult result;
    result.mic_channel_mode = state.mic_channel_mode;
    result.mic_device_id = state.selected_mic_device_id;
    result.mic_gain_linear = state.mic_gain_linear;

    // Audio encoding params (ADR 0019) — pass through directly.
    result.audio_bitrate_kbps = state.audio_bitrate_kbps;
    result.opus_frame_duration = state.opus_frame_duration;
    result.opus_complexity = state.opus_complexity;

    // Brickwall limiter (Audio v2) — pass through directly.
    result.limiter_enabled = state.limiter_enabled;
    result.limiter_ceiling_db = state.limiter_ceiling_db;

    // Microphone high-pass filter (Audio v2) — pass through directly.
    result.mic_hpf_enabled = state.mic_hpf_enabled;
    result.mic_hpf_cutoff_hz = state.mic_hpf_cutoff_hz;

    // Microphone noise gate (Audio v2) — pass through directly.
    result.mic_gate_enabled = state.mic_gate_enabled;
    result.mic_gate_threshold_db = state.mic_gate_threshold_db;

    result.plan = recorder_core::ResolveAudioTracks(state.source_rows);

    const bool app_active = state.IsAppEnabled();
    if (app_active && state.target_kind == CaptureTargetKind::Window) {
        result.audio_target_process_id = state.selected_window_pid;
    }

    result.record_audio = !result.plan.tracks.empty();
    return result;
}

std::vector<AudioTrackPreview> BuildAudioTrackPreview(const AudioPlanResult& result) {
    std::vector<AudioTrackPreview> preview;
    if (!result.record_audio) {
        return preview;
    }

    preview.reserve(result.plan.tracks.size());
    for (std::size_t i = 0; i < result.plan.tracks.size(); ++i) {
        const auto& track = result.plan.tracks[i];
        if (track.sources.empty()) {
            continue;
        }

        AudioTrackPreview item;
        item.track_number = static_cast<uint32_t>(i + 1);

        if (track.sources.size() > 1) {
            item.source_key = "merged";
            item.display_label = "Mixed Audio";
        } else {
            switch (track.sources.front()) {
            case recorder_core::AudioSourceKind::App:
                item.source_key = "app";
                item.display_label = "Application Audio";
                break;
            case recorder_core::AudioSourceKind::Sys:
                item.source_key = "sys";
                item.display_label = "Other System Audio";
                break;
            case recorder_core::AudioSourceKind::Mic:
                item.source_key = "mic";
                item.display_label = "Microphone";
                break;
            case recorder_core::AudioSourceKind::SystemOutput:
                item.source_key = "system_output";
                item.display_label = "System Audio";
                break;
            default:
                item.source_key = "unknown";
                item.display_label = "Unknown Audio Source";
                break;
            }
        }

        preview.push_back(std::move(item));
    }

    return preview;
}

} // namespace exosnap::capability
