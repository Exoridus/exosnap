#pragma once
#include <compare>
#include <cstdint>

namespace exosnap::engine {

// One monotonically-increasing counter per independently-changing visual
// input. Each counter is bumped only when that specific input actually
// changed — a new accepted screen/webcam sample, a cursor position/
// visibility/shape/capture-toggle change, an overlay geometry/opacity/
// chroma-key change, or an HDR/colour-pipeline reconfiguration. Comparing
// the resulting VisualFrameKey tells the pipeline whether the previous
// composited/converted frame is still valid without ever touching pixels.
struct VisualGenerations {
    uint64_t screen = 0;
    uint64_t webcam = 0;
    uint64_t cursor = 0;
    uint64_t overlay = 0;
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

} // namespace exosnap::engine
