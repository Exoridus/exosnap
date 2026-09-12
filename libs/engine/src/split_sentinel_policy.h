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
#include <iterator>
#include <string>

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

// ---------------------------------------------------------------------------
// Binding a split request's sequence number to the trigger that caused it.
//
// The two lived in separate atomics and were read in separate loads:
//
//     const uint64_t seq = split_request_seq.load();     // (A)
//     ...
//     trigger = split_last_trigger.load();               // (B)
//
// A size-driven request landing between (A) and (B) overwrites the trigger, so
// the consumer attributes sequence N to the trigger of sequence N+1: a manual
// split reported as an automatic size split. Nothing is corrupted -- the split
// happens correctly either way -- but the reason recorded for it is wrong, and
// the reason is the whole point of recording it.
//
// So the request IS one value. Sequence and trigger are packed into one 64-bit
// word and read in one load, which makes the mismatch unrepresentable rather
// than unlikely.
//
// Coalescing is preserved and made explicit. Several requests before a boundary
// collapse into one split, which is correct; what was lost is that they
// happened. The word also carries a bitmask of every trigger seen since the
// last consume, so a boundary can report the request that caused it AND what
// else coalesced into it, instead of one arbitrary winner.
//
// Layout, low to high: 8 bits primary trigger, 8 bits trigger mask, 48 bits
// sequence. 48 bits is 2.8e14 splits -- it cannot wrap in a recording.
// ---------------------------------------------------------------------------

inline constexpr uint64_t kSplitTriggerBits = 8;
inline constexpr uint64_t kSplitMaskBits = 8;
inline constexpr uint64_t kSplitTriggerMask = (1ULL << kSplitTriggerBits) - 1ULL;
inline constexpr uint64_t kSplitCoalesceMask = (1ULL << kSplitMaskBits) - 1ULL;

struct SplitRequestState {
    uint64_t sequence = 0;
    // The trigger of the most recent request -- the one that caused this split.
    uint32_t primary_trigger = 0;
    // One bit per SplitTriggerSource seen since the last consume, primary
    // included. More than one bit set means requests coalesced.
    uint32_t coalesced_triggers = 0;
};

[[nodiscard]] constexpr uint64_t PackSplitRequest(const SplitRequestState& state) noexcept {
    return (state.sequence << (kSplitTriggerBits + kSplitMaskBits)) |
           ((static_cast<uint64_t>(state.coalesced_triggers) & kSplitCoalesceMask) << kSplitTriggerBits) |
           (static_cast<uint64_t>(state.primary_trigger) & kSplitTriggerMask);
}

[[nodiscard]] constexpr SplitRequestState UnpackSplitRequest(uint64_t packed) noexcept {
    SplitRequestState state;
    state.primary_trigger = static_cast<uint32_t>(packed & kSplitTriggerMask);
    state.coalesced_triggers = static_cast<uint32_t>((packed >> kSplitTriggerBits) & kSplitCoalesceMask);
    state.sequence = packed >> (kSplitTriggerBits + kSplitMaskBits);
    return state;
}

// The next value of the word when a request with `trigger` arrives: the sequence
// advances, the trigger becomes primary, and its bit joins the coalesce mask.
// Applied with a compare-exchange so two requesters cannot lose each other's
// bit -- a plain store would drop whichever read first.
[[nodiscard]] constexpr uint64_t SplitRequestWith(uint64_t packed, uint32_t trigger) noexcept {
    SplitRequestState state = UnpackSplitRequest(packed);
    state.sequence += 1;
    state.primary_trigger = trigger & static_cast<uint32_t>(kSplitTriggerMask);
    state.coalesced_triggers |= (1u << (trigger & 0x7u));
    return PackSplitRequest(state);
}

// The word after a consumer has taken everything up to `state.sequence`: the
// sequence and primary trigger stay (they identify what was consumed), the
// coalesce mask is cleared so the next boundary reports its own requests.
[[nodiscard]] constexpr uint64_t SplitRequestConsumed(const SplitRequestState& state) noexcept {
    SplitRequestState next = state;
    next.coalesced_triggers = 0;
    return PackSplitRequest(next);
}

// The coalesce mask as text for a log line: every trigger that fed this
// boundary, in a stable order, so two entries are comparable.
[[nodiscard]] inline std::string SplitTriggerMaskText(uint32_t mask) noexcept {
    // Index is the SplitTriggerSource enumerator value (recorder_session.h).
    static constexpr const char* kNames[] = {"automatic-duration", "automatic-size", "manual-button", "hotkey"};
    std::string out;
    for (uint32_t bit = 0; bit < std::size(kNames); ++bit) {
        if ((mask & (1u << bit)) == 0u)
            continue;
        if (!out.empty())
            out += "+";
        out += kNames[bit];
    }
    return out.empty() ? std::string{"none"} : out;
}

} // namespace exosnap::engine
