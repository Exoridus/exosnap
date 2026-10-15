#pragma once

#include <exosnap/engine/edit_types.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <set>
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
        return start_us + (source_out_us - source_in_us);
    }
    uint64_t track = 0;
    std::optional<TimelineStreamRef> stream;
    bool enabled = true;
    double gain_db = 0;
    double opacity = 1;
    EdgeFade fade_in;
    EdgeFade fade_out;
    bool original_enabled = true;
    bool operator==(const TimelineClip&) const = default;
};

struct TimelineCrossfade {
    uint64_t outgoing = 0;
    uint64_t incoming = 0;
    int64_t duration_us = 0;
};

struct TimelineTrack {
    uint64_t id = 0;
    bool audio = false;
    bool enabled = true;
    double gain_db = 0;
    bool operator==(const TimelineTrack&) const = default;
};

struct TimelineJunction {
    uint64_t id = 0;
    uint64_t track = 0;
    uint64_t outgoing = 0;
    uint64_t incoming = 0;
    JunctionKind kind = JunctionKind::Crossfade;
    int64_t in_us = 0;
    int64_t out_us = 0;
    int64_t cut_us = 0;
    CurveSpec curve;
    AudioGainLaw audio_law = AudioGainLaw::EqualGain;
    JunctionAnchor anchor = JunctionAnchor::FullOverlap;
    bool operator==(const TimelineJunction&) const = default;
};

struct TimelineSnapshot {
    std::vector<TimelineClip> clips;
    std::vector<TimelineCrossfade> crossfades;
    int64_t duration_us = 0;
    std::vector<TimelineTrack> tracks;
    std::vector<TimelineJunction> junctions;
    uint64_t revision = 0;
    int64_t result_in_us = 0;
    int64_t result_out_us = -1;
    uint32_t output_fps_num = 0;
    uint32_t output_fps_den = 1;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
};

struct TimelineContribution {
    uint64_t clip = 0;
    uint64_t asset = 0;
    int64_t source_us = 0;
    double weight = 1;
    uint64_t track = 0;
    std::optional<TimelineStreamRef> stream;
    double opacity = 1;
};

struct VideoTrackLayer {
    uint64_t track = 0;
    std::vector<TimelineContribution> contributions;
};

struct AudioContribution {
    uint64_t clip = 0;
    uint64_t asset = 0;
    uint64_t track = 0;
    std::optional<TimelineStreamRef> stream;
    int64_t source_us = 0;
    double gain = 1;
};

struct TimelineEvaluation {
    std::vector<TimelineContribution> video;
    std::vector<TimelineContribution> audio;
    bool gap = true;
    std::string error;
    std::vector<VideoTrackLayer> video_layers;
    std::vector<AudioContribution> audio_contributions;
    bool video_gap = true;
    bool audio_gap = true;
};

