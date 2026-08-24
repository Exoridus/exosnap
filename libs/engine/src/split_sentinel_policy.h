#pragma once

// Split-sentinel routing policy, factored out of VideoThread so it can be unit
// tested without a live encode session.
//
// Background — why order-preservation alone is not enough once an encoder
// can submit ahead of completion (async NVENC):
//   `split_armed && pkt.keyframe` (the previous, sync-only condition) is
//   correct only because arming (maybeArmSplit) and routing happen in the SAME loop
//   iteration with no packet in between it — the routed packet IS the forced
//   frame. Once EncodeFrame/ReapCompleted can return packets from EARLIER,
//   already-submitted frames in the same iteration (async submit-ahead), that
//   adjacency breaks: a natural GOP-boundary keyframe already in flight when
//   the split arms could satisfy `split_armed && pkt.keyframe` first,
//   absorbing the split onto the wrong (earlier) keyframe — the segment
//   boundary would land a few frames before the actual forced IDR.
//
// The fix binds the sentinel to the SPECIFIC frame whose submission consumed
// the forced-IDR request: maybeArmSplit records the PTS of that frame
// (`split_forced_pts_ns`) when it arms, and only a packet whose own PTS is at
// or after that mark may satisfy the condition — `>=` rather than `==` is
// defensive (never an earlier keyframe; if that exact PTS is somehow never
// seen, the next keyframe after it is accepted rather than the sentinel being
// lost).

#include <cstdint>

namespace exosnap::engine {

// True if `pkt` is the specific forced-IDR frame (or, defensively, the next
// keyframe at or after it) that should carry the split sentinel. Equivalent
// to the previous, sync-only `split_armed && pkt.keyframe` condition in sync
// mode, where the routed packet is always the just-submitted frame
// (pkt_pts_ns == forced_pts_ns).
inline bool ShouldEmitSplitSentinel(bool split_armed, uint64_t split_forced_pts_ns, bool pkt_keyframe,
                                    uint64_t pkt_pts_ns) noexcept {
    return split_armed && pkt_keyframe && pkt_pts_ns >= split_forced_pts_ns;
}

} // namespace exosnap::engine
