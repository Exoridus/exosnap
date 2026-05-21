#include "recorder_core/audio_meter.h"

#include <algorithm>
#include <cmath>

namespace recorder_core {

float ComputeRmsLinear(const float* samples, std::size_t count) {
    if (samples == nullptr || count == 0) {
        return 0.0f;
    }

    double sum_sq = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double s = static_cast<double>(samples[i]);
        sum_sq += s * s;
    }

    return static_cast<float>(std::sqrt(sum_sq / static_cast<double>(count)));
}

float ComputeRmsFromInt16(const std::int16_t* samples, std::size_t count) {
    if (samples == nullptr || count == 0) {
        return 0.0f;
    }

    double sum_sq = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double s = static_cast<double>(samples[i]) / 32768.0;
        sum_sq += s * s;
    }

    return static_cast<float>(std::sqrt(sum_sq / static_cast<double>(count)));
}

float RmsToDbfs(float rms_linear) {
    if (rms_linear <= 0.0f) {
        return kDbFloor;
    }

    return std::max(kDbFloor, 20.0f * std::log10(rms_linear));
}

float RmsToMeter01(float rms_linear) {
    const float db = RmsToDbfs(rms_linear);
    return std::clamp((db - kDbFloor) / -kDbFloor, 0.0f, 1.0f);
}

} // namespace recorder_core
