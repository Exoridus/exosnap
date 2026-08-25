#include "models/RecordingPulse.h"

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

} // namespace exosnap