namespace timeline_detail {

inline const TimelineClip* Find(const TimelineSnapshot& snapshot, uint64_t id) {
    const auto found =
        std::find_if(snapshot.clips.begin(), snapshot.clips.end(), [id](const auto& clip) { return clip.id == id; });
    return found == snapshot.clips.end() ? nullptr : &*found;
}

inline bool Overlap(const TimelineClip& outgoing, const TimelineClip& incoming) {
    return outgoing.track == incoming.track && outgoing.audio == incoming.audio &&
           outgoing.start_us < incoming.start_us && incoming.start_us < outgoing.end_us() &&
           outgoing.end_us() < incoming.end_us();
}

inline std::optional<double> JunctionGain(const TimelineJunction& junction, bool incoming, int64_t time_us,
                                          bool audio) {
    if (junction.kind == JunctionKind::HardCut)
        return incoming == (time_us >= junction.cut_us) ? 1.0 : 0.0;
    if (time_us < junction.in_us)
        return incoming ? 0.0 : 1.0;
    if (time_us >= junction.out_us)
        return incoming ? 1.0 : 0.0;
    const auto progress = EvaluateCurve(junction.curve, static_cast<double>(time_us - junction.in_us) /
                                                            static_cast<double>(junction.out_us - junction.in_us));
    if (!progress)
        return std::nullopt;
    if (audio && junction.audio_law == AudioGainLaw::EqualPower) {
        const auto angle = std::numbers::pi * *progress / 2;
        return incoming ? std::sin(angle) : std::cos(angle);
    }
    return incoming ? *progress : 1 - *progress;
}

inline std::optional<double> EdgeGain(const TimelineClip& clip, int64_t time_us) {
    double gain = 1;
    if (clip.fade_in.duration_us > 0 && time_us - clip.start_us < clip.fade_in.duration_us) {
        const auto fade = EvaluateCurve(clip.fade_in.curve, static_cast<double>(time_us - clip.start_us) /
                                                                static_cast<double>(clip.fade_in.duration_us));
        if (!fade)
            return std::nullopt;
        gain *= *fade;
    }
    if (clip.fade_out.duration_us > 0 && clip.end_us() - time_us <= clip.fade_out.duration_us) {
        const auto fade = EvaluateCurve(clip.fade_out.curve,
                                        static_cast<double>(time_us - (clip.end_us() - clip.fade_out.duration_us)) /
                                            static_cast<double>(clip.fade_out.duration_us));
        if (!fade)
            return std::nullopt;
        gain *= 1 - *fade;
    }
    return gain;
}

inline std::vector<TimelineTrack> Tracks(const TimelineSnapshot& snapshot) {
    if (!snapshot.tracks.empty())
        return snapshot.tracks;
    std::vector<TimelineTrack> result;
    for (const auto& clip : snapshot.clips)
        if (std::none_of(result.begin(), result.end(),
                         [&](const auto& track) { return track.id == clip.track && track.audio == clip.audio; }))
            result.push_back({clip.track, clip.audio});
    return result;
}

inline bool Validate(const TimelineSnapshot& snapshot, std::vector<TimelineJunction>& junctions, std::string& error) {
    std::set<uint64_t> track_ids;
    for (const auto& track : snapshot.tracks)
        if (!track.id || !track_ids.insert(track.id).second) {
            error = "Invalid timeline track identity.";
            return false;
        }
    std::set<uint64_t> ids;
    for (const auto& clip : snapshot.clips) {
        if (!clip.id || !ids.insert(clip.id).second || clip.start_us < 0 || clip.source_in_us < 0 ||
            clip.source_out_us <= clip.source_in_us ||
            clip.start_us > (std::numeric_limits<int64_t>::max)() - (clip.source_out_us - clip.source_in_us) ||
            !std::isfinite(clip.opacity) || clip.opacity < 0 || clip.opacity > 1 || !std::isfinite(clip.gain_db) ||
            clip.fade_in.duration_us < 0 || clip.fade_out.duration_us < 0 ||
            clip.fade_in.duration_us > clip.source_out_us - clip.source_in_us ||
            clip.fade_out.duration_us > clip.source_out_us - clip.source_in_us - clip.fade_in.duration_us ||
            !EvaluateCurve(clip.fade_in.curve, 0) || !EvaluateCurve(clip.fade_out.curve, 0)) {
            error = "Invalid timeline clip.";
            return false;
        }
        if (!snapshot.tracks.empty() &&
            std::none_of(snapshot.tracks.begin(), snapshot.tracks.end(),
                         [&](const auto& track) { return track.id == clip.track && track.audio == clip.audio; })) {
            error = "Timeline clip refers to an unavailable track.";
            return false;
        }
    }
    junctions = snapshot.junctions;
    for (const auto& legacy : snapshot.crossfades) {
        if (std::any_of(junctions.begin(), junctions.end(), [&](const auto& junction) {
                return junction.outgoing == legacy.outgoing && junction.incoming == legacy.incoming;
            }))
            continue;
        const auto* outgoing = Find(snapshot, legacy.outgoing);
        const auto* incoming = Find(snapshot, legacy.incoming);
        if (!outgoing || !incoming || outgoing->audio || incoming->audio || !Overlap(*outgoing, *incoming) ||
            legacy.duration_us != outgoing->end_us() - incoming->start_us) {
            error = "Invalid crossfade relationship.";
            return false;
        }
        junctions.push_back({0, outgoing->track, outgoing->id, incoming->id, JunctionKind::Crossfade,
                             incoming->start_us, outgoing->end_us()});
    }
    const auto video_junctions = junctions;
    for (const auto& video : video_junctions) {
        const auto* outgoing = Find(snapshot, video.outgoing);
        const auto* incoming = Find(snapshot, video.incoming);
        if (!outgoing || !incoming || outgoing->audio || incoming->audio || !outgoing->group || !incoming->group)
            continue;
        for (const auto& a : snapshot.clips) {
            if (!a.audio || a.group != outgoing->group)
                continue;
            for (const auto& b : snapshot.clips) {
                if (!b.audio || b.group != incoming->group || !Overlap(a, b) ||
                    std::any_of(junctions.begin(), junctions.end(), [&](const auto& junction) {
                        return junction.outgoing == a.id && junction.incoming == b.id;
                    }))
                    continue;
                junctions.push_back({0, a.track, a.id, b.id, JunctionKind::Crossfade, b.start_us, a.end_us()});
            }
        }
    }
    for (const auto& junction : junctions) {
        const auto* a = Find(snapshot, junction.outgoing);
        const auto* b = Find(snapshot, junction.incoming);
        if (!a || !b || !Overlap(*a, *b) || junction.track != a->track ||
            (junction.kind != JunctionKind::Crossfade && junction.kind != JunctionKind::HardCut) ||
            (junction.kind == JunctionKind::Crossfade &&
             (junction.in_us < b->start_us || junction.out_us > a->end_us() || junction.out_us <= junction.in_us)) ||
            (junction.kind == JunctionKind::HardCut &&
             (junction.cut_us < b->start_us || junction.cut_us > a->end_us())) ||
            !EvaluateCurve(junction.curve, 0) ||
            (junction.audio_law != AudioGainLaw::EqualGain && junction.audio_law != AudioGainLaw::EqualPower)) {
            error = "Invalid timeline junction.";
            return false;
        }
    }
    for (const auto& track : Tracks(snapshot)) {
        if (!std::isfinite(track.gain_db)) {
            error = "Invalid track gain.";
            return false;
        }
        std::vector<const TimelineClip*> clips;
        for (const auto& clip : snapshot.clips)
            if (clip.track == track.id && clip.audio == track.audio)
                clips.push_back(&clip);
        std::sort(clips.begin(), clips.end(), [](const auto* left, const auto* right) {
            return left->start_us != right->start_us ? left->start_us < right->start_us : left->id < right->id;
        });
        for (size_t index = 1; index < clips.size(); ++index) {
            const auto& a = *clips[index - 1];
            const auto& b = *clips[index];
            if (a.end_us() <= b.start_us)
                continue;
            if (!Overlap(a, b) || (index > 1 && clips[index - 2]->end_us() > b.start_us) ||
                std::count_if(junctions.begin(), junctions.end(), [&](const auto& junction) {
                    return junction.outgoing == a.id && junction.incoming == b.id;
                }) != 1) {
                error = "Invalid same-track overlap.";
                return false;
            }
        }
    }
    return true;
}

} // namespace timeline_detail

