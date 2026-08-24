#include <gtest/gtest.h>

#include <exosnap/engine/gpu_timestamp_math.h>

namespace {

using exosnap::engine::GpuTimestampDisjoint;
using exosnap::engine::ResolveGpuSpanMs;

// A clean, non-disjoint 1 GHz clock: 1 tick == 1 ns.
constexpr GpuTimestampDisjoint kGood{/*frequency*/ 1'000'000'000ull, /*disjoint*/ false};

TEST(GpuTimestampMath, ConvertsTicksToMilliseconds) {
    // 3,000,000 ns == 3 ms.
    const auto ms = ResolveGpuSpanMs(1'000, 3'001'000, kGood);
    ASSERT_TRUE(ms.has_value());
    EXPECT_NEAR(*ms, 3.0, 1e-9);
}

TEST(GpuTimestampMath, HonoursNonNanosecondFrequency) {
    // 12 MHz clock: 24000 ticks == 2 ms.
    const GpuTimestampDisjoint clk{12'000'000ull, false};
    const auto ms = ResolveGpuSpanMs(1000, 25000, clk);
    ASSERT_TRUE(ms.has_value());
    EXPECT_NEAR(*ms, 2.0, 1e-9);
}

TEST(GpuTimestampMath, ZeroSpanIsZeroMillisecondsNotDropped) {
    const auto ms = ResolveGpuSpanMs(500, 500, kGood);
    ASSERT_TRUE(ms.has_value());
    EXPECT_DOUBLE_EQ(*ms, 0.0);
}

TEST(GpuTimestampMath, DisjointFrameIsDropped) {
    const GpuTimestampDisjoint dj{1'000'000'000ull, true};
    EXPECT_FALSE(ResolveGpuSpanMs(1000, 5000, dj).has_value());
}

TEST(GpuTimestampMath, UncalibratedZeroFrequencyIsDropped) {
    const GpuTimestampDisjoint dj{0ull, false};
    EXPECT_FALSE(ResolveGpuSpanMs(1000, 5000, dj).has_value());
}

TEST(GpuTimestampMath, EndBeforeBeginIsDropped) {
    // Counter wrap or a mis-ordered/incomplete pair must never yield a huge or
    // negative duration — it is simply dropped.
    EXPECT_FALSE(ResolveGpuSpanMs(9000, 1000, kGood).has_value());
}

} // namespace
