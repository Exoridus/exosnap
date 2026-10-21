#pragma once

#include "EditCommandTypes.h"
#include "EditRenderSnapshot.h"

#include <algorithm>
#include <set>
#include <tuple>

namespace exosnap::edit {

enum class SelectionMode { None, Clips, Range, Junction, Gap, Spacer, Track };
struct EditRange {
    Time start_us = 0;
    Time end_us = 0;
    std::vector<Id> track_ids;
    Time anchor_us = 0;
    bool operator==(const EditRange&) const = default;
};

enum class RangeValidity { Valid, EmptyScope, InvalidTrack, OutOfBounds, Collapsed };
struct EditRangePreview {
    EditRange range;
    RangeValidity validity = RangeValidity::Collapsed;
    bool valid() const {
        return validity == RangeValidity::Valid;
    }
};

inline EditRangePreview PreviewEditRange(const EditSnapshot& snapshot, Time start, Time end,
                                         const std::vector<Id>& tracks, Time anchor = -1) {
    EditRangePreview preview;
    auto& result = preview.range;
    result.start_us = std::min(start, end);
    result.end_us = std::max(start, end);
    result.anchor_us = anchor < 0 ? start : anchor;
    const auto duration = SnapshotDuration(snapshot);
    if (tracks.empty()) {
        preview.validity = RangeValidity::EmptyScope;
        return preview;
    }
    std::set<Id> requested;
    for (const auto id : tracks)
        if (!requested.insert(id).second || std::none_of(snapshot.tracks.begin(), snapshot.tracks.end(),
                                                         [id](const auto& track) { return track.id == id; })) {
            preview.validity = RangeValidity::InvalidTrack;
            return preview;
        }
    bool audio = true;
    for (const auto& track : snapshot.tracks)
        if (requested.contains(track.id)) {
            result.track_ids.push_back(track.id);
            audio = audio && track.type == TrackType::Audio;
        }
    if (start < 0 || end < 0 || start > duration || end > duration || result.anchor_us > duration) {
        preview.validity = RangeValidity::OutOfBounds;
        return preview;
    }
    const auto from = BoundedGridTime(result.start_us, 0, duration, snapshot.output_fps, audio);
    const auto to = BoundedGridTime(result.end_us, 0, duration, snapshot.output_fps, audio);
    const auto fixed = BoundedGridTime(result.anchor_us, 0, duration, snapshot.output_fps, audio);
    if (!from || !to || !fixed) {
        preview.validity = RangeValidity::OutOfBounds;
        return preview;
    }
    result.start_us = *from;
    result.end_us = *to;
    result.anchor_us = *fixed;
    preview.validity = *from < *to ? RangeValidity::Valid : RangeValidity::Collapsed;
    return preview;
}

inline std::optional<EditRange> MakeEditRange(const EditSnapshot& snapshot, Time start, Time end,
                                              const std::vector<Id>& tracks, Time anchor = -1) {
    auto preview = PreviewEditRange(snapshot, start, end, tracks, anchor);
    return preview.valid() ? std::optional<EditRange>{std::move(preview.range)} : std::nullopt;
}

inline std::vector<Id> OrderedClipIds(const EditSnapshot& snapshot) {
    std::vector<const Clip*> clips;
    for (const auto& clip : snapshot.clips)
        clips.push_back(&clip);
    const auto trackOrder = [&](Id id) {
        return std::find_if(snapshot.tracks.begin(), snapshot.tracks.end(),
                            [id](const auto& track) { return track.id == id; }) -
               snapshot.tracks.begin();
    };
    std::sort(clips.begin(), clips.end(), [&](const auto* left, const auto* right) {
        return std::tuple{trackOrder(left->track), left->start, left->id} <
               std::tuple{trackOrder(right->track), right->start, right->id};
    });
    std::vector<Id> ids;
    for (const auto* clip : clips)
        ids.push_back(clip->id);
    return ids;
}

class EditSelection {
  public:
    SelectionMode mode = SelectionMode::None;
    std::vector<Id> clip_ids;
    Id primary_clip = 0;
    Id anchor_clip = 0;
    Id active_track = 0;
    std::optional<EditRange> range;

