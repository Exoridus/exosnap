#pragma once

#include "EditCommandTypes.h"
#include <exosnap/engine/edit_timeline.h>

#include <algorithm>

namespace exosnap::edit {

[[nodiscard]] inline Time SnapshotDuration(const EditSnapshot& snapshot) {
    Time end = snapshot.timeline_end.mode == TimelineEndMode::FixedEnd ? snapshot.timeline_end.end_us : 0;
    for (const auto& clip : snapshot.clips)
        end = std::max(end, clip.end());
    for (const auto& spacer : snapshot.spacers)
        end = std::max(end, spacer.end());
    return end;
}

[[nodiscard]] inline engine::TimelineSnapshot RenderTimeline(const EditSnapshot& snapshot) {
    engine::TimelineSnapshot result;
    result.duration_us = SnapshotDuration(snapshot);
    result.revision = snapshot.revision;
    result.result_in_us = snapshot.result_range.in_us;
    result.result_out_us = snapshot.result_range.out_mode == ResultOutMode::FollowTimelineEnd
                               ? result.duration_us
                               : snapshot.result_range.out_us;
    result.output_fps_num = static_cast<int>(snapshot.output_fps.num);
    result.output_fps_den = static_cast<int>(snapshot.output_fps.den);
    result.output_width = snapshot.output_width;
    result.output_height = snapshot.output_height;
    for (const auto& track : snapshot.tracks)
        result.tracks.push_back({track.id, track.type == TrackType::Audio, track.edit_enabled, track.gain_db});
    for (const auto& clip : snapshot.clips) {
        const auto track = std::find_if(snapshot.tracks.begin(), snapshot.tracks.end(),
                                        [&](const auto& value) { return value.id == clip.track; });
        if (track == snapshot.tracks.end())
            continue;
        engine::TimelineClip projected{clip.id,    clip.asset,     clip.group,     track->type == TrackType::Audio,
                                       clip.start, clip.source_in, clip.source_out};
        projected.track = clip.track;
        projected.stream = clip.stream;
        projected.enabled = clip.enabled;
        projected.gain_db = clip.gain_db;
        projected.opacity = clip.opacity;
        projected.fade_in = clip.fade_in;
        projected.fade_out = clip.fade_out;
        projected.original_enabled = clip.original_enabled;
        result.clips.push_back(std::move(projected));
    }
    for (const auto& junction : snapshot.junctions) {
        if (junction.kind == TransitionKind::Crossfade)
            result.crossfades.push_back({junction.outgoing, junction.incoming, junction.duration});
        result.junctions.push_back({junction.id, junction.track, junction.outgoing, junction.incoming, junction.kind,
                                    junction.in_us, junction.out_us, junction.cut_us, junction.curve,
                                    junction.audio_law, junction.anchor});
    }
    return result;
}

} // namespace exosnap::edit
