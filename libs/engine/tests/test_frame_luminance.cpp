// Pure evaluation logic for per-frame luminance measurements (frame_luminance.h):
// histogram binning and percentiles, the stream-wide MaxCLL/MaxFALL accumulator,
// the asymmetric content-peak smoother, and the knee invariant the smoothed peak
// has to keep. The GPU pass that produces the raw numbers is pinned separately in
// test_gpu_frame_luminance.cpp.

#include <gtest/gtest.h>

#include "frame_luminance.h"

#include <array>
#include <cstdint>
#include <limits>

namespace {

using exosnap::engine::ContentLightLevelCode;
using exosnap::engine::ContentPeakScale;
using exosnap::engine::ContentPeakSmoother;
using exosnap::engine::FrameLuminanceHistogramBin;
using exosnap::engine::FrameLuminanceHistogramBinCentreNits;
using exosnap::engine::FrameLuminanceStats;
using exosnap::engine::HistogramPercentileNits;
using exosnap::engine::kContentPeakFallTimeConstantSeconds;
using exosnap::engine::kContentPeakPercentile;
using exosnap::engine::kContentPeakRiseTimeConstantSeconds;
using exosnap::engine::kFrameLuminanceHistogramBins;
using exosnap::engine::kHdrToneMapKnee;
using exosnap::engine::SdrPaperWhiteScale;
using exosnap::engine::StreamLuminanceAccumulator;

using Histogram = std::array<uint32_t, kFrameLuminanceHistogramBins>;

// --- binning ----------------------------------------------------------------

TEST(FrameLuminanceTest, BinIsMonotonicInLuminance) {
    int previous = FrameLuminanceHistogramBin(0.0f);
    for (float nits = 0.01f; nits < 12000.0f; nits *= 1.07f) {
        const int bin = FrameLuminanceHistogramBin(nits);
        EXPECT_GE(bin, previous) << "nits=" << nits;
        EXPECT_GE(bin, 0);
        EXPECT_LT(bin, kFrameLuminanceHistogramBins);
        previous = bin;
    }
}

TEST(FrameLuminanceTest, BlackAndNegativesLandInTheFirstBin) {
    EXPECT_EQ(FrameLuminanceHistogramBin(0.0f), 0);
    EXPECT_EQ(FrameLuminanceHistogramBin(-4.0f), 0);
    EXPECT_EQ(FrameLuminanceHistogramBin(1e-9f), 0);
}

TEST(FrameLuminanceTest, AboveTheDomainSaturatesInTheLastBin) {
    EXPECT_EQ(FrameLuminanceHistogramBin(1.0e6f), kFrameLuminanceHistogramBins - 1);
}

TEST(FrameLuminanceTest, BinCentreRoundTripsToItsOwnBin) {
    for (int bin = 0; bin < kFrameLuminanceHistogramBins; ++bin) {
        EXPECT_EQ(FrameLuminanceHistogramBin(FrameLuminanceHistogramBinCentreNits(bin)), bin) << "bin=" << bin;
    }
}

// The domain has to cover the levels this product actually meets: an SDR desktop
// at its dimmest composed white, and the brightest HDR10 mastering level.
TEST(FrameLuminanceTest, DomainSeparatesTheLevelsTheProductMeets) {
    const int at_80 = FrameLuminanceHistogramBin(80.0f);
    const int at_203 = FrameLuminanceHistogramBin(203.0f);
    const int at_480 = FrameLuminanceHistogramBin(480.0f);
    const int at_1000 = FrameLuminanceHistogramBin(1000.0f);
    const int at_10000 = FrameLuminanceHistogramBin(10000.0f);
    EXPECT_LT(at_80, at_203);
    EXPECT_LT(at_203, at_480);
    EXPECT_LT(at_480, at_1000);
    EXPECT_LT(at_1000, at_10000);
    EXPECT_LT(at_10000, kFrameLuminanceHistogramBins - 1) << "10000 cd/m^2 must not be in the saturating bin";
}

// --- percentile -------------------------------------------------------------

TEST(FrameLuminanceTest, PercentileOfAnEmptyHistogramIsZero) {
    const Histogram bins{};
    EXPECT_EQ(HistogramPercentileNits(bins, 0.99f), 0.0f);
}

TEST(FrameLuminanceTest, PercentileOfASingleBinIsThatBinsCentre) {
    Histogram bins{};
    const int bin = FrameLuminanceHistogramBin(200.0f);
    bins[static_cast<size_t>(bin)] = 4096;
    for (const float p : {0.0f, 0.5f, 0.9999f, 1.0f}) {
        EXPECT_FLOAT_EQ(HistogramPercentileNits(bins, p), FrameLuminanceHistogramBinCentreNits(bin)) << "p=" << p;
    }
}

// The reason the knee is taken at a rank rather than at the maximum: a handful
// of outlier pixels must not define the roll-off for the whole picture.
TEST(FrameLuminanceTest, PercentileRejectsASparseOutlierButNotAGenuineHighlight) {
    Histogram bins{};
    const int body = FrameLuminanceHistogramBin(200.0f);
    const int highlight = FrameLuminanceHistogramBin(1200.0f);
    const int outlier = kFrameLuminanceHistogramBins - 1;
    bins[static_cast<size_t>(body)] = 1000000;
    bins[static_cast<size_t>(highlight)] = 5000;
    bins[static_cast<size_t>(outlier)] = 3;

    const float p = HistogramPercentileNits(bins, kContentPeakPercentile);
    EXPECT_FLOAT_EQ(p, FrameLuminanceHistogramBinCentreNits(highlight));
    EXPECT_FLOAT_EQ(HistogramPercentileNits(bins, 1.0f), FrameLuminanceHistogramBinCentreNits(outlier));
}

TEST(FrameLuminanceTest, PercentileIsMonotonicInTheRequestedRank) {
    Histogram bins{};
    for (int bin = 10; bin < 40; ++bin) {
        bins[static_cast<size_t>(bin)] = static_cast<uint32_t>(bin);
    }
    float previous = 0.0f;
    for (float p = 0.0f; p <= 1.0f; p += 0.05f) {
        const float value = HistogramPercentileNits(bins, p);
        EXPECT_GE(value, previous) << "p=" << p;
        previous = value;
    }
}

TEST(FrameLuminanceTest, PercentileMatchesAKnownUniformDistribution) {
    // 100 pixels spread one per bin over bins 0..99 is impossible with 64 bins,
    // so use 64 bins x 100 pixels: the rank then falls on an exact bin boundary
    // and the expected bin is computable by hand.
    Histogram bins{};
    for (int bin = 0; bin < kFrameLuminanceHistogramBins; ++bin) {
        bins[static_cast<size_t>(bin)] = 100;
    }
    // 50 % leaves half the pixels above: bins 32..63 hold exactly half, so the
    // first bin whose cumulative top-down count exceeds that half is bin 31.
    EXPECT_FLOAT_EQ(HistogramPercentileNits(bins, 0.5f), FrameLuminanceHistogramBinCentreNits(31));
    // 75 % leaves a quarter above: bins 48..63 are exactly a quarter, so bin 47.
    EXPECT_FLOAT_EQ(HistogramPercentileNits(bins, 0.75f), FrameLuminanceHistogramBinCentreNits(47));
}

// --- stream accumulator -----------------------------------------------------

FrameLuminanceStats MakeStats(float min_nits, float mean_nits, float max_nits) {
    FrameLuminanceStats s;
    s.min_nits = min_nits;
    s.mean_nits = mean_nits;
    s.max_nits = max_nits;
    return s;
}

TEST(FrameLuminanceTest, AccumulatorTakesTheStreamWideExtremes) {
    StreamLuminanceAccumulator acc;
    EXPECT_FALSE(acc.HasData());
    acc.Accumulate(MakeStats(0.5f, 120.0f, 900.0f));
    acc.Accumulate(MakeStats(0.1f, 400.0f, 600.0f));
    acc.Accumulate(MakeStats(2.0f, 90.0f, 1500.0f));

    EXPECT_TRUE(acc.HasData());
    EXPECT_EQ(acc.frames, 3u);
    EXPECT_FLOAT_EQ(acc.max_cll_nits, 1500.0f);
    EXPECT_FLOAT_EQ(acc.max_fall_nits, 400.0f);
    EXPECT_FLOAT_EQ(acc.min_cll_nits, 0.1f);
}

TEST(FrameLuminanceTest, AccumulatorNeverDecays) {
    StreamLuminanceAccumulator acc;
    acc.Accumulate(MakeStats(1.0f, 300.0f, 2000.0f));
    for (int i = 0; i < 100; ++i) {
        acc.Accumulate(MakeStats(1.0f, 10.0f, 20.0f));
    }
    EXPECT_FLOAT_EQ(acc.max_cll_nits, 2000.0f);
    EXPECT_FLOAT_EQ(acc.max_fall_nits, 300.0f);
}

TEST(FrameLuminanceTest, AccumulatorIgnoresNonFiniteMeasurements) {
    StreamLuminanceAccumulator acc;
    acc.Accumulate(MakeStats(1.0f, 100.0f, 500.0f));
    acc.Accumulate(MakeStats(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                             std::numeric_limits<float>::infinity()));
    EXPECT_FLOAT_EQ(acc.max_cll_nits, 500.0f);
    EXPECT_FLOAT_EQ(acc.max_fall_nits, 100.0f);
    EXPECT_FLOAT_EQ(acc.min_cll_nits, 1.0f);
}

// A device loss must not carry a dead device's content into the recovered file.
TEST(FrameLuminanceTest, ResetClearsEverything) {
    StreamLuminanceAccumulator acc;
    acc.Accumulate(MakeStats(1.0f, 300.0f, 2000.0f));
    acc.Reset();
    EXPECT_FALSE(acc.HasData());
    EXPECT_EQ(acc.frames, 0u);
    EXPECT_FLOAT_EQ(acc.max_cll_nits, 0.0f);
    EXPECT_FLOAT_EQ(acc.max_fall_nits, 0.0f);
}

TEST(FrameLuminanceTest, ContentLightLevelCodeRoundsAndSaturates) {
    EXPECT_EQ(ContentLightLevelCode(0.0f), 0u);
    EXPECT_EQ(ContentLightLevelCode(-5.0f), 0u);
    EXPECT_EQ(ContentLightLevelCode(999.4f), 999u);
    EXPECT_EQ(ContentLightLevelCode(999.6f), 1000u);
    EXPECT_EQ(ContentLightLevelCode(1.0e9f), 65535u);
    EXPECT_EQ(ContentLightLevelCode(std::numeric_limits<float>::quiet_NaN()), 0u);
}

// --- smoothing --------------------------------------------------------------

TEST(FrameLuminanceTest, SmootherAdoptsItsFirstMeasurementOutright) {
    ContentPeakSmoother s;
    EXPECT_FALSE(s.HasValue());
    EXPECT_FLOAT_EQ(s.Update(750.0f, 1.0f / 60.0f), 750.0f);
    EXPECT_TRUE(s.HasValue());
}

// The asymmetry, stated as the behaviour it exists for: a step up is followed
// within a couple of frames, a step down takes the better part of a second.
TEST(FrameLuminanceTest, SmootherRisesFastAndFallsSlow) {
    constexpr float kDt = 1.0f / 60.0f;
    ContentPeakSmoother s;
    s.Update(100.0f, kDt);

    int frames_up = 0;
    while (s.ValueNits() < 0.9f * 1000.0f && frames_up < 600) {
        s.Update(1000.0f, kDt);
        ++frames_up;
    }
    EXPECT_LE(frames_up, 9) << "a highlight must reach the knee before it burns out";

    int frames_down = 0;
    while (s.ValueNits() > 0.5f * 1000.0f && frames_down < 600) {
        s.Update(100.0f, kDt);
        ++frames_down;
    }
    EXPECT_GE(frames_down, 45) << "a fast fall is what makes the picture pump";
    EXPECT_GT(static_cast<float>(frames_down), 4.0f * static_cast<float>(frames_up));
}

// The time constants have to mean what they are named: one tau of a step covers
// 1 - 1/e of the distance, whatever step size and frame rate are used.
TEST(FrameLuminanceTest, SmootherFollowsItsNamedTimeConstants) {
    constexpr float kDt = 1.0f / 120.0f;
    constexpr float kExpected = 0.6321206f; // 1 - exp(-1)

    ContentPeakSmoother rising;
    rising.Update(100.0f, kDt);
    for (float t = 0.0f; t < kContentPeakRiseTimeConstantSeconds - 0.5f * kDt; t += kDt) {
        rising.Update(1100.0f, kDt);
    }
    EXPECT_NEAR((rising.ValueNits() - 100.0f) / 1000.0f, kExpected, 0.02f);

    ContentPeakSmoother falling;
    falling.Update(1100.0f, kDt);
    for (float t = 0.0f; t < kContentPeakFallTimeConstantSeconds - 0.5f * kDt; t += kDt) {
        falling.Update(100.0f, kDt);
    }
    EXPECT_NEAR((1100.0f - falling.ValueNits()) / 1000.0f, kExpected, 0.02f);
}

TEST(FrameLuminanceTest, SmootherIsFrameRateIndependent) {
    ContentPeakSmoother at_30;
    ContentPeakSmoother at_240;
    at_30.Update(100.0f, 1.0f / 30.0f);
    at_240.Update(100.0f, 1.0f / 240.0f);
    for (int i = 0; i < 30; ++i) {
        at_30.Update(1000.0f, 1.0f / 30.0f);
    }
    for (int i = 0; i < 240; ++i) {
        at_240.Update(1000.0f, 1.0f / 240.0f);
    }
    EXPECT_NEAR(at_30.ValueNits(), at_240.ValueNits(), 1.0f);
}

TEST(FrameLuminanceTest, SmootherRejectsNonsenseWithoutLosingItsState) {
    ContentPeakSmoother s;
    s.Update(500.0f, 1.0f / 60.0f);
    EXPECT_FLOAT_EQ(s.Update(std::numeric_limits<float>::quiet_NaN(), 1.0f / 60.0f), 500.0f);
    EXPECT_FLOAT_EQ(s.Update(-1.0f, 1.0f / 60.0f), 500.0f);
}

TEST(FrameLuminanceTest, SmootherResetForgetsItsValue) {
    ContentPeakSmoother s;
    s.Update(500.0f, 1.0f / 60.0f);
    s.Reset();
    EXPECT_FALSE(s.HasValue());
    EXPECT_FLOAT_EQ(s.Update(90.0f, 1.0f / 60.0f), 90.0f);
}

// --- knee invariant ---------------------------------------------------------

// The invariant HdrToneMapTest.ResolvedPeakAlwaysClearsTheKneeAfterNormalisation
// pins for the display-derived peak, repeated for the measured one: whatever the
// content measures, the normalised peak must stay above the knee or the roll-off
// degenerates into a hard clamp at paper white.
TEST(FrameLuminanceTest, MeasuredPeakAlwaysClearsTheKneeAfterNormalisation) {
    const float whites[] = {0.0f, 80.0f, 203.0f, 240.0f, 400.0f, 480.0f, 100000.0f};
    const float measured[] = {0.0f, 0.001f, 1.0f, 79.0f, 100.0f, 203.0f, 480.0f, 1000.0f, 10000.0f};
    for (const float white : whites) {
        for (const float peak : measured) {
            const float paper_white = SdrPaperWhiteScale(white);
            const float peak_scale = ContentPeakScale(peak, white);
            EXPECT_GT(peak_scale / paper_white, kHdrToneMapKnee) << "white=" << white << " measured=" << peak;
        }
    }
}

TEST(FrameLuminanceTest, MeasuredPeakAbovePaperWhiteIsUsedUnchanged) {
    constexpr float kWhiteNits = 240.0f;
    EXPECT_FLOAT_EQ(ContentPeakScale(1600.0f, kWhiteNits), 1600.0f / 80.0f);
    // At or below paper white the floor takes over, and it is exactly paper white.
    EXPECT_FLOAT_EQ(ContentPeakScale(10.0f, kWhiteNits), kWhiteNits / 80.0f);
    EXPECT_FLOAT_EQ(ContentPeakScale(std::numeric_limits<float>::quiet_NaN(), kWhiteNits), kWhiteNits / 80.0f);
}

// The defect the whole pass exists to remove: on the measured hardware the
// display reports a peak below its own composed white, and the content peak is
// the value that actually describes the highlights.
TEST(FrameLuminanceTest, MeasuredPeakBeatsTheDisplayProxyOnTheMeasuredHardware) {
    constexpr float kWhiteNits = 480.0f;
    constexpr float kReportedDisplayPeakNits = 240.0f; // both panels report this
    constexpr float kMeasuredContentPeakNits = 1200.0f;

    const float fallback = exosnap::engine::HdrPeakScale(true, kReportedDisplayPeakNits, kWhiteNits);
    const float measured = ContentPeakScale(kMeasuredContentPeakNits, kWhiteNits);

    // The display proxy falls back to a guessed 1000 cd/m^2 because its reading
    // is unusable; the measurement needs no guess.
    EXPECT_FLOAT_EQ(fallback, exosnap::engine::kHdrFallbackPeakNits / 80.0f);
    EXPECT_FLOAT_EQ(measured, kMeasuredContentPeakNits / 80.0f);
    EXPECT_GT(measured, fallback);
}

} // namespace