inline TimelineEvaluation EvaluateTimeline(const TimelineSnapshot& timeline, int64_t time_us) {
    TimelineEvaluation result;
    std::vector<TimelineJunction> junctions;
    if (!timeline_detail::Validate(timeline, junctions, result.error))
        return result;
    const auto tracks = timeline_detail::Tracks(timeline);
    for (auto track = tracks.rbegin(); track != tracks.rend(); ++track) {
        if (!track->enabled)
            continue;
        VideoTrackLayer layer{track->id, {}};
        for (const auto& clip : timeline.clips) {
            if (clip.track != track->id || clip.audio != track->audio || !clip.enabled || time_us < clip.start_us ||
                time_us >= clip.end_us())
                continue;
            double weight = 1;
            for (const auto& junction : junctions) {
                if (junction.track != track->id || (junction.outgoing != clip.id && junction.incoming != clip.id))
                    continue;
                const auto* outgoing = timeline_detail::Find(timeline, junction.outgoing);
                const auto* incoming = timeline_detail::Find(timeline, junction.incoming);
                if (time_us < incoming->start_us || time_us >= outgoing->end_us())
                    continue;
                const auto gain =
                    timeline_detail::JunctionGain(junction, junction.incoming == clip.id, time_us, clip.audio);
                if (!gain) {
                    result.error = "Invalid junction envelope.";
                    return result;
                }
                weight *= *gain;
            }
            const auto edge = timeline_detail::EdgeGain(clip, time_us);
            if (!edge) {
                result.error = "Invalid edge envelope.";
                return result;
            }
            if (weight <= 0)
                continue;
            TimelineContribution contribution{
                clip.id,     clip.asset,          clip.source_in_us + (time_us - clip.start_us), weight, clip.track,
                clip.stream, clip.opacity * *edge};
            if (clip.audio) {
                const double gain = weight * *edge * std::pow(10.0, (clip.gain_db + track->gain_db) / 20);
                if (!std::isfinite(gain)) {
                    result.error = "Audio gain is outside the supported numeric range.";
                    return result;
                }
                contribution.weight = gain;
                contribution.opacity = 1;
                result.audio.push_back(contribution);
                result.audio_contributions.push_back(
                    {clip.id, clip.asset, clip.track, clip.stream, contribution.source_us, gain});
            } else {
                layer.contributions.push_back(contribution);
            }
        }
        if (!track->audio && !layer.contributions.empty()) {
            std::sort(layer.contributions.begin(), layer.contributions.end(), [&](const auto& left, const auto& right) {
                return timeline_detail::Find(timeline, left.clip)->start_us <
                       timeline_detail::Find(timeline, right.clip)->start_us;
            });
            for (const auto& contribution : layer.contributions)
                result.video.push_back(contribution);
            result.video_layers.push_back(std::move(layer));
        }
    }
    result.video_gap = result.video.empty();
    result.audio_gap = result.audio.empty();
    result.gap = result.video_gap;
    return result;
}

