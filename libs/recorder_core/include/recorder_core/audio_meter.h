#pragma once

#include <cstddef>
#include <cstdint>

namespace recorder_core {

inline constexpr float kDbFloor = -60.0f;

[[nodiscard]] float ComputeRmsLinear(const float* samples, std::size_t count);
[[nodiscard]] float ComputeRmsFromInt16(const std::int16_t* samples, std::size_t count);
[[nodiscard]] float RmsToDbfs(float rms_linear);
[[nodiscard]] float RmsToMeter01(float rms_linear);

} // namespace recorder_core
