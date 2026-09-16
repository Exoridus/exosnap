#pragma once
#include <compare>
#include <cstdint>

namespace exosnap::engine {

// One monotonically-increasing counter per independently-changing visual
// input. Each counter is bumped only when that specific input actually
// changed: a new accepted screen/webcam sample, a cursor position/
// visibility/shape/capture-toggle change, or an overlay geometry/opacity/
// chroma-key change. Comparing the resulting VisualFrameKey tells the
// pipeline whether the previous composited/converted frame is still valid
// without ever touching pixels.
//
// Every capture backend must advance the counters for the inputs it owns. A
// backend that leaves one standing makes two different frames compare equal,
// which is indistinguishable from "nothing changed" and silently re-uses the
// cached composite or encoder slot.
struct VisualGenerations {
    uint64_t screen = 0;
    uint64_t webcam = 0;
    uint64_t cursor = 0;
    uint64_t overlay = 0;
    // Reserved. The colour pipeline (SDR / tone-map / native HDR10) is
    // negotiated once from the first captured frame and cannot change while a
    // session runs -- a display whose HDR is toggled mid-session fails the
    // recording rather than reconfiguring -- so nothing advances this today.
    uint64_t color_pipeline = 0;
};

// Snapshot of VisualGenerations at the moment a frame was composited/
// converted. Two keys compare equal iff every input was unchanged, which is
// the sole condition under which a cached composited/converted/encoded
// result may be reused instead of redone.
struct VisualFrameKey {
    uint64_t screen_generation = 0;
    uint64_t webcam_generation = 0;
    uint64_t cursor_generation = 0;
    uint64_t overlay_generation = 0;
    uint64_t color_pipeline_generation = 0;

    auto operator<=>(const VisualFrameKey&) const = default;
};

[[nodiscard]] constexpr VisualFrameKey MakeVisualFrameKey(const VisualGenerations& gens) noexcept {
    return VisualFrameKey{gens.screen, gens.webcam, gens.cursor, gens.overlay, gens.color_pipeline};
}

// Whether one input's generation advanced since the last composited frame.
//
// Separate from the recomposition decision on purpose, and not the same
// predicate. Recompositing must also happen when there is no previous key at
// all -- the first composite of a session has nothing to reuse -- but that is
// initialisation, not a change, and counting it makes a measurement of "did
// this input move" read one on a session where it never did. A verification
// source that serves one frame forever is exactly such a session, and it is the
// baseline an overlay measurement is attributed against.
[[nodiscard]] constexpr bool GenerationAdvanced(bool have_last_composited_key, uint64_t current,
                                                uint64_t last_composited) noexcept {
    return have_last_composited_key && current != last_composited;
}

} // namespace exosnap::engine
