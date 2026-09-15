#pragma once

// A webcam frame provider that never changes, for measuring what the PiP overlay
// does when nothing else moves.
//
// The overlay's geometry, opacity and chroma key are settable while a recording
// runs, and a change has to advance the overlay generation or a still capture
// source keeps re-emitting the composite made with the previous overlay. Proving
// that needs a recording in which the overlay is the only thing that can have
// caused a recomposition -- and a real camera is the opposite of that: every
// delivered sample advances the webcam generation, which recomposites the frame
// on its own and hides a missing overlay generation completely.
//
// Hence both halves of the contract below. Identical pixels are not enough; the
// generation has to stand still too.
//
// This is a measurement aid in the same class as --auto-record and --visual-test:
// selected by argv, never the default, and it replaces only the camera. The
// overlay, compositor and encoder paths under test are the shipping ones.

#include <exosnap/engine/recorder_session.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace exosnap {

// The pattern the provider serves. Four quadrants of distinct brightness, a
// border, and one small asymmetric mark, so an analyzer can tell a moved overlay
// from a scaled one, from an absent one, and from a change in the screen source
// behind it. Deliberately not a flat colour, and deliberately not animated: a
// time code would be a visual generation of its own.
[[nodiscard]] inline std::vector<uint8_t> MakeStaticWebcamPattern(int width, int height);

class StaticWebcamFrameSource final : public exosnap::engine::WebcamFrameProvider {
  public:
    inline StaticWebcamFrameSource(int width, int height);

    // Contract, and the whole point of the class:
    //   * the first call and every later one return the same pixels,
    //   * out_generation is the same value on every call,
    //   * it never returns false.
    // A provider that bumps the generation per call, or serves different pixels
    // under one generation, breaks the measurement it exists to enable rather
    // than merely being untidy.
    inline bool TryGetFrame(int& out_width, int& out_height, std::vector<uint8_t>& out_bgra,
                            uint64_t& out_generation) override;

    // For the evidence record: which source produced the pixels, and at which
    // generation they were frozen.
    [[nodiscard]] uint64_t Generation() const noexcept {
        return generation_;
    }
    [[nodiscard]] int Width() const noexcept {
        return width_;
    }
    [[nodiscard]] int Height() const noexcept {
        return height_;
    }

  private:
    int width_ = 0;
    int height_ = 0;
    uint64_t generation_ = 1;
    std::vector<uint8_t> bgra_;
};

// Header-only: every target that links RecordingCoordinator composites through
// this seam, and a separate translation unit would have to be added to each of
// their source lists to link.
namespace static_webcam_detail {

// Distinct enough that a half-resolution decode still separates them, and none
// of them is the black a missing overlay leaves behind.
inline constexpr uint8_t kQuadrantLevels[4] = {40, 110, 180, 245};
inline constexpr uint8_t kBorderLevel = 15;
inline constexpr int kBorderPixels = 4;

inline void SetPixel(std::vector<uint8_t>& bgra, int width, int x, int y, uint8_t level) {
    const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
    bgra[offset] = level;
    bgra[offset + 1] = level;
    bgra[offset + 2] = level;
    bgra[offset + 3] = 255u;
}

} // namespace static_webcam_detail

inline std::vector<uint8_t> MakeStaticWebcamPattern(int width, int height) {
    if (width <= 0 || height <= 0) {
        return {};
    }
    std::vector<uint8_t> bgra(static_cast<size_t>(width) * height * 4u, 0u);
    const int mid_x = width / 2;
    const int mid_y = height / 2;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int quadrant = (y < mid_y ? 0 : 2) + (x < mid_x ? 0 : 1);
            static_webcam_detail::SetPixel(bgra, width, x, y, static_webcam_detail::kQuadrantLevels[quadrant]);
        }
    }

    const int border = std::min(static_webcam_detail::kBorderPixels, std::min(width, height) / 2);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (x < border || y < border || x >= width - border || y >= height - border) {
                static_webcam_detail::SetPixel(bgra, width, x, y, static_webcam_detail::kBorderLevel);
            }
        }
    }

    // One mark, in a single quadrant, so a 180-degree rotation or a mirror is not
    // mistaken for the unaltered pattern.
    const int mark_w = std::max(1, width / 8);
    const int mark_h = std::max(1, height / 8);
    const int mark_x = std::min(width - mark_w, border + width / 16);
    const int mark_y = std::min(height - mark_h, border + height / 16);
    for (int y = mark_y; y < mark_y + mark_h; ++y) {
        for (int x = mark_x; x < mark_x + mark_w; ++x) {
            static_webcam_detail::SetPixel(bgra, width, x, y, 255u);
        }
    }
    return bgra;
}

inline StaticWebcamFrameSource::StaticWebcamFrameSource(int width, int height)
    : width_(std::max(0, width)), height_(std::max(0, height)), bgra_(MakeStaticWebcamPattern(width_, height_)) {
}

inline bool StaticWebcamFrameSource::TryGetFrame(int& out_width, int& out_height, std::vector<uint8_t>& out_bgra,
                                                 uint64_t& out_generation) {
    if (bgra_.empty()) {
        return false;
    }
    out_width = width_;
    out_height = height_;
    out_bgra = bgra_;
    // Not incremented. A generation that advances per call recomposites the frame
    // on the webcam's behalf, which is exactly what would mask a missing overlay
    // generation -- the defect this source exists to expose.
    out_generation = generation_;
    return true;
}

} // namespace exosnap
