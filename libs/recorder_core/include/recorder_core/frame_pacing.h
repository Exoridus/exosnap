#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

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

// Sustained-encoder-lag resync for the CFR scheduler.
//
// The CFR scheduler emits at most `max_catch_up_frames` frames per outer iteration so a
// brief stall (e.g. process suspension) cannot trigger a burst. If the encoder is
// *persistently* slower than real time, that cap is hit every iteration and the media
// clock (frame_index x frame_interval) trails the wall clock ever further: the file
// ends with less video than audio and drifts out of sync with nothing reported. This
// returns how many frame indices the scheduler must SKIP forward (each counted as a
// real drop) so the next emitted frame's media time realigns with the wall clock. A
// one-catch-up-budget cushion (`max_catch_up_frames`, i.e. one second) is retained so
// the ordinary catch-up loop resumes smoothly instead of snapping exactly to "now".
//
// lag_100ns: how far media time currently trails the wall clock (elapsed - next_tick),
//   in 100 ns units; 0 when on time. Returns 0 while the lag is within one catch-up
//   budget (the ordinary loop absorbs it) or when frame_interval_100ns is 0.
[[nodiscard]] uint64_t ComputeCatchUpSkip(uint64_t lag_100ns, uint64_t frame_interval_100ns,
                                          uint64_t max_catch_up_frames) noexcept;

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

// ---------------------------------------------------------------------------
// Held-screen re-composition
//
// A screen capture only produces a frame when the screen changes: DXGI Output
// Duplication yields nothing on a still desktop, and WGC only delivers on
// repaint. The webcam, cursor, or overlay-settings state, however, keeps
// moving or changing. Duplicating the last *composited* frame therefore
// freezes that dynamic overlay content inside the recording whenever the
// desktop is still — which is exactly when it is the only thing worth
// watching.
//
// When the screen produced no fresh frame but the webcam, cursor or
// overlay-settings state has changed, the held screen is composited again
// with the current webcam image and encoded as a real frame. The result is a
// picture-in-picture that moves at the encode cadence rather than at the
// desktop's change rate.
//
// Two conditions forbid it. Without a held screen there is nothing to composite
// onto. And while the OD source is holding (mid-reopen after a display loss) the
// capture's display-tied GPU resources are gone, so the frame must stay frozen
// until Reopen() succeeds.
// ---------------------------------------------------------------------------
[[nodiscard]] constexpr bool ShouldRecompositeHeldScreen(bool has_fresh_source, bool od_holding,
                                                         bool dynamic_overlay_changed, bool has_held_screen) noexcept {
    if (has_fresh_source)
        return false; // a real frame is available; composite that instead
    if (od_holding || !has_held_screen)
        return false;
    return dynamic_overlay_changed;
}

// ---------------------------------------------------------------------------
// Why a scheduled CFR tick emitted no frame
//
// A CFR tick can end without a frame for two fundamentally different reasons,
// and conflating them makes the drop counters lie: one is ordinary pacing, the
// other is a genuine processing failure that costs the user picture.
//
// Pacing: the capture backend produced nothing for this tick and there is no
// reference frame to hold. That is the session's own start — the encode loop has
// not composited a first frame yet — and it resolves itself as soon as one
// arrives. Benign.
//
// Processing failure: either a source frame WAS available for this tick and its
// GPU conversion (input-view creation / VideoProcessorBlt) failed, or the
// reference texture could not be allocated at all, in which case every held tick
// on a still source drops for the rest of the session. Both lose real picture and
// must never be filed under benign pacing.
// ---------------------------------------------------------------------------
enum class CfrTickDropCause : uint8_t {
    Pacing = 0,           // benign: nothing to encode yet
    ProcessingFailure = 1 // real: a frame was there (or should have been held) and was lost
};

// had_source_frame: a source texture was selected for this tick (fresh capture or
//   held screen re-composition), so the drop can only come from the conversion path.
// reference_storage_available: the reference texture used to hold the last composited
//   frame exists. False only when its allocation failed at session start, which turns
//   every no-fresh-frame tick into a lost frame rather than a startup gap.
[[nodiscard]] constexpr CfrTickDropCause ClassifyCfrTickDrop(bool had_source_frame,
                                                             bool reference_storage_available) noexcept {
    if (had_source_frame)
        return CfrTickDropCause::ProcessingFailure;
    return reference_storage_available ? CfrTickDropCause::Pacing : CfrTickDropCause::ProcessingFailure;
}

// ---------------------------------------------------------------------------
// Held-screen ownership rotation for the phase-correct capture ring
//
// The held screen is the desktop texture a CFR tick re-composites when it has no
// fresh capture but the cursor or webcam moved (see ShouldRecompositeHeldScreen).
// It must be the LAST EMITTED capture. While phase-correct pacing is on, the ring
// owns the drain and nothing else writes the held slot, so a held texture that is
// merely "whatever was written once" stays the session's first frame and every
// such tick encodes that frame instead of the current screen.
//
// The exchange is an ownership rotation, not a copy: the emitted entry has
// already been consumed (its present timestamp cleared) before this is called, so
// handing the previous held texture back in its place gives the drain a free slot
// of identical description, while the encode keeps reading the very texture it
// was handed.
//
// Two invariants a future ring rework must preserve, and that the tests pin:
//   - after the rotation the held texture is the emitted one, and the ring entry
//     is the previously held one;
//   - the held texture is never aliased by a live ring entry, or the drain would
//     overwrite the screen a later tick still intends to re-composite.
// ---------------------------------------------------------------------------
template <typename TexturePtr>
void AdoptEmittedAsHeldScreen(TexturePtr& emitted_ring_entry, TexturePtr& held_screen) noexcept {
    using std::swap;
    swap(emitted_ring_entry, held_screen);
}

} // namespace recorder_core
