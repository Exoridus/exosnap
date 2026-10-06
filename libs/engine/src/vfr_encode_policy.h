#pragma once

#include <algorithm>
#include <cstdint>

namespace exosnap::engine {

inline bool NeedsVfrEncoderHeartbeat(uint64_t pending_frames, bool cached_picture, uint64_t now_ns,
                                     uint64_t last_submission_ns) noexcept {
    return pending_frames > 0 && cached_picture && now_ns >= last_submission_ns &&
           now_ns - last_submission_ns >= 1000000000ULL;
}

inline uint64_t VfrPicturePts(uint64_t capture_pts_ns, uint64_t last_picture_pts_ns,
                              uint64_t mux_progress_floor_ns) noexcept {
    return (std::max)({capture_pts_ns, last_picture_pts_ns + 1, mux_progress_floor_ns});
}

} // namespace exosnap::engine