    bool selectClip(const EditSnapshot& snapshot, Id id, bool toggle, bool extend, bool linked) {
        if (toggle && extend)
            return false;
        const auto target = std::find_if(snapshot.clips.begin(), snapshot.clips.end(),
                                         [id](const auto& clip) { return clip.id == id; });
        if (target == snapshot.clips.end())
            return false;
        const auto ordered = OrderedClipIds(snapshot);
        std::set<Id> requested{id};
        const auto anchor = std::find(ordered.begin(), ordered.end(), anchor_clip);
        if (extend && anchor != ordered.end()) {
            const auto clicked = std::find(ordered.begin(), ordered.end(), id);
            requested = std::set<Id>(std::min(anchor, clicked), std::max(anchor, clicked) + 1);
        }
        if (linked) {
            std::set<Id> groups;
            for (const auto& clip : snapshot.clips)
                if (requested.contains(clip.id) && clip.group)
                    groups.insert(clip.group);
            for (const auto& clip : snapshot.clips)
                if (clip.group && groups.contains(clip.group))
                    requested.insert(clip.id);
        }
        std::set<Id> selected;
        if (mode == SelectionMode::Clips &&
            (toggle || (!extend && std::find(clip_ids.begin(), clip_ids.end(), id) != clip_ids.end())))
            selected.insert(clip_ids.begin(), clip_ids.end());
        const bool remove = toggle && std::all_of(requested.begin(), requested.end(),
                                                  [&](Id value) { return selected.contains(value); });
        for (const auto value : requested)
            if (remove)
                selected.erase(value);
            else
                selected.insert(value);
        clip_ids.clear();
        for (const auto value : ordered)
            if (selected.contains(value))
                clip_ids.push_back(value);
        range.reset();
        mode = clip_ids.empty() ? SelectionMode::None : SelectionMode::Clips;
        primary_clip = selected.contains(id) ? id : clip_ids.empty() ? 0 : clip_ids.front();
        if (!extend || anchor == ordered.end())
            anchor_clip = id;
        active_track = target->track;
        return true;
    }
    void selectAll(const EditSnapshot& snapshot) {
        clear();
        clip_ids = OrderedClipIds(snapshot);
        if (!clip_ids.empty()) {
            mode = SelectionMode::Clips;
            primary_clip = anchor_clip = clip_ids.front();
        }
    }
    void prune(const EditSnapshot& snapshot) {
        const auto exists = [&](Id id) {
            return std::any_of(snapshot.clips.begin(), snapshot.clips.end(),
                               [id](const auto& clip) { return clip.id == id; });
        };
        std::erase_if(clip_ids, [&](Id id) { return !exists(id); });
        if (!exists(primary_clip))
            primary_clip = clip_ids.empty() ? 0 : clip_ids.front();
        if (!exists(anchor_clip))
            anchor_clip = 0;
        if (mode == SelectionMode::Clips && clip_ids.empty())
            mode = SelectionMode::None;
        const auto trackExists = [&](Id id) {
            return std::any_of(snapshot.tracks.begin(), snapshot.tracks.end(),
                               [id](const auto& track) { return track.id == id; });
        };
        if (!trackExists(active_track))
            active_track = 0;
        if (range) {
            std::erase_if(range->track_ids, [&](Id id) { return !trackExists(id); });
            const auto end = SnapshotDuration(snapshot);
            range->start_us = std::clamp(range->start_us, Time{0}, end);
            range->end_us = std::clamp(range->end_us, range->start_us, end);
            range->anchor_us = std::clamp(range->anchor_us, Time{0}, end);
            if (range->track_ids.empty() || range->start_us == range->end_us)
                clear();
        }
    }
    void clear() {
        mode = SelectionMode::None;
        clip_ids.clear();
        primary_clip = 0;
        anchor_clip = 0;
        range.reset();
    }
    void setRange(EditRange value) {
        clear();
        mode = SelectionMode::Range;
        range = std::move(value);
    }
};

} // namespace exosnap::edit
