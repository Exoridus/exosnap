#include <recorder_core/audio_track_model.h>

namespace recorder_core {

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

} // namespace recorder_core
