#include <gtest/gtest.h>

#include "perf_histogram.h"

#include <array>

namespace {

using exosnap::engine::LatencyHistogram;

TEST(LatencyHistogram, EmptyIsZero) {
    LatencyHistogram h;
    EXPECT_EQ(h.count(), 0u);
    EXPECT_DOUBLE_EQ(h.Quantile(0.5), 0.0);
    EXPECT_DOUBLE_EQ(h.Quantile(0.99), 0.0);
}

TEST(LatencyHistogram, BucketEdgesAreStrictlyMonotonic) {
    double prev = -1.0;
    for (std::size_t b = 1; b < LatencyHistogram::kGeoBuckets; ++b) {
        const double lo = LatencyHistogram::BucketLowEdge(b);
        EXPECT_GT(lo, prev) << "bucket " << b;
        prev = lo;
    }
    // The top geometric edge lands exactly on kHiMs.
    EXPECT_NEAR(LatencyHistogram::BucketHighEdge(LatencyHistogram::kGeoBuckets - 1), LatencyHistogram::kHiMs,
                LatencyHistogram::kHiMs * 1e-9);
}

TEST(LatencyHistogram, BucketIndexBoundaries) {
    // Below the low edge clamps into bucket 0; at/above the high edge is overflow.
    EXPECT_EQ(LatencyHistogram::BucketIndex(0.001), 0u);
    EXPECT_EQ(LatencyHistogram::BucketIndex(0.0), 0u);
    EXPECT_EQ(LatencyHistogram::BucketIndex(-5.0), 0u);
    EXPECT_EQ(LatencyHistogram::BucketIndex(1000.0), LatencyHistogram::kGeoBuckets);
    EXPECT_EQ(LatencyHistogram::BucketIndex(LatencyHistogram::kHiMs), LatencyHistogram::kGeoBuckets);
    // An in-range sample lands in a geometric bucket whose edges bracket it.
    const std::size_t b = LatencyHistogram::BucketIndex(10.0);
    EXPECT_GE(10.0, LatencyHistogram::BucketLowEdge(b));
    EXPECT_LT(10.0, LatencyHistogram::BucketHighEdge(b));
}

TEST(LatencyHistogram, SingleValueQuantileWithinBucket) {
    LatencyHistogram h;
    for (int i = 0; i < 100; ++i) {
        h.Add(10.0);
    }
    EXPECT_EQ(h.count(), 100u);
    const std::size_t b = LatencyHistogram::BucketIndex(10.0);
    const double lo = LatencyHistogram::BucketLowEdge(b);
    const double hi = LatencyHistogram::BucketHighEdge(b);
    for (double q : {0.0, 0.5, 0.9, 0.99, 1.0}) {
        const double v = h.Quantile(q);
        EXPECT_GE(v, lo) << "q=" << q;
        EXPECT_LE(v, hi) << "q=" << q;
    }
}

TEST(LatencyHistogram, OverflowBucketReportsCeiling) {
    LatencyHistogram h;
    h.Add(2000.0); // way past kHiMs
    EXPECT_EQ(h.BucketCounts()[LatencyHistogram::kGeoBuckets], 1u);
    EXPECT_DOUBLE_EQ(h.Quantile(0.99), LatencyHistogram::kHiMs);
}

TEST(LatencyHistogram, KnownDistributionMedian) {
    // Uniform 1..200 ms; the median (~100 ms) must land in the bucket containing 100.
    LatencyHistogram h;
    for (int ms = 1; ms <= 200; ++ms) {
        h.Add(static_cast<double>(ms));
    }
    const double p50 = h.Quantile(0.5);
    const std::size_t b100 = LatencyHistogram::BucketIndex(100.0);
    // Allow the median to fall within one bucket either side of the 100 ms bucket
    // (bucket-boundary rounding), i.e. bracket it generously by value.
    EXPECT_GT(p50, 80.0);
    EXPECT_LT(p50, 125.0);
    const double p99 = h.Quantile(0.99);
    EXPECT_GT(p99, p50);
    EXPECT_LT(p99, 260.0);
    (void)b100;
}

TEST(LatencyHistogram, MergeCombinesCountsAndTotal) {
    LatencyHistogram a;
    LatencyHistogram b;
    for (int i = 0; i < 40; ++i) {
        a.Add(5.0);
    }
    for (int i = 0; i < 60; ++i) {
        b.Add(50.0);
    }
    a.Merge(b);
    EXPECT_EQ(a.count(), 100u);
    EXPECT_EQ(a.BucketCounts()[LatencyHistogram::BucketIndex(5.0)], 40u);
    EXPECT_EQ(a.BucketCounts()[LatencyHistogram::BucketIndex(50.0)], 60u);
    // p50 falls at the 5.0 population (first 40) boundary into the 50.0 population;
    // either way it is bracketed by the two occupied buckets.
    const double p50 = a.Quantile(0.5);
    EXPECT_GE(p50, LatencyHistogram::BucketLowEdge(LatencyHistogram::BucketIndex(5.0)));
    EXPECT_LE(p50, LatencyHistogram::BucketHighEdge(LatencyHistogram::BucketIndex(50.0)));
}

TEST(LatencyHistogram, ClearResets) {
    LatencyHistogram h;
    for (int i = 0; i < 10; ++i) {
        h.Add(12.0);
    }
    h.Clear();
    EXPECT_EQ(h.count(), 0u);
    for (auto c : h.BucketCounts()) {
        EXPECT_EQ(c, 0u);
    }
    EXPECT_DOUBLE_EQ(h.Quantile(0.5), 0.0);
}

} // namespace
