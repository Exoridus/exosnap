#pragma once

#include <algorithm>
#include <cmath>

namespace exosnap::engine {

inline float Bt709ToLinear(float signal) {
    signal = std::clamp(signal, 0.0f, 1.0f);
    return signal < 0.081f ? signal / 4.5f : std::pow((signal + 0.099f) / 1.099f, 1.0f / 0.45f);
}

inline float LinearToBt709(float linear) {
    linear = std::clamp(linear, 0.0f, 1.0f);
    return linear < 0.018f ? linear * 4.5f : 1.099f * std::pow(linear, 0.45f) - 0.099f;
}

inline constexpr char kBt709TransferHlsl[] = R"(
float Bt709ToLinear(float s) {
    s = saturate(s);
    return s < 0.081f ? s / 4.5f : pow((s + 0.099f) / 1.099f, 1.0f / 0.45f);
}
float LinearToBt709(float l) {
    l = saturate(l);
    return l < 0.018f ? l * 4.5f : 1.099f * pow(l, 0.45f) - 0.099f;
}
)";

} // namespace exosnap::engine
