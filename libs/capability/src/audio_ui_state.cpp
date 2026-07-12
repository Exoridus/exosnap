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

    // A/V clock slaving (H-3) — pass through directly.
    result.clock_slaving_enabled = state.clock_slaving_enabled;

    // Microphone high-pass filter (Audio v2) — pass through directly.
    result.mic_hpf_enabled = state.mic_hpf_enabled;
    result.mic_hpf_cutoff_hz = state.mic_hpf_cutoff_hz;

    // Microphone noise gate (Audio v2) — pass through directly.
    result.mic_gate_enabled = state.mic_gate_enabled;
    result.mic_gate_threshold_db = state.mic_gate_threshold_db;

    // Microphone automatic gain control (Audio v2) — pass through directly.
    result.mic_agc_enabled = state.mic_agc_enabled;
    result.mic_agc_target_db = state.mic_agc_target_db;

    // Microphone RNNoise neural noise suppression (Audio v2) — pass through directly.
    result.mic_rnnoise_enabled = state.mic_rnnoise_enabled;

    // Channel / sample-format model (ADR 0030) — pass through directly.
    result.audio_sample_rate = state.audio_sample_rate;
    result.audio_channels = state.audio_channels;
    result.audio_bit_depth = state.audio_bit_depth;
    result.audio_pcm_float = state.audio_pcm_float;
    result.flac_compression_level = state.flac_compression_level;

    // A Display or Region target has no process to scope App/Sys to. Normalize before
    // resolving, or a stored app row survives into the plan and the engine refuses to
    // prepare for want of a process id.
    const bool window_target = state.target_kind == CaptureTargetKind::Window;
    result.plan = recorder_core::ResolveAudioTracks(
        recorder_core::NormalizeSourceRowsForTarget(state.source_rows, window_target));

    // Sys is the App row's complement and is just as process-scoped, so the pid has to
    // follow the plan rather than the App row alone: a window recording with Sys on and
    // App off still needs the process it excludes.
    const bool plan_needs_pid =
        std::any_of(result.plan.tracks.begin(), result.plan.tracks.end(), [](const auto& track) {
            return std::any_of(track.sources.begin(), track.sources.end(), [](recorder_core::AudioSourceKind kind) {
                return kind == recorder_core::AudioSourceKind::App || kind == recorder_core::AudioSourceKind::Sys;
            });
        });
    if (window_target && plan_needs_pid) {
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
