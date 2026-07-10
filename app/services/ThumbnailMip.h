#pragma once

// Which level of a mip chain a thumbnail should be read back from.
//
// Reading a 4K desktop back at full resolution and shrinking it on the CPU costs
// tens of milliseconds per tile, which is what kept the picker's tiles at a
// couple of frames per second. Letting the GPU build the mip chain and reading
// back a level that is only just larger than the tile turns that into a copy of
// a few hundred kilobytes.
//
// "Only just larger" matters: the final smooth downscale to the exact tile size
// still runs on the CPU, and it needs more pixels than it emits, or the result
// is a magnified mip.

#include <algorithm>
#include <cstdint>

namespace exosnap {

// The largest level whose image is still at least `desired` in both axes.
// Level 0 when the source is already no bigger than the tile, or when `desired`
// is degenerate. Never returns a level outside the chain.
[[nodiscard]] constexpr uint32_t ChooseMipLevel(uint32_t src_width, uint32_t src_height, uint32_t mip_count,
                                                uint32_t desired_width, uint32_t desired_height) noexcept {
    if (mip_count <= 1 || desired_width == 0 || desired_height == 0)
        return 0;

    uint32_t level = 0;
    for (uint32_t next = 1; next < mip_count; ++next) {
        const uint32_t w = std::max(1u, src_width >> next);
        const uint32_t h = std::max(1u, src_height >> next);
        if (w < desired_width || h < desired_height)
            break;
        level = next;
    }
    return level;
}

[[nodiscard]] constexpr uint32_t MipExtent(uint32_t extent, uint32_t level) noexcept {
    return std::max(1u, extent >> level);
}

} // namespace exosnap
