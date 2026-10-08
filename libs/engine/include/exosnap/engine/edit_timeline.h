#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace exosnap::engine {

struct TimelineClip {
    uint64_t id = 0;
    uint64_t asset = 0;
    uint64_t group = 0;
    bool audio = false;
    int64_t start_us = 0;
    int64_t source_in_us = 0;
    int64_t source_out_us = 0;
    [[nodiscard]] int64_t end_us() const {
        return start_us + source_out_us - source_in_us;
    }
};

struct TimelineCrossfade {
    uint64_t outgoing = 0;
    uint64_t incoming = 0;
    int64_t duration_us = 0;
};

struct TimelineSnapshot {
    std::vector<TimelineClip> clips;
    std::vector<TimelineCrossfade> crossfades;
    int64_t duration_us = 0;
};

struct TimelineContribution {
    uint64_t clip = 0;
    uint64_t asset = 0;
    int64_t source_us = 0;
    double weight = 1;
};

struct TimelineEvaluation {
    std::vector<TimelineContribution> video;
    std::vector<TimelineContribution> audio;
    bool gap = true;
    std::string error;
};

// Intervals are half-open. Both linked rows use the video relationship's
// interval, including when one side has no audio. No source handles are added.
inline TimelineEvaluation EvaluateTimeline(const TimelineSnapshot& timeline, int64_t time_us) {
    TimelineEvaluation result;
    const auto find = [&](uint64_t id) -> const TimelineClip* {
        const auto it = std::find_if(timeline.clips.begin(), timeline.clips.end(),
                                     [id](const auto& c) { return c.id == id && !c.audio; });
        return it == timeline.clips.end() ? nullptr : &*it;
    };
    for (const auto& transition : timeline.crossfades) {
        const auto* a = find(transition.outgoing);
        const auto* b = find(transition.incoming);
        if (!a || !b || a->id == b->id || a->start_us >= b->start_us || a->end_us() >= b->end_us() ||
            transition.duration_us <= 0 || a->end_us() - b->start_us != transition.duration_us) {
            result.error = "Invalid crossfade relationship.";
            return result;
        }
    }
    for (const auto& clip : timeline.clips) {
        if (time_us < clip.start_us || time_us >= clip.end_us())
            continue;
        double weight = 1;
        for (const auto& transition : timeline.crossfades) {
            const auto* outgoing = find(transition.outgoing);
            const auto* incoming = find(transition.incoming);
            if (time_us < incoming->start_us || time_us >= outgoing->end_us())
                continue;
            const double progress =
                static_cast<double>(time_us - incoming->start_us) / static_cast<double>(transition.duration_us);
            if (clip.id == outgoing->id || (clip.audio && clip.group && clip.group == outgoing->group))
                weight = 1 - progress;
            if (clip.id == incoming->id || (clip.audio && clip.group && clip.group == incoming->group))
                weight = progress;
        }
        auto& contributions = clip.audio ? result.audio : result.video;
        contributions.push_back({clip.id, clip.asset, clip.source_in_us + time_us - clip.start_us, weight});
    }
    result.gap = result.video.empty();
    if (result.video.size() > 2)
        result.error = "More than two simultaneous video sources are unsupported.";
    if (result.video.size() == 2 &&
        !std::any_of(timeline.crossfades.begin(), timeline.crossfades.end(), [&](const auto& transition) {
            return (transition.outgoing == result.video[0].clip && transition.incoming == result.video[1].clip) ||
                   (transition.outgoing == result.video[1].clip && transition.incoming == result.video[0].clip);
        }))
        result.error = "Overlapping video sources have no valid crossfade relationship.";
    return result;
}

} // namespace exosnap::engine
