#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace exosnap::quick {

// Nearest-rank percentile over an ALREADY SORTED sample vector.
//
// Split out of ExoPreviewItem.cpp so the arithmetic can be asserted directly.
// The caller sorts once per sample buffer and then reads two or three
// percentiles out of it; the previous shape took the vector BY VALUE and sorted
// inside, which metricsSnapshot() paid eight times per call, four times a second,
// over three buffers of up to 1024 doubles.
//
// The ranking is unchanged, including the degenerate `fraction <= 0` case: the
// index computation underflows to SIZE_MAX and std::min clamps it to the last
// element, exactly as before.
[[nodiscard]] inline double PercentileSorted(const std::vector<double>& sorted_values, double fraction) {
    if (sorted_values.empty())
        return 0.0;
    const std::size_t index =
        static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(sorted_values.size()))) - 1;
    return sorted_values[std::min(index, sorted_values.size() - 1)];
}

} // namespace exosnap::quick
