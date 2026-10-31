#pragma once

#include <exosnap/engine/edit_automation.h>

#include <limits>

namespace exosnap::engine {

inline int64_t AudioSampleIndex(int64_t time_us) {
    const auto whole = (time_us / 1'000'000) * 48000;
    const auto remainder = time_us % 1'000'000;
    const auto fraction = ((remainder < 0 ? -remainder : remainder) * 48000 + 500'000) / 1'000'000;
    return whole + (remainder < 0 ? -fraction : fraction);
}

inline int64_t AudioSampleTime(int64_t sample) {
    if (sample < 0 || sample > AudioSampleIndex((std::numeric_limits<int64_t>::max)()))
        return -1;
    const auto whole = (sample / 48000) * 1'000'000;
    const auto fraction = ((sample % 48000) * 1'000'000 + 24000) / 48000;
    return whole > (std::numeric_limits<int64_t>::max)() - fraction ? -1 : whole + fraction;
}

struct RangeGainRamp {
    int64_t in_sample = 0;
    int64_t out_sample = 0;
    int64_t ramp_samples = 0;
    double delta_db = 0;
    bool operator==(const RangeGainRamp&) const = default;
};

struct GainEnvelope {
    PropertyChannel channel;
    std::vector<RangeGainRamp> ranges;
    bool operator==(const GainEnvelope&) const = default;
};

inline double EvaluateGainChannel(const PropertyChannel& channel, int64_t sample) {
    double value = 0;
    const auto& keys = channel.keys;
    const auto right = std::upper_bound(keys.begin(), keys.end(), sample, [](int64_t at, const auto& key) {
        return at < AudioSampleIndex(key.source_us);
    });
    if (!keys.empty()) {
        if (right == keys.begin()) {
            value = right->value;
        } else {
            const auto& left = *std::prev(right);
            value = left.value;
            if (right != keys.end() && left.interpolation != KeyInterpolation::Hold) {
                double t = static_cast<double>(sample - AudioSampleIndex(left.source_us)) /
                           static_cast<double>(AudioSampleIndex(right->source_us) - AudioSampleIndex(left.source_us));
                if (left.interpolation == KeyInterpolation::Smooth)
                    t = t * t * (3 - 2 * t);
                value += (right->value - left.value) * t;
            }
        }
    }
    return value;
}

inline double EvaluateRangeGain(const RangeGainRamp& range, int64_t sample) {
    if (sample < range.in_sample || sample >= range.out_sample)
        return 0;
    const auto weight = range.ramp_samples > 0
                            ? std::min({1.0, static_cast<double>(sample - range.in_sample) / range.ramp_samples,
                                        static_cast<double>(range.out_sample - sample) / range.ramp_samples})
                            : 1;
    return range.delta_db * weight;
}

inline double EvaluateGainEnvelope(const GainEnvelope& envelope, int64_t sample) {
    double value = EvaluateGainChannel(envelope.channel, sample);
    for (const auto& range : envelope.ranges)
        value += EvaluateRangeGain(range, sample);
    return value;
}

// Independent maxima conservatively retain a peak when one pixel spans several automation segments.
inline double PeakGainEnvelope(const GainEnvelope& envelope, int64_t first, int64_t last) {
    double value = std::max(EvaluateGainChannel(envelope.channel, first), EvaluateGainChannel(envelope.channel, last));
    for (const auto& key : envelope.channel.keys) {
        const auto at = AudioSampleIndex(key.source_us);
        if (at >= first && at <= last)
            value = std::max(value, key.value);
    }
    for (const auto& range : envelope.ranges) {
        double maximum = std::max(EvaluateRangeGain(range, first), EvaluateRangeGain(range, last));
        for (const auto at : {range.in_sample, range.in_sample + range.ramp_samples,
                              range.out_sample - range.ramp_samples, range.out_sample})
            if (at >= first && at <= last)
                maximum = std::max(maximum, EvaluateRangeGain(range, at));
        value += maximum;
    }
    return value;
}

inline bool ValidGainEnvelope(const GainEnvelope& envelope, double clip_gain_db) {
    if (!ValidChannel(envelope.channel, -84, 84) || !std::isfinite(clip_gain_db))
        return false;
    if (envelope.channel.keys.empty() && envelope.ranges.empty())
        return true;
    std::vector<int64_t> boundaries{0};
    int64_t previous = -1;
    for (const auto& key : envelope.channel.keys) {
        const auto sample = AudioSampleIndex(key.source_us);
        if (sample <= previous)
            return false;
        previous = sample;
        boundaries.push_back(sample);
    }
    for (const auto& range : envelope.ranges) {
        if (range.in_sample <= -(std::numeric_limits<int64_t>::max)() / 4 ||
            range.out_sample >= (std::numeric_limits<int64_t>::max)() / 4 || range.out_sample <= range.in_sample ||
            range.ramp_samples < 0 || range.ramp_samples > (range.out_sample - range.in_sample) / 4 ||
            !std::isfinite(range.delta_db) || std::abs(range.delta_db) > 84)
            return false;
        for (const auto at : {range.in_sample, range.in_sample + range.ramp_samples,
                              range.out_sample - range.ramp_samples, range.out_sample})
            if (at >= 0)
                boundaries.push_back(at);
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    const auto valid = [&](int64_t at) {
        const auto db = clip_gain_db + EvaluateGainEnvelope(envelope, at);
        return std::isfinite(db) && db >= -60 - 1e-9 && db <= 24 + 1e-9;
    };
    for (const auto at : boundaries)
        if (!valid(at) || (at > 0 && !valid(at - 1)))
            return false;
    // A smooth key segment plus linear ramps can have an extremum between boundaries.
    for (size_t i = 1; i < boundaries.size(); ++i) {
        const auto begin = boundaries[i - 1], end = boundaries[i];
        const auto middle = begin + (end - begin) / 2;
        const auto& keys = envelope.channel.keys;
        const auto right = std::upper_bound(keys.begin(), keys.end(), middle, [](int64_t at, const auto& key) {
            return at < AudioSampleIndex(key.source_us);
        });
        if (right == keys.begin() || right == keys.end())
            continue;
        const auto& left = *std::prev(right);
        if (left.interpolation != KeyInterpolation::Smooth || left.value == right->value)
            continue;
        double slope = 0;
        for (const auto& range : envelope.ranges) {
            if (range.ramp_samples <= 0)
                continue;
            if (middle >= range.in_sample && middle < range.in_sample + range.ramp_samples)
                slope += range.delta_db / range.ramp_samples;
            else if (middle >= range.out_sample - range.ramp_samples && middle < range.out_sample)
                slope -= range.delta_db / range.ramp_samples;
        }
        const auto origin = AudioSampleIndex(left.source_us);
        const auto duration = AudioSampleIndex(right->source_us) - origin;
        const double factor = 6 * (right->value - left.value) / static_cast<double>(duration);
        const auto discriminant = 1 + 4 * slope / factor;
        if (discriminant < 0)
            continue;
        for (const auto u : {(1 - std::sqrt(discriminant)) / 2, (1 + std::sqrt(discriminant)) / 2}) {
            if (u < 0 || u > 1)
                continue;
            const auto sample = origin + static_cast<int64_t>(u * static_cast<double>(duration));
            if (sample >= begin && sample <= end && (!valid(sample) || !valid(sample + 1)))
                return false;
        }
    }
    return true;
}

} // namespace exosnap::engine
