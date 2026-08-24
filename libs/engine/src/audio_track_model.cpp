#include <exosnap/engine/audio_track_model.h>

#include <algorithm>

namespace exosnap::engine {

std::vector<AudioSourceRow> NormalizeSourceRowsForTarget(std::vector<AudioSourceRow> rows, bool window_target) {
    if (window_target) {
        return rows;
    }

    // App captures one process tree; Sys captures everything *except* that tree.
    // Both need a process id, and a display or region target has none. App loses
    // its meaning entirely, so the row goes. Sys does not: with no app to exclude,
    // "everything except the app" is the full system output — the same audio the
    // user asked for, from a source that needs no pid.
    std::erase_if(rows, [](const AudioSourceRow& row) { return row.kind == AudioSourceKind::App; });
    for (AudioSourceRow& row : rows) {
        if (row.kind == AudioSourceKind::Sys) {
            row.kind = AudioSourceKind::SystemOutput;
        }
    }

    // The conversion can collide with an existing SystemOutput row. Two full
    // loopbacks would capture the same audio twice, so only the first survives.
    bool seen_system_output = false;
    std::erase_if(rows, [&seen_system_output](const AudioSourceRow& row) {
        if (row.kind != AudioSourceKind::SystemOutput) {
            return false;
        }
        const bool duplicate = seen_system_output;
        seen_system_output = true;
        return duplicate;
    });

    return rows;
}

AudioTrackPlan ResolveAudioTracks(const std::vector<AudioSourceRow>& rows) {
    AudioTrackPlan plan;

    for (const AudioSourceRow& row : rows) {
        if (!row.enabled) {
            continue;
        }

        const float row_gain = GainDbToLinear(row.gain_db, row.muted);

        if (plan.tracks.empty() || !row.merge_with_above) {
            ResolvedAudioTrack track;
            track.track_index = static_cast<uint32_t>(plan.tracks.size());
            track.sources.push_back(row.kind);
            track.source_gain_linear.push_back(row_gain);
            plan.tracks.push_back(track);
            continue;
        }

        plan.tracks.back().sources.push_back(row.kind);
        plan.tracks.back().source_gain_linear.push_back(row_gain);
    }

    return plan;
}

std::string AudioSourceKindDisplayName(AudioSourceKind kind) {
    switch (kind) {
    case AudioSourceKind::App:
        return "Application";
    case AudioSourceKind::Mic:
        return "Microphone";
    case AudioSourceKind::Sys:
    case AudioSourceKind::SystemOutput:
        return "System";
    }
    return "";
}

std::string DeriveAudioTrackName(const ResolvedAudioTrack& track) {
    std::string name;
    for (const AudioSourceKind kind : track.sources) {
        if (!name.empty()) {
            name += " + ";
        }
        name += AudioSourceKindDisplayName(kind);
    }
    return name;
}

} // namespace exosnap::engine
