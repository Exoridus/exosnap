#include "StaticWebcamFrameSource.h"

#include <algorithm>

namespace exosnap {
namespace {

// Distinct enough that a half-resolution decode still separates them, and none
// of them is the black a missing overlay leaves behind.
constexpr uint8_t kQuadrantLevels[4] = {40, 110, 180, 245};
constexpr uint8_t kBorderLevel = 15;
constexpr int kBorderPixels = 4;

void SetPixel(std::vector<uint8_t>& bgra, int width, int x, int y, uint8_t level) {
    const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
    bgra[offset] = level;
    bgra[offset + 1] = level;
    bgra[offset + 2] = level;
    bgra[offset + 3] = 255u;
}

} // namespace

std::vector<uint8_t> MakeStaticWebcamPattern(int width, int height) {
    if (width <= 0 || height <= 0) {
        return {};
    }
    std::vector<uint8_t> bgra(static_cast<size_t>(width) * height * 4u, 0u);
    const int mid_x = width / 2;
    const int mid_y = height / 2;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int quadrant = (y < mid_y ? 0 : 2) + (x < mid_x ? 0 : 1);
            SetPixel(bgra, width, x, y, kQuadrantLevels[quadrant]);
        }
    }

    const int border = std::min(kBorderPixels, std::min(width, height) / 2);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (x < border || y < border || x >= width - border || y >= height - border) {
                SetPixel(bgra, width, x, y, kBorderLevel);
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
            SetPixel(bgra, width, x, y, 255u);
        }
    }
    return bgra;
}

StaticWebcamFrameSource::StaticWebcamFrameSource(int width, int height)
    : width_(std::max(0, width)), height_(std::max(0, height)), bgra_(MakeStaticWebcamPattern(width_, height_)) {
}

bool StaticWebcamFrameSource::TryGetFrame(int& out_width, int& out_height, std::vector<uint8_t>& out_bgra,
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
