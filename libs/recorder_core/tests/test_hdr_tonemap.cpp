// Pure-math tests for the scRGB(HDR) -> SDR BT.709 tone-map and for the OD
// capture-format x HDR-mode negotiation decision. No GPU: the shader replicates
// HdrToneMapChannel/Bt709Oetf verbatim, so pinning the CPU reference here pins
// the on-screen result too.

#include "dxgi_od_capture_src.h"
#include "hdr_tonemap.h"

#include <recorder_core/hdr_color_space.h>

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

TEST(SrgbOetf, MatchesTheDesktopTransfer) {
    EXPECT_FLOAT_EQ(SrgbOetf(0.0f), 0.0f);
    EXPECT_NEAR(SrgbOetf(1.0f), 1.0f, 1e-5f);
    EXPECT_FLOAT_EQ(SrgbOetf(-1.0f), 0.0f); // clamps
    EXPECT_NEAR(SrgbOetf(2.0f), 1.0f, 1e-5f);
    // sRGB mid-grey (code 128) is linear ~0.2159 and must encode back to ~0.502.
    EXPECT_NEAR(SrgbOetf(0.2159f), 0.5019f, 1e-3f);
}

TEST(SrgbOetf, PreservesSdrScrgbLevelsUnlikeTheHdrCurve) {
    // An ACM (Advanced Color) desktop with HDR off delivers scRGB FP16 whose
    // reference white is exactly 1.0 (measured). Encoding it for an sRGB target
    // must round-trip; the HDR tone-map curve instead crushes white and shadows.
    EXPECT_NEAR(SrgbOetf(1.0f), 1.0f, 1e-5f);

    const float hdr_peak = HdrPeakScale(/*display_hdr_active=*/false, 0.0f);      // 1000/80 fallback
    EXPECT_NEAR(Bt709Oetf(HdrToneMapChannel(1.0f, hdr_peak)), 0.9135f, 1e-3f);    // white -> ~233/255
    EXPECT_NEAR(Bt709Oetf(HdrToneMapChannel(0.0144f, hdr_peak)), 0.0653f, 1e-3f); // code 32 -> ~17/255
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

// Convenience wrappers: default the newer parameters for the common cases.
namespace {
bool Resolve(DXGI_FORMAT fmt, HdrMode hdr, bool hdr_active, bool hdr10_ok, OdCaptureMode& m) {
    return ResolveOdCaptureMode(fmt, hdr, hdr_active, hdr10_ok, m);
}
} // namespace

TEST(OdCaptureMode, Bgra8AlwaysSdr) {
    OdCaptureMode mode{};
    for (HdrMode hdr : {HdrMode::Off, HdrMode::TonemapSdr, HdrMode::Hdr10}) {
        for (bool active : {false, true}) {
            EXPECT_TRUE(Resolve(DXGI_FORMAT_B8G8R8A8_UNORM, hdr, active, true, mode));
            EXPECT_EQ(mode, OdCaptureMode::Sdr);
        }
    }
}

TEST(OdCaptureMode, Rgb10SdrDesktopUnchanged) {
    // An SDR 10 bpc desktop (hdr_active=false) stays SDR in every mode — the
    // existing behaviour must not change.
    OdCaptureMode mode{};
    for (HdrMode hdr : {HdrMode::Off, HdrMode::TonemapSdr, HdrMode::Hdr10}) {
        EXPECT_TRUE(Resolve(DXGI_FORMAT_R10G10B10A2_UNORM, hdr, /*hdr_active=*/false, /*hdr10_ok=*/true, mode));
        EXPECT_EQ(mode, OdCaptureMode::Sdr);
    }
    // An HDR-active 10 bpc desktop that is NOT requested as native Hdr10 (or on a
    // non-HDR10 codec) also keeps SDR handling (unchanged legacy behaviour).
    EXPECT_TRUE(Resolve(DXGI_FORMAT_R10G10B10A2_UNORM, HdrMode::TonemapSdr, /*hdr_active=*/true, true, mode));
    EXPECT_EQ(mode, OdCaptureMode::Sdr);
    EXPECT_TRUE(Resolve(DXGI_FORMAT_R10G10B10A2_UNORM, HdrMode::Hdr10, /*hdr_active=*/true, /*hdr10_ok=*/false, mode));
    EXPECT_EQ(mode, OdCaptureMode::Sdr);
}

TEST(OdCaptureMode, Rgb10Hdr10DesktopIsNative) {
    // An HDR-active 10 bpc desktop, native requested, HDR10-capable codec.
    OdCaptureMode mode{};
    EXPECT_TRUE(Resolve(DXGI_FORMAT_R10G10B10A2_UNORM, HdrMode::Hdr10, /*hdr_active=*/true, /*hdr10_ok=*/true, mode));
    EXPECT_EQ(mode, OdCaptureMode::HdrNative);
}

TEST(OdCaptureMode, Fp16NativeWhenHdr10AndCapable) {
    OdCaptureMode mode{};
    EXPECT_TRUE(Resolve(DXGI_FORMAT_R16G16B16A16_FLOAT, HdrMode::Hdr10, true, /*hdr10_ok=*/true, mode));
    EXPECT_EQ(mode, OdCaptureMode::HdrNative);
}

TEST(OdCaptureMode, Fp16TonemapsForSdrModesAndH264) {
    OdCaptureMode mode{};
    EXPECT_TRUE(Resolve(DXGI_FORMAT_R16G16B16A16_FLOAT, HdrMode::TonemapSdr, true, true, mode));
    EXPECT_EQ(mode, OdCaptureMode::HdrToneMap);
    // Hdr10 requested but codec cannot encode HDR10 (H.264) -> tone-map to SDR.
    EXPECT_TRUE(Resolve(DXGI_FORMAT_R16G16B16A16_FLOAT, HdrMode::Hdr10, true, /*hdr10_ok=*/false, mode));
    EXPECT_EQ(mode, OdCaptureMode::HdrToneMap);
}

TEST(OdCaptureMode, Fp16OffIsCaptureError) {
    OdCaptureMode mode{};
    EXPECT_FALSE(Resolve(DXGI_FORMAT_R16G16B16A16_FLOAT, HdrMode::Off, /*hdr_active=*/true, true, mode));
}

TEST(OdCaptureMode, Fp16SdrDesktopIsScrgbSdr) {
    // Windows Advanced Color Management composites the desktop in scRGB FP16 even
    // with HDR switched off. The format alone must not imply an HDR desktop:
    // hdr_active disambiguates, exactly as it does for R10G10B10A2. Such a desktop
    // is SDR content (reference white == 1.0) and must only be sRGB-encoded, never
    // tone-mapped -- and it is recordable in every HDR-handling mode, including Off.
    OdCaptureMode mode{};
    for (HdrMode hdr : {HdrMode::Off, HdrMode::TonemapSdr, HdrMode::Hdr10}) {
        for (bool hdr10_ok : {false, true}) {
            EXPECT_TRUE(Resolve(DXGI_FORMAT_R16G16B16A16_FLOAT, hdr, /*hdr_active=*/false, hdr10_ok, mode));
            EXPECT_EQ(mode, OdCaptureMode::SdrScrgb);
        }
    }
}

TEST(OdCaptureMode, UnknownFormatRejected) {
    OdCaptureMode mode{};
    EXPECT_FALSE(Resolve(DXGI_FORMAT_R8G8B8A8_UNORM, HdrMode::TonemapSdr, false, true, mode));
}

// --- WGC (window) frame-pool plan ------------------------------------------

TEST(WgcCapturePlan, SdrDesktopKeepsBgra8) {
    // No HDR on the display: BGRA8 pool, plain SDR — byte-identical to the
    // historic window-capture path, in every HDR-handling mode.
    for (HdrMode hdr : {HdrMode::Off, HdrMode::TonemapSdr, HdrMode::Hdr10}) {
        const WgcCapturePlan plan = ResolveWgcCapturePlan(/*hdr_active=*/false, hdr, /*hdr10_ok=*/true);
        EXPECT_EQ(plan.frame_pool_format, DXGI_FORMAT_B8G8R8A8_UNORM);
        EXPECT_EQ(plan.mode, OdCaptureMode::Sdr);
    }
}

TEST(WgcCapturePlan, HdrDisplayButHandlingOffKeepsBgra8) {
    // HDR-active display but the user disabled HDR handling: keep BGRA8 (DWM
    // tone-maps the window to SDR), matching HdrMode::Off elsewhere.
    const WgcCapturePlan plan = ResolveWgcCapturePlan(/*hdr_active=*/true, HdrMode::Off, /*hdr10_ok=*/true);
    EXPECT_EQ(plan.frame_pool_format, DXGI_FORMAT_B8G8R8A8_UNORM);
    EXPECT_EQ(plan.mode, OdCaptureMode::Sdr);
}

TEST(WgcCapturePlan, HdrDisplayTonemapUsesFp16) {
    const WgcCapturePlan plan = ResolveWgcCapturePlan(/*hdr_active=*/true, HdrMode::TonemapSdr, /*hdr10_ok=*/true);
    EXPECT_EQ(plan.frame_pool_format, DXGI_FORMAT_R16G16B16A16_FLOAT);
    EXPECT_EQ(plan.mode, OdCaptureMode::HdrToneMap);
}

TEST(WgcCapturePlan, HdrDisplayNativeWhenHdr10AndCapable) {
    const WgcCapturePlan plan = ResolveWgcCapturePlan(/*hdr_active=*/true, HdrMode::Hdr10, /*hdr10_ok=*/true);
    EXPECT_EQ(plan.frame_pool_format, DXGI_FORMAT_R16G16B16A16_FLOAT);
    EXPECT_EQ(plan.mode, OdCaptureMode::HdrNative);
}

TEST(WgcCapturePlan, Hdr10OnIncapableCodecTonemaps) {
    // Hdr10 requested but the codec cannot encode HDR10 (H.264): FP16 pool, but
    // tone-map to SDR (mirrors the OD FP16 rule).
    const WgcCapturePlan plan = ResolveWgcCapturePlan(/*hdr_active=*/true, HdrMode::Hdr10, /*hdr10_ok=*/false);
    EXPECT_EQ(plan.frame_pool_format, DXGI_FORMAT_R16G16B16A16_FLOAT);
    EXPECT_EQ(plan.mode, OdCaptureMode::HdrToneMap);
}

// --- Tone-map intermediate format choice -----------------------------------

TEST(ToneMapIntermediateFormat, TenBitEncodeUsesR10) {
    // A 10-bit encode (P010) tone-maps into R10G10B10A2 so the extra depth
    // survives the RGB->P010 hop instead of being crushed at 8 bits.
    EXPECT_EQ(ToneMapIntermediateFormat(/*encode_is_10bit=*/true), DXGI_FORMAT_R10G10B10A2_UNORM);
}

TEST(ToneMapIntermediateFormat, EightBitEncodeKeepsBgra8) {
    // 8-bit encodes are byte-identical to the historic path: BGRA8 intermediate.
    EXPECT_EQ(ToneMapIntermediateFormat(/*encode_is_10bit=*/false), DXGI_FORMAT_B8G8R8A8_UNORM);
}

// A studio-range PQ display is in HDR just as much as a full-range one. The capture
// path used to accept only the full-range variant, so a studio-range HDR display made
// the coordinator pin PQ/BT.2020 metadata while capture reported SDR, and the session
// aborted on expectNativeHdr && !nativeHdr.
TEST(HdrColorSpace, BothPqRangesCountAsHdr) {
    EXPECT_TRUE(recorder_core::IsHdrColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020));
    EXPECT_TRUE(recorder_core::IsHdrColorSpace(DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020));
}

TEST(HdrColorSpace, NonPqColorSpacesAreNotHdr) {
    // sRGB desktop.
    EXPECT_FALSE(recorder_core::IsHdrColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709));
    // scRGB FP16 — an Advanced-Color SDR desktop, not HDR (see OdCaptureMode::SdrScrgb).
    EXPECT_FALSE(recorder_core::IsHdrColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709));
}

} // namespace
