#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace recorder_core {

// CFR output pacing. Smooth = phase-correct present-time-nearest selection (default,
// the recording use case). Newest = lowest-latency newest-at-tick (and the WGC fallback).
enum class FramePacingMode : uint8_t {
    Smooth = 0,
    Newest = 1,
};

// Ring size for phase-correct pacing: clamp(ceil(refresh/fps)+2, 4, 12); 8 when refresh
// or fps is unknown (0). Sized for the source-faster-than-output case (e.g. 240->60).
[[nodiscard]] std::size_t ComputePacingRingSize(uint32_t monitor_refresh_hz, uint32_t output_fps);

// Result of SelectFrameForSlot: which ring entry to encode (or duplicate) for a CFR slot.
// NOTE: this header is included from Qt translation units (via recorder_session.h),
// where `emit` is a macro. Guard the field name so the struct stays Qt-safe.
#pragma push_macro("emit")
#undef emit
struct PacingDecision {
    bool emit = false;          // true → encode ring[index]; false → duplicate previous slot
    std::size_t index = 0;      // valid iff emit
    uint32_t newly_dropped = 0; // fresh entries strictly older than the chosen one (skipped)
};
#pragma pop_macro("emit")

// ring_present_qpc: present-time QPC of each LIVE ring entry, ASCENDING capture order.
// slot_qpc: ideal present time of this output slot. last_emitted_present_qpc: present time
// of the last frame already encoded (0 if none). Only entries strictly newer than
// last_emitted are "fresh" (eligible) — guarantees monotonic, non-repeating selection.
[[nodiscard]] PacingDecision SelectFrameForSlot(std::span<const uint64_t> ring_present_qpc, uint64_t slot_qpc,
                                                uint64_t last_emitted_present_qpc, FramePacingMode mode);

} // namespace recorder_core
