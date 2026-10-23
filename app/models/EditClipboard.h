#pragma once

#include "EditCommandTypes.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace exosnap::edit {

inline std::shared_ptr<const ClipClipboard> CopyClips(const EditSnapshot& snapshot, const std::vector<Id>& ids) {
    std::set<Id> selected(ids.begin(), ids.end());
    if (selected.empty() || selected.size() != ids.size())
        return {};
    auto result = std::make_shared<ClipClipboard>();
    std::set<Id> sources;
    std::map<Id, size_t> groups;
    Time origin = (std::numeric_limits<Time>::max)();
    for (const auto& clip : snapshot.clips)
        if (selected.contains(clip.id)) {
            if (clip.start < 0 || clip.duration() <= 0 ||
                clip.start > (std::numeric_limits<Time>::max)() - clip.duration())
                return {};
            result->clips.push_back(clip);
            origin = std::min(origin, clip.start);
            sources.insert(clip.asset);
            if (clip.group)
                ++groups[clip.group];
        }
    if (result->clips.size() != selected.size())
        return {};
    result->tracks = snapshot.tracks;
    const auto track_order = [&](Id id) {
        return std::find_if(snapshot.tracks.begin(), snapshot.tracks.end(),
                            [id](const auto& track) { return track.id == id; }) -
               snapshot.tracks.begin();
    };
    std::sort(result->clips.begin(), result->clips.end(), [&](const auto& left, const auto& right) {
        return std::tuple{left.start, track_order(left.track), left.id} <
               std::tuple{right.start, track_order(right.track), right.id};
    });
    for (auto& clip : result->clips) {
        if (track_order(clip.track) == static_cast<std::ptrdiff_t>(snapshot.tracks.size()))
            return {};
        clip.start -= origin;
        result->duration_us = std::max(result->duration_us, clip.end());
        if (clip.group && groups[clip.group] < 2)
            clip.group = 0;
    }
    for (const auto& source : snapshot.assets)
        if (source && sources.contains(source->id))
            result->assets.push_back(source);
    if (result->assets.size() != sources.size())
        return {};
    for (auto junction : snapshot.junctions)
        if (selected.contains(junction.outgoing) && selected.contains(junction.incoming)) {
            junction.in_us -= origin;
            junction.out_us -= origin;
            junction.cut_us -= origin;
            junction.incoming_anchor_us -= origin;
            result->junctions.push_back(std::move(junction));
        }
    return result;
}

} // namespace exosnap::edit
