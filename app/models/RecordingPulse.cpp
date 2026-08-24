#include "models/RecordingPulse.h"

#include <algorithm>
#include <cmath>

namespace exosnap {

namespace {

// Frames index an icon array, so a caller's bad value is corrected rather than
// propagated: an out-of-range index reaching a shell call is a wrong icon at
// best and a crash at worst.
[[nodiscard]] int NormalizeFrame(int frame, int frame_count) noexcept {
    if (frame_count <= 0)
        return 0;
    if (frame < 0 || frame >= frame_count)
        return 0;
    return frame;
}

} // namespace

int NextRecordingPulseFrame(int frame, int frame_count) noexcept {
    if (frame_count <= 0)
        return 0;
    const int current = NormalizeFrame(frame, frame_count);
    return (current + 1) % frame_count;
}

double RecordingPulseIntensity(int frame, int frame_count) noexcept {
    if (frame_count <= 1)
        return 0.0;
    const int current = NormalizeFrame(frame, frame_count);
    const int half = frame_count / 2;
    if (half <= 0)
        return 0.0;
    // Distance from the trough, folded at the peak: 0, 1, ... half, ... 1.
    const int distance = current <= half ? current : frame_count - current;
    return static_cast<double>(distance) / static_cast<double>(half);
}

int RecordingPulseLevel(int frame, int levels, int frame_count) noexcept {
    if (levels <= 1)
        return 0;
    const double intensity = RecordingPulseIntensity(frame, frame_count);
    const int top = levels - 1;
    const int level = static_cast<int>(std::lround(intensity * static_cast<double>(top)));
    return std::clamp(level, 0, top);
}

} // namespace exosnap
