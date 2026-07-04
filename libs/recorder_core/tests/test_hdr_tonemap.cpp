// Pure-math tests for the scRGB(HDR) -> SDR BT.709 tone-map and for the OD
// capture-format x HDR-mode negotiation decision. No GPU: the shader replicates
// HdrToneMapChannel/Bt709Oetf verbatim, so pinning the CPU reference here pins
// the on-screen result too.

#include "dxgi_od_capture_src.h"
#include "hdr_tonemap.h"

#include <cmath>

#include <gtest/gtest.h>

using namespace recorder_core;

namespace {

// 400 cd/m^2 display peak in reference-white multiples (400 / 80).
constexpr float kPeak400 = 5.0f;

TEST(HdrToneMap, BlackMapsToZero) {
    EXPECT_FLOAT_EQ(HdrToneMapChannel(0.0f, kPeak400), 0.0f);
    EXPECT_FLOAT_EQ(ScrgbToSdr709Channel(0.0f, kPeak400), 0.0f);
}

TEST(HdrToneMap, NegativeInputClampsToZero) {
    // Wide-gamut scRGB can go slightly negative; it must not produce negatives.
    EXPECT_FLOAT_EQ(HdrToneMapChannel(-0.5f, kPeak400), 0.0f);
}

TEST(HdrToneMap, ShadowsAndMidtonesPreservedBelowKnee) {
    // Below the knee the operator is identity (SDR-range content is untouched).
    EXPECT_FLOAT_EQ(HdrToneMapChannel(0.10f, kPeak400), 0.10f);
    EXPECT_FLOAT_EQ(HdrToneMapChannel(0.50f, kPeak400), 0.50f);
    EXPECT_FLOAT_EQ(HdrToneMapChannel(kHdrToneMapKnee, kPeak400), kHdrToneMapKnee);
}

TEST(HdrToneMap, ReferenceWhiteStaysBright) {
    // scRGB 1.0 == 80 cd/m^2 reference white. After the roll-off + BT.709 OETF
    // it must land near full white, not a dim grey (the "washed out" failure).
    const float linear = HdrToneMapChannel(1.0f, kPeak400);
    EXPECT_GT(linear, kHdrToneMapKnee); // above the knee, still climbing
    EXPECT_LT(linear, 1.0f);            // but rolled off below peak
    const float signal = ScrgbToSdr709Channel(1.0f, kPeak400);
    EXPECT_GT(signal, 0.85f); // visibly bright reference white
    EXPECT_LE(signal, 1.0f);
}

TEST(HdrToneMap, DisplayPeakMapsToOne) {
    // The display peak maps to exactly 1.0 (full SDR white) and no further.
    EXPECT_NEAR(HdrToneMapChannel(kPeak400, kPeak400), 1.0f, 1e-4f);
    EXPECT_NEAR(ScrgbToSdr709Channel(kPeak400, kPeak400), 1.0f, 1e-4f);
}

TEST(HdrToneMap, NeverExceedsOneAbovePeak) {
    // Content brighter than the display peak clamps, never overshoots.
    for (float x : {5.5f, 8.0f, 20.0f, 100.0f}) {
        const float linear = HdrToneMapChannel(x, kPeak400);
        EXPECT_LE(linear, 1.0f) << "x=" << x;
        EXPECT_FALSE(std::isnan(linear)) << "x=" << x;
        const float signal = ScrgbToSdr709Channel(x, kPeak400);
        EXPECT_LE(signal, 1.0f) << "x=" << x;
        EXPECT_GE(signal, 0.0f) << "x=" << x;
    }
}

TEST(HdrToneMap, MonotonicNonDecreasing) {
    float prev = -1.0f;
    for (int i = 0; i <= 600; ++i) {
        const float x = static_cast<float>(i) / 100.0f; // 0 .. 6.0
        const float y = HdrToneMapChannel(x, kPeak400);
        EXPECT_GE(y, prev - 1e-6f) << "non-monotonic at x=" << x;
        EXPECT_FALSE(std::isnan(y)) << "NaN at x=" << x;
        prev = y;
    }
}

TEST(Bt709Oetf, Endpoints) {
    EXPECT_FLOAT_EQ(Bt709Oetf(0.0f), 0.0f);
    EXPECT_NEAR(Bt709Oetf(1.0f), 1.0f, 1e-4f);
    EXPECT_FLOAT_EQ(Bt709Oetf(-1.0f), 0.0f); // clamps
    EXPECT_NEAR(Bt709Oetf(2.0f), 1.0f, 1e-4f);
}

TEST(HdrPeakScale, UsesActiveDisplayLuminance) {
    // An actively-HDR display's peak drives the knee.
    EXPECT_FLOAT_EQ(HdrPeakScale(true, 400.0f), 5.0f);
    EXPECT_FLOAT_EQ(HdrPeakScale(true, 1000.0f), 12.5f);
}

TEST(HdrPeakScale, IgnoresEdidCapsOfSdrDisplay) {
    // The EDID trap: an SDR-mode display reports inflated luminance caps
    // (measured 1499 cd/m^2). Not HDR-active -> fallback, never that value.
    const float expected_fallback = kHdrFallbackPeakNits / kHdrReferenceWhiteNits;
    EXPECT_FLOAT_EQ(HdrPeakScale(false, 1499.0f), expected_fallback);
    // Degenerate active reading at/below reference white also falls back.
    EXPECT_FLOAT_EQ(HdrPeakScale(true, 50.0f), expected_fallback);
    EXPECT_GE(HdrPeakScale(false, 0.0f), 1.0f);
}

// --- OD capture-format x HDR-mode negotiation ------------------------------

TEST(OdCaptureFormat, Fp16IsNowSupported) {
    EXPECT_TRUE(IsSupportedOdCaptureFormat(DXGI_FORMAT_B8G8R8A8_UNORM));
    EXPECT_TRUE(IsSupportedOdCaptureFormat(DXGI_FORMAT_R10G10B10A2_UNORM));
    EXPECT_TRUE(IsSupportedOdCaptureFormat(DXGI_FORMAT_R16G16B16A16_FLOAT));
    EXPECT_FALSE(IsSupportedOdCaptureFormat(DXGI_FORMAT_R8G8B8A8_UNORM));
}

TEST(OdCaptureMode, SdrFormatsAlwaysSdr) {
    OdCaptureMode mode{};
    for (HdrMode hdr : {HdrMode::Off, HdrMode::TonemapSdr, HdrMode::Hdr10}) {
        EXPECT_TRUE(ResolveOdCaptureMode(DXGI_FORMAT_B8G8R8A8_UNORM, hdr, mode));
        EXPECT_EQ(mode, OdCaptureMode::Sdr);
        EXPECT_TRUE(ResolveOdCaptureMode(DXGI_FORMAT_R10G10B10A2_UNORM, hdr, mode));
        EXPECT_EQ(mode, OdCaptureMode::Sdr);
    }
}

TEST(OdCaptureMode, Fp16TonemapsUnlessOff) {
    OdCaptureMode mode{};
    EXPECT_TRUE(ResolveOdCaptureMode(DXGI_FORMAT_R16G16B16A16_FLOAT, HdrMode::TonemapSdr, mode));
    EXPECT_EQ(mode, OdCaptureMode::HdrToneMap);
    // Hdr10 falls back to tone-map in this build (no native HDR10 output yet).
    EXPECT_TRUE(ResolveOdCaptureMode(DXGI_FORMAT_R16G16B16A16_FLOAT, HdrMode::Hdr10, mode));
    EXPECT_EQ(mode, OdCaptureMode::HdrToneMap);
    // Off: HDR desktop is a defined capture error, not recordable.
    EXPECT_FALSE(ResolveOdCaptureMode(DXGI_FORMAT_R16G16B16A16_FLOAT, HdrMode::Off, mode));
}

TEST(OdCaptureMode, UnknownFormatRejected) {
    OdCaptureMode mode{};
    EXPECT_FALSE(ResolveOdCaptureMode(DXGI_FORMAT_R8G8B8A8_UNORM, HdrMode::TonemapSdr, mode));
}

} // namespace
