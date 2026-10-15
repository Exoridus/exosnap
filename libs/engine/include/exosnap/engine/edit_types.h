#pragma once

#include <exosnap/engine/recording_media_types.h>

#include <cstdint>
#include <optional>

namespace exosnap::engine {

enum class TimelineStreamKind { Video, Audio };

struct TimelineStreamRef {
    uint64_t asset_id = 0;
    AudioTrackId stream_identity;
    uint32_t container_index = 0;
    TimelineStreamKind kind = TimelineStreamKind::Video;
    bool operator==(const TimelineStreamRef&) const = default;
};

enum class CurveKind { Linear, Smooth, Bias };

struct CurveSpec {
    CurveKind kind = CurveKind::Linear;
    double bias = 0;
    bool operator==(const CurveSpec&) const = default;
};

enum class AudioGainLaw { EqualGain, EqualPower };

struct EdgeFade {
    int64_t duration_us = 0;
    CurveSpec curve;
    bool operator==(const EdgeFade&) const = default;
};

enum class JunctionKind { Crossfade, HardCut };
enum class JunctionAnchor { FullOverlap, Custom };

[[nodiscard]] std::optional<double> EvaluateCurve(CurveSpec curve, double progress);

} // namespace exosnap::engine
