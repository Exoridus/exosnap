#include <capability/audio_track_preview.h>
#include <capability/audio_ui_state.h>

#include <utility>

namespace exosnap::capability {
namespace {

void AddSingleSourceTrack(recorder_core::AudioTrackPlan& plan, recorder_core::AudioSourceKind kind) {
    recorder_core::ResolvedAudioTrack track;
    track.track_index = static_cast<uint32_t>(plan.tracks.size());
    track.sources.push_back(kind);
    plan.tracks.push_back(std::move(track));
}

} // namespace

AudioPlanResult BuildAudioPlan(const AudioUiState& state) {
    AudioPlanResult result;
    result.mic_channel_mode = state.mic_channel_mode;
    result.mic_device_id = state.selected_mic_device_id;
    result.mic_gain_linear = state.mic_gain_linear;

    if (state.target_kind == CaptureTargetKind::Window) {
        const bool app = state.record_application_audio;
        const bool sys = state.record_system_audio;
        const bool sep = state.separate_output_tracks;
        const bool mic = state.record_microphone;

        if (sep) {
            // Separate tracks: each enabled source gets its own track.
            if (app && sys) {
                AddSingleSourceTrack(result.plan, recorder_core::AudioSourceKind::App);
                AddSingleSourceTrack(result.plan, recorder_core::AudioSourceKind::Sys);
                result.audio_target_process_id = state.selected_window_pid;
            } else if (app) {
                AddSingleSourceTrack(result.plan, recorder_core::AudioSourceKind::App);
                result.audio_target_process_id = state.selected_window_pid;
            } else if (sys) {
                AddSingleSourceTrack(result.plan, recorder_core::AudioSourceKind::Sys);
                result.audio_target_process_id = state.selected_window_pid;
            }
            if (mic) {
                AddSingleSourceTrack(result.plan, recorder_core::AudioSourceKind::Mic);
            }
        } else {
            // Merge-first: one track with all enabled sources; MIC always last.
            recorder_core::ResolvedAudioTrack merged;
            merged.track_index = 0;
            if (app)
                merged.sources.push_back(recorder_core::AudioSourceKind::App);
            if (sys)
                merged.sources.push_back(recorder_core::AudioSourceKind::Sys);
            if (mic)
                merged.sources.push_back(recorder_core::AudioSourceKind::Mic);

            if (!merged.sources.empty()) {
                result.plan.tracks.push_back(std::move(merged));
            }
            if (app || sys) {
                result.audio_target_process_id = state.selected_window_pid;
            }
        }
    } else {
        // Display target: merge system output and mic into one track when both enabled.
        const bool sys = state.record_system_audio;
        const bool mic = state.record_microphone;

        if (sys && mic) {
            recorder_core::ResolvedAudioTrack merged;
            merged.track_index = 0;
            merged.sources.push_back(recorder_core::AudioSourceKind::SystemOutput);
            merged.sources.push_back(recorder_core::AudioSourceKind::Mic);
            result.plan.tracks.push_back(std::move(merged));
        } else if (sys) {
            AddSingleSourceTrack(result.plan, recorder_core::AudioSourceKind::SystemOutput);
        } else if (mic) {
            AddSingleSourceTrack(result.plan, recorder_core::AudioSourceKind::Mic);
        }
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
    for (size_t i = 0; i < result.plan.tracks.size(); ++i) {
        const auto& track = result.plan.tracks[i];
        if (track.sources.empty()) {
            continue;
        }

        AudioTrackPreview item;
        item.track_number = static_cast<uint32_t>(i + 1);

        if (track.sources.size() > 1) {
            // Multi-source (merged) track: one preview row for the mix.
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
