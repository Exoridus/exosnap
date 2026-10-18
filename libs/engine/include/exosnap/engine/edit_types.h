#pragma once

#include <exosnap/engine/recording_media_types.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace exosnap::engine {

enum class TimelineStreamKind { Video, Audio };

// Half-open interval on the normalized 48 kHz output sample grid.
struct AudioSampleSpan {
    int64_t first = 0;
    int64_t end = 0;
    bool operator==(const AudioSampleSpan&) const = default;
};

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

[[nodiscard]] inline std::optional<double> EvaluateCurve(CurveSpec curve, double progress) {
    if (!std::isfinite(progress) || !std::isfinite(curve.bias) || curve.bias < -1 || curve.bias > 1)
        return std::nullopt;
    const double q = std::clamp(progress, 0.0, 1.0);
    switch (curve.kind) {
    case CurveKind::Linear:
        return q;
    case CurveKind::Smooth:
        return q * q * (3 - 2 * q);
    case CurveKind::Bias:
        return curve.bias >= 0 ? std::pow(q, std::pow(2.0, 2 * curve.bias))
                               : 1 - std::pow(1 - q, std::pow(2.0, -2 * curve.bias));
    }
    return std::nullopt;
}

} // namespace exosnap::engine
