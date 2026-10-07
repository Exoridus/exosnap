#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <span>

namespace exosnap::engine {

inline uint64_t TimelineAudioRenderThrough(uint64_t duration_frames, uint64_t consumed_frames) {
    return std::min<uint64_t>(duration_frames, consumed_frames + 9600);
}

// The consumed cursor includes silence inserted by the endpoint. Late samples
// must be discarded, or an underrun would permanently delay audio against video.
inline void AppendTimelineAudio(std::deque<float>& queue, uint64_t consumed_frames, uint64_t first_frame,
                                std::span<const float> samples, size_t capacity_floats) {
    const uint64_t write_frame = consumed_frames + queue.size() / 2;
    if (first_frame > write_frame) {
        const auto gap =
            static_cast<size_t>(std::min<uint64_t>((first_frame - write_frame) * 2, capacity_floats - queue.size()));
        queue.insert(queue.end(), gap, 0);
    }
    const uint64_t next = consumed_frames + queue.size() / 2;
    if (first_frame > next || next - first_frame >= samples.size() / 2)
        return;
    const auto skip = static_cast<size_t>(next - first_frame) * 2;
    const auto count = std::min<size_t>(samples.size() - skip, capacity_floats - queue.size());
    queue.insert(queue.end(), samples.begin() + static_cast<std::ptrdiff_t>(skip),
                 samples.begin() + static_cast<std::ptrdiff_t>(skip + count));
}

} // namespace exosnap::engine