struct AudioIntervalContribution {
    uint64_t clip = 0;
    uint64_t asset = 0;
    uint64_t track = 0;
    std::optional<TimelineStreamRef> stream;
    int64_t clip_start_us = 0;
    int64_t source_in_us = 0;
    int64_t source_out_us = 0;
    double gain_linear = 1;
    EdgeFade fade_in;
    EdgeFade fade_out;
    std::vector<TimelineJunction> junctions;
};

struct AudioInterval {
    int64_t in_us = 0;
    int64_t out_us = 0;
    std::vector<AudioIntervalContribution> contributions;
};

struct AudioIntervalEvaluation {
    std::vector<AudioInterval> intervals;
    std::string error;
};

inline int64_t AudioSampleIndex(int64_t time_us) {
    const auto whole = (time_us / 1'000'000) * 48000;
    const auto remainder = time_us % 1'000'000;
    const auto fraction = ((remainder < 0 ? -remainder : remainder) * 48000 + 500'000) / 1'000'000;
    return whole + (remainder < 0 ? -fraction : fraction);
}

inline std::optional<int64_t> AudioSourceSample(const AudioIntervalContribution& contribution,
                                                int64_t absolute_output_sample) {
    if (contribution.source_in_us < 0 || contribution.clip_start_us < 0 || absolute_output_sample < 0)
        return std::nullopt;
    const auto offset = AudioSampleIndex(contribution.source_in_us - contribution.clip_start_us);
    if ((offset > 0 && absolute_output_sample > (std::numeric_limits<int64_t>::max)() - offset) ||
        (offset < 0 && absolute_output_sample < -offset))
        return std::nullopt;
    return absolute_output_sample + offset;
}

namespace timeline_detail {

inline std::optional<double> SampleProgress(int64_t sample, int64_t in_us, int64_t out_us) {
    if (sample < 0 || in_us < 0 || out_us <= in_us)
        return std::nullopt;
    const auto whole = (in_us / 1'000'000) * 48000;
    const double fraction = static_cast<double>(in_us % 1'000'000) * 48000 / 1'000'000;
    return (static_cast<double>(sample - whole) - fraction) / (static_cast<double>(out_us - in_us) * 48000 / 1'000'000);
}

} // namespace timeline_detail

inline std::optional<double> EvaluateAudioGain(const AudioIntervalContribution& contribution,
                                               int64_t absolute_output_sample) {
    if (absolute_output_sample < 0 || !std::isfinite(contribution.gain_linear) || contribution.gain_linear < 0 ||
        contribution.clip_start_us < 0 || contribution.source_in_us < 0 ||
        contribution.source_out_us <= contribution.source_in_us ||
        contribution.clip_start_us >
            (std::numeric_limits<int64_t>::max)() - (contribution.source_out_us - contribution.source_in_us))
        return std::nullopt;
    const auto end = contribution.clip_start_us + (contribution.source_out_us - contribution.source_in_us);
    if (absolute_output_sample < AudioSampleIndex(contribution.clip_start_us) ||
        absolute_output_sample >= AudioSampleIndex(end))
        return 0;
    double gain = contribution.gain_linear;
    if (contribution.fade_in.duration_us < 0 || contribution.fade_out.duration_us < 0 ||
        contribution.fade_in.duration_us > end - contribution.clip_start_us ||
        contribution.fade_out.duration_us > end - contribution.clip_start_us - contribution.fade_in.duration_us)
        return std::nullopt;
    if (contribution.fade_in.duration_us > 0) {
        const auto progress =
            timeline_detail::SampleProgress(absolute_output_sample, contribution.clip_start_us,
                                            contribution.clip_start_us + contribution.fade_in.duration_us);
        const auto envelope = progress ? EvaluateCurve(contribution.fade_in.curve, *progress) : std::nullopt;
        if (!envelope)
            return std::nullopt;
        gain *= *envelope;
    }
    if (contribution.fade_out.duration_us > 0) {
        const auto progress =
            timeline_detail::SampleProgress(absolute_output_sample, end - contribution.fade_out.duration_us, end);
        const auto envelope = progress ? EvaluateCurve(contribution.fade_out.curve, *progress) : std::nullopt;
        if (!envelope)
            return std::nullopt;
        gain *= 1 - *envelope;
    }
    for (const auto& junction : contribution.junctions) {
        const bool incoming = junction.incoming == contribution.clip;
        if (!incoming && junction.outgoing != contribution.clip)
            return std::nullopt;
        double weight = 0;
        if (junction.kind == JunctionKind::HardCut) {
            weight = incoming == (absolute_output_sample >= AudioSampleIndex(junction.cut_us)) ? 1.0 : 0.0;
        } else {
            const auto progress =
                timeline_detail::SampleProgress(absolute_output_sample, junction.in_us, junction.out_us);
            const auto shaped = progress ? EvaluateCurve(junction.curve, *progress) : std::nullopt;
            if (!shaped)
                return std::nullopt;
            if (junction.audio_law == AudioGainLaw::EqualGain) {
                weight = incoming ? *shaped : 1 - *shaped;
            } else if (junction.audio_law == AudioGainLaw::EqualPower) {
                weight = incoming ? std::sin(std::numbers::pi * *shaped / 2) : std::cos(std::numbers::pi * *shaped / 2);
            } else {
                return std::nullopt;
            }
        }
        gain *= weight;
    }
    return std::isfinite(gain) ? std::optional<double>{gain} : std::nullopt;
}

inline AudioIntervalEvaluation EvaluateAudioIntervals(const TimelineSnapshot& timeline, int64_t in_us, int64_t out_us) {
    AudioIntervalEvaluation result;
    if (in_us < 0 || out_us < in_us) {
        result.error = "Invalid audio interval.";
        return result;
    }
    std::vector<TimelineJunction> junctions;
    if (!timeline_detail::Validate(timeline, junctions, result.error) || in_us == out_us)
        return result;
    std::vector<int64_t> boundaries{in_us, out_us};
    auto add = [&](int64_t value) {
        if (in_us < value && value < out_us)
            boundaries.push_back(value);
    };
    for (const auto& clip : timeline.clips) {
        if (!clip.audio)
            continue;
        add(clip.start_us);
        add(clip.end_us());
        add(clip.start_us + clip.fade_in.duration_us);
        add(clip.end_us() - clip.fade_out.duration_us);
    }
    for (const auto& junction : junctions) {
        const auto* outgoing = timeline_detail::Find(timeline, junction.outgoing);
        if (!outgoing || !outgoing->audio)
            continue;
        if (junction.kind == JunctionKind::HardCut)
            add(junction.cut_us);
        else {
            add(junction.in_us);
            add(junction.out_us);
        }
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    const auto tracks = timeline_detail::Tracks(timeline);
    for (size_t index = 1; index < boundaries.size(); ++index) {
        AudioInterval interval{boundaries[index - 1], boundaries[index], {}};
        for (const auto& clip : timeline.clips) {
            if (!clip.audio || !clip.enabled || clip.start_us >= interval.out_us || clip.end_us() <= interval.in_us)
                continue;
            const auto track = std::find_if(tracks.begin(), tracks.end(),
                                            [&](const auto& value) { return value.audio && value.id == clip.track; });
            if (track == tracks.end() || !track->enabled)
                continue;
            const auto gain = std::pow(10.0, (track->gain_db + clip.gain_db) / 20);
            if (!std::isfinite(gain)) {
                result.error = "Audio gain is outside the supported numeric range.";
                result.intervals.clear();
                return result;
            }
            AudioIntervalContribution contribution{
                clip.id, clip.asset,   clip.track,    clip.stream, clip.start_us, clip.source_in_us, clip.source_out_us,
                gain,    clip.fade_in, clip.fade_out, {}};
            for (const auto& junction : junctions)
                if (junction.outgoing == clip.id || junction.incoming == clip.id)
                    contribution.junctions.push_back(junction);
            interval.contributions.push_back(std::move(contribution));
        }
        result.intervals.push_back(std::move(interval));
    }
    return result;
}

} // namespace exosnap::engine
