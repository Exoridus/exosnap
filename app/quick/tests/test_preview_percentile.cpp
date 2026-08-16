// QCR-607. The preview metrics snapshot used to hand `percentile()` a vector BY
// VALUE and let it sort — eight times per snapshot across three buffers, four
// times a second for as long as a preview is up. The sort moved out to the
// caller, which does it once per buffer.
//
// These cases pin the ranking itself: same inputs, same answers as the
// copy-and-sort form, including the degenerate ones. The reference below is the
// OLD implementation verbatim, so a divergence fails here rather than silently
// changing a published percentile.

#include "PreviewPercentile.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using exosnap::quick::PercentileSorted;

namespace {

// The pre-QCR-607 implementation, copied unchanged.
double LegacyPercentile(std::vector<double> values, double fraction) {
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const size_t index = static_cast<size_t>(std::ceil(fraction * static_cast<double>(values.size()))) - 1;
    return values[std::min(index, values.size() - 1)];
}

double NewPercentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    return PercentileSorted(values, fraction);
}

void ExpectSameAsLegacy(const std::vector<double>& values, double fraction) {
    EXPECT_DOUBLE_EQ(NewPercentile(values, fraction), LegacyPercentile(values, fraction))
        << "fraction=" << fraction << " n=" << values.size();
}

} // namespace

TEST(PreviewPercentileTest, EmptySampleSetIsZero) {
    const std::vector<double> empty;
    EXPECT_DOUBLE_EQ(PercentileSorted(empty, 0.95), 0.0);
    ExpectSameAsLegacy(empty, 0.50);
    ExpectSameAsLegacy(empty, 0.95);
    ExpectSameAsLegacy(empty, 0.99);
}

TEST(PreviewPercentileTest, OneSampleIsThatSampleAtEveryFraction) {
    const std::vector<double> one{16.7};
    EXPECT_DOUBLE_EQ(PercentileSorted(one, 0.50), 16.7);
    EXPECT_DOUBLE_EQ(PercentileSorted(one, 0.95), 16.7);
    EXPECT_DOUBLE_EQ(PercentileSorted(one, 0.99), 16.7);
}

TEST(PreviewPercentileTest, NearestRankOverAKnownSet) {
    // 1..10 sorted. Nearest rank: ceil(f*n) - 1, zero-based.
    std::vector<double> values;
    for (int i = 1; i <= 10; ++i)
        values.push_back(static_cast<double>(i));

    EXPECT_DOUBLE_EQ(PercentileSorted(values, 0.50), 5.0);  // ceil(5.0) - 1 = 4
    EXPECT_DOUBLE_EQ(PercentileSorted(values, 0.95), 10.0); // ceil(9.5) - 1 = 9
    EXPECT_DOUBLE_EQ(PercentileSorted(values, 0.99), 10.0); // ceil(9.9) - 1 = 9
}

TEST(PreviewPercentileTest, DuplicatesDoNotShiftTheRank) {
    const std::vector<double> values{4.0, 4.0, 4.0, 4.0, 9.0};
    EXPECT_DOUBLE_EQ(PercentileSorted(values, 0.50), 4.0);
    EXPECT_DOUBLE_EQ(PercentileSorted(values, 0.95), 9.0);
    ExpectSameAsLegacy(values, 0.50);
    ExpectSameAsLegacy(values, 0.95);
    ExpectSameAsLegacy(values, 0.99);
}

TEST(PreviewPercentileTest, ZeroFractionKeepsTheOldUnderflowClamp) {
    // Not reached by any caller, but the index computation underflows there and
    // the clamp is what stopped it being a read past the end. Pinned so a future
    // "cleanup" of the arithmetic cannot quietly reintroduce it.
    const std::vector<double> values{3.0, 1.0, 2.0};
    ExpectSameAsLegacy(values, 0.0);
}

TEST(PreviewPercentileTest, MatchesTheLegacyFormOverAWholeWindow) {
    // The real buffers are up to 1024 samples of frame intervals in milliseconds.
    std::vector<double> values;
    values.reserve(1024);
    for (int i = 0; i < 1024; ++i) {
        // Deterministic, unsorted, with a heavy tail — a plausible frame-interval
        // distribution rather than a ramp.
        const double jitter = static_cast<double>((i * 37) % 101) / 10.0;
        values.push_back(16.6 + jitter + (i % 97 == 0 ? 22.0 : 0.0));
    }
    for (const double fraction : {0.50, 0.90, 0.95, 0.99, 1.0})
        ExpectSameAsLegacy(values, fraction);
}

TEST(PreviewPercentileTest, TheMaximumIsTheLastElementAfterSorting) {
    // metricsSnapshot() replaced std::max_element with back() on the sorted
    // buffer; this is the property that makes those the same value.
    std::vector<double> values{5.0, 41.0, 2.0, 17.0};
    const double expected = *std::max_element(values.begin(), values.end());
    std::sort(values.begin(), values.end());
    EXPECT_DOUBLE_EQ(values.back(), expected);
}
