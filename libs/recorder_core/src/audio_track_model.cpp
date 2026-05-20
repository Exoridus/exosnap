#include <recorder_core/audio_track_model.h>

namespace recorder_core {

AudioTrackPlan ResolveAudioTracks(const std::vector<AudioSourceRow>& rows) {
    AudioTrackPlan plan;

    for (const AudioSourceRow& row : rows) {
        if (!row.enabled) {
            continue;
        }

        if (plan.tracks.empty() || !row.merge_with_above) {
            ResolvedAudioTrack track;
            track.track_index = static_cast<uint32_t>(plan.tracks.size());
            track.sources.push_back(row.kind);
            plan.tracks.push_back(track);
            continue;
        }

        plan.tracks.back().sources.push_back(row.kind);
    }

    return plan;
}

} // namespace recorder_core
