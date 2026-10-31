#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <vector>

namespace exosnap::engine {

enum class KeyInterpolation { Linear, Smooth, Hold };

struct PropertyKey {
    int64_t source_us = 0;
    double value = 0;
    KeyInterpolation interpolation = KeyInterpolation::Linear;
    bool operator==(const PropertyKey&) const = default;
};

struct PropertyChannel {
    std::vector<PropertyKey> keys;
    bool operator==(const PropertyChannel&) const = default;
};

struct VideoTransform {
    double x = 0;
    double y = 0;
    double scale = 1;
    double rotation = 0;
    bool operator==(const VideoTransform&) const = default;
};

using TransformChannels = std::array<PropertyChannel, 4>;

inline bool ValidChannel(const PropertyChannel& channel, double minimum, double maximum);

inline bool ValidTransform(const VideoTransform& value, const TransformChannels& channels) {
    const std::array<double, 4> values{value.x, value.y, value.scale, value.rotation};
    constexpr std::array<double, 4> minimum{-1024, -1024, 0.001, -360000};
    constexpr std::array<double, 4> maximum{1024, 1024, 1024, 360000};
    for (size_t i = 0; i < values.size(); ++i)
        if (!std::isfinite(values[i]) || values[i] < minimum[i] || values[i] > maximum[i] ||
            !ValidChannel(channels[i], minimum[i], maximum[i]))
            return false;
    return true;
}

inline bool ValidChannel(const PropertyChannel& channel, double minimum, double maximum) {
    int64_t previous = -1;
    for (const auto& key : channel.keys) {
        if (key.source_us <= previous || !std::isfinite(key.value) || key.value < minimum || key.value > maximum ||
            (key.interpolation != KeyInterpolation::Linear && key.interpolation != KeyInterpolation::Smooth &&
             key.interpolation != KeyInterpolation::Hold))
            return false;
        previous = key.source_us;
    }
    return true;
}

inline double EvaluateChannel(const PropertyChannel& channel, double constant, int64_t source_us) {
    if (channel.keys.empty())
        return constant;
    const auto right = std::upper_bound(channel.keys.begin(), channel.keys.end(), source_us,
                                        [](int64_t time, const auto& key) { return time < key.source_us; });
    if (right == channel.keys.begin())
        return right->value;
    const auto& left = *std::prev(right);
    if (right == channel.keys.end() || left.interpolation == KeyInterpolation::Hold)
        return left.value;
    double t = static_cast<double>(source_us - left.source_us) / static_cast<double>(right->source_us - left.source_us);
    if (left.interpolation == KeyInterpolation::Smooth)
        t = t * t * (3 - 2 * t);
    return left.value + (right->value - left.value) * t;
}

inline void PutKey(PropertyChannel& channel, int64_t source_us, double value,
                   KeyInterpolation interpolation = KeyInterpolation::Linear) {
    const auto key = std::lower_bound(channel.keys.begin(), channel.keys.end(), source_us,
                                      [](const auto& item, int64_t time) { return item.source_us < time; });
    if (key != channel.keys.end() && key->source_us == source_us)
        key->value = value;
    else
        channel.keys.insert(key, {source_us, value, interpolation});
}

// Mutate a candidate only: the caller validates every affected channel before publishing.
inline void EditChannel(PropertyChannel& channel, double& constant, int64_t source_in_us, int64_t source_us,
                        double value, bool auto_key, bool multiply = false,
                        KeyInterpolation interpolation = KeyInterpolation::Linear) {
    const auto current = EvaluateChannel(channel, constant, source_us);
    if (value == current)
        return;
    if (auto_key) {
        if (channel.keys.empty() && source_us > source_in_us)
            PutKey(channel, source_in_us, constant, interpolation);
        PutKey(channel, source_us, value, interpolation);
    } else if (channel.keys.empty()) {
        constant = value;
    } else {
        const auto offset = value - current;
        for (auto& key : channel.keys)
            key.value = multiply ? key.value * (value / current) : key.value + offset;
    }
}

} // namespace exosnap::engine
