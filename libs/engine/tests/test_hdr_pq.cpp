// Pure-math tests for the native HDR10 (PQ / BT.2020 / P010) output chain. No
// GPU: the compute shader (gpu_hdr_pq.*) replicates hdr_pq.h verbatim, so
// pinning the CPU reference here pins the encoded HDR10 signal too.

#include "hdr_pq.h"

#include <exosnap/engine/hdr_native.h>

#include <cmath>

#include <gtest/gtest.h>

using namespace exosnap::engine;

namespace {

// ---- Effective-mode decision (HdrMode x hdr_active x codec) -----------------

TEST(HdrNative, EffectiveOnlyWhenHdr10AndActiveAndCapable) {
    // The full truth table: native engages only for Hdr10 + HDR-active display +
    // an HDR10-capable codec (HEVC/AV1).
    EXPECT_TRUE(IsHdr10NativeEffective(HdrMode::Hdr10, true, VideoCodec::Hevc));
    EXPECT_TRUE(IsHdr10NativeEffective(HdrMode::Hdr10, true, VideoCodec::Av1));

    // Wrong mode.
    EXPECT_FALSE(IsHdr10NativeEffective(HdrMode::TonemapSdr, true, VideoCodec::Av1));
    EXPECT_FALSE(IsHdr10NativeEffective(HdrMode::Off, true, VideoCodec::Av1));
    // SDR display (not HDR-active) — never native even in Hdr10.
    EXPECT_FALSE(IsHdr10NativeEffective(HdrMode::Hdr10, false, VideoCodec::Av1));
    // H.264 cannot encode HDR10.
    EXPECT_FALSE(IsHdr10NativeEffective(HdrMode::Hdr10, true, VideoCodec::H264));
}

TEST(HdrNative, CodecCapability) {
    EXPECT_TRUE(CodecSupportsHdr10Native(VideoCodec::Hevc));
    EXPECT_TRUE(CodecSupportsHdr10Native(VideoCodec::Av1));
    EXPECT_FALSE(CodecSupportsHdr10Native(VideoCodec::H264));
}

// ---- HDR10 colour-metadata assembly ----------------------------------------

HdrDisplayFacts SampleHdrFacts() {
    // Roughly the DCI-P3 primaries + 400-nit peak an HDR display reports.
    HdrDisplayFacts f;
    f.hdr_active = true;
    f.red_primary_x = 0.6800f;
    f.red_primary_y = 0.3200f;
    f.green_primary_x = 0.2650f;
    f.green_primary_y = 0.6900f;
    f.blue_primary_x = 0.1500f;
    f.blue_primary_y = 0.0600f;
    f.white_point_x = 0.3127f;
    f.white_point_y = 0.3290f;
    f.max_luminance_nits = 400.0f;
    f.min_luminance_nits = 0.0100f;
    return f;
}

TEST(HdrNative, ColorMetadataIsPqBt2020Limited10Bit) {
    const ColorMetadata c = MakeHdr10ColorMetadata(SampleHdrFacts());
    EXPECT_EQ(c.primaries, ColorPrimaries::Bt2020);
    EXPECT_EQ(c.transfer, TransferCharacteristics::SmpteSt2084);
    EXPECT_EQ(c.matrix, MatrixCoefficients::Bt2020Ncl);
    EXPECT_EQ(c.range, ColorRange::Limited);
    EXPECT_EQ(c.bits_per_channel, 10u);
    EXPECT_TRUE(c.hdr);
    // No per-frame content-light analysis: MaxCLL/MaxFALL stay absent.
    EXPECT_EQ(c.max_content_light_level, 0u);
    EXPECT_EQ(c.max_frame_average_light_level, 0u);
}

TEST(HdrNative, MasteringDisplayFilledFromFacts) {
    const HdrDisplayFacts f = SampleHdrFacts();
    const ColorMetadata c = MakeHdr10ColorMetadata(f);
    ASSERT_TRUE(c.has_mastering_display);
    EXPECT_FLOAT_EQ(c.mastering_display_primary_r_x, f.red_primary_x);
    EXPECT_FLOAT_EQ(c.mastering_display_primary_g_y, f.green_primary_y);
    EXPECT_FLOAT_EQ(c.mastering_display_primary_b_x, f.blue_primary_x);
    EXPECT_FLOAT_EQ(c.mastering_display_white_point_x, f.white_point_x);
    EXPECT_FLOAT_EQ(c.mastering_display_max_luminance, 400.0f);
    EXPECT_FLOAT_EQ(c.mastering_display_min_luminance, 0.0100f);
}

TEST(HdrNative, DegenerateFactsOmitMasteringButKeepPq) {
    // A display with no usable chromaticity/luminance readings still signals
    // PQ/BT.2020 but writes no (zeroed) mastering element.
    HdrDisplayFacts f;
    f.hdr_active = true; // all primaries/luminance left at 0
    const ColorMetadata c = MakeHdr10ColorMetadata(f);
    EXPECT_EQ(c.transfer, TransferCharacteristics::SmpteSt2084);
    EXPECT_FALSE(c.has_mastering_display);
}

// ---- PQ OETF (SMPTE ST 2084) golden values ---------------------------------

TEST(HdrPq, PqOetfGoldenValues) {
    // L is the fraction of the 10 000-nit PQ ceiling.
    EXPECT_NEAR(PqOetf(0.0f), 0.0f, 1e-5f);                     // 0 nits -> ~0 (black)
    EXPECT_NEAR(PqOetf(80.0f / 10000.0f), 0.4858568f, 1e-5f);   // 80 nits (reference white)
    EXPECT_NEAR(PqOetf(100.0f / 10000.0f), 0.5080784f, 1e-5f);  // 100 nits
    EXPECT_NEAR(PqOetf(1000.0f / 10000.0f), 0.7518271f, 1e-5f); // 1000 nits
    EXPECT_NEAR(PqOetf(1.0f), 1.0f, 1e-6f);                     // 10 000 nits -> 1.0
}

TEST(HdrPq, PqOetfMonotonicAndClamped) {
    float prev = -1.0f;
    for (int i = 0; i <= 1000; ++i) {
        const float l = static_cast<float>(i) / 1000.0f;
        const float e = PqOetf(l);
        EXPECT_GE(e, prev - 1e-6f) << "non-monotonic at L=" << l;
        EXPECT_GE(e, 0.0f);
        EXPECT_LE(e, 1.0f);
        EXPECT_FALSE(std::isnan(e));
        prev = e;
    }
    EXPECT_FLOAT_EQ(PqOetf(-0.5f), PqOetf(0.0f)); // clamps
    EXPECT_FLOAT_EQ(PqOetf(2.0f), 1.0f);          // clamps
}

// ---- scRGB nit-scaling (1.0 = 80 nits) --------------------------------------

TEST(HdrPq, ScrgbNitScaling) {
    EXPECT_FLOAT_EQ(ScrgbToPqNormalized(1.0f), 80.0f / 10000.0f);    // reference white
    EXPECT_FLOAT_EQ(ScrgbToPqNormalized(12.5f), 1000.0f / 10000.0f); // 1000 nits
    EXPECT_FLOAT_EQ(ScrgbToPqNormalized(125.0f), 1.0f);              // 10 000 nits
    // Pure scaling, no clamp at either end: the ceiling is applied by PqOetf and
    // a negative must survive to the gamut matrix (see the next test).
    EXPECT_FLOAT_EQ(ScrgbToPqNormalized(500.0f), 4.0f);
    EXPECT_FLOAT_EQ(ScrgbToPqNormalized(-0.5f), -40.0f / 10000.0f);
}

// A BT.2020 pure red in scRGB has negative green and blue. Clamping those away
// before the 709->2020 matrix desaturates it; carried through, the matrix lands
// it as a saturated red, which is what the encoded file has to carry.
TEST(HdrPq, WideGamutNegativesSurviveToTheMatrix) {
    const LinearRgb scrgb_red_2020{1.6604910023f, -0.1245504746f, -0.0181507634f};
    const LinearRgb lin = Bt709ToBt2020(LinearRgb{
        ScrgbToPqNormalized(scrgb_red_2020.r),
        ScrgbToPqNormalized(scrgb_red_2020.g),
        ScrgbToPqNormalized(scrgb_red_2020.b),
    });
    // Red carries all the energy; green and blue collapse to ~0 instead of
    // the ~7 % / ~2 % crosstalk a pre-clamped input produces.
    EXPECT_NEAR(lin.g / lin.r, 0.0f, 0.005f);
    EXPECT_NEAR(lin.b / lin.r, 0.0f, 0.005f);
}

// The ceiling still holds at the end of the chain, where it belongs.
TEST(HdrPq, CeilingIsAppliedByTheTransferFunction) {
    const P010Codes at_ceiling = ScrgbToP010(LinearRgb{125.0f, 125.0f, 125.0f});
    const P010Codes above_ceiling = ScrgbToP010(LinearRgb{500.0f, 500.0f, 500.0f});
    EXPECT_EQ(at_ceiling.y, above_ceiling.y);
    EXPECT_EQ(at_ceiling.cb, above_ceiling.cb);
    EXPECT_EQ(at_ceiling.cr, above_ceiling.cr);
}

// ---- BT.709 -> BT.2020 gamut matrix golden values --------------------------

TEST(HdrPq, Bt709ToBt2020GoldenValues) {
    const LinearRgb white = Bt709ToBt2020(LinearRgb{1.0f, 1.0f, 1.0f});
    EXPECT_NEAR(white.r, 1.0f, 1e-6f); // white maps to white (rows sum to 1)
    EXPECT_NEAR(white.g, 1.0f, 1e-6f);
    EXPECT_NEAR(white.b, 1.0f, 1e-6f);

    const LinearRgb red = Bt709ToBt2020(LinearRgb{1.0f, 0.0f, 0.0f});
    EXPECT_NEAR(red.r, 0.6274039f, 1e-6f);
    EXPECT_NEAR(red.g, 0.0690973f, 1e-6f);
    EXPECT_NEAR(red.b, 0.0163914f, 1e-6f);

    const LinearRgb green = Bt709ToBt2020(LinearRgb{0.0f, 1.0f, 0.0f});
    EXPECT_NEAR(green.r, 0.3292830f, 1e-6f);
    EXPECT_NEAR(green.g, 0.9195404f, 1e-6f);
    EXPECT_NEAR(green.b, 0.0880133f, 1e-6f);

    // BT.2020 is a wider gamut, so a BT.709 primary maps inside it: the mapped
    // green is less saturated (non-zero red/blue crosstalk), never negative here.
    EXPECT_GT(green.r, 0.0f);
    EXPECT_GT(green.b, 0.0f);
}

// ---- Y'CbCr BT.2020 NCL + 10-bit limited-range boundaries ------------------

TEST(HdrPq, YcbcrLimitedRangeBoundaries) {
    // Black and white land on the narrow-range luma endpoints; neutral chroma.
    const P010Codes black = QuantizeYcbcr10Limited(PqRgbToYcbcr(0.0f, 0.0f, 0.0f));
    EXPECT_EQ(black.y, 64u);
    EXPECT_EQ(black.cb, 512u);
    EXPECT_EQ(black.cr, 512u);

    const P010Codes white = QuantizeYcbcr10Limited(PqRgbToYcbcr(1.0f, 1.0f, 1.0f));
    EXPECT_EQ(white.y, 940u);
    EXPECT_EQ(white.cb, 512u);
    EXPECT_EQ(white.cr, 512u);
}

TEST(HdrPq, YcbcrPrimaryCodes) {
    // Pure primed-red: luma from Kr, chroma extremes (Cr high, Cb low).
    const Ycbcr red = PqRgbToYcbcr(1.0f, 0.0f, 0.0f);
    EXPECT_NEAR(red.y, kKr2020, 1e-6f);
    EXPECT_NEAR(red.cr, 0.5f, 1e-6f);
    const P010Codes rc = QuantizeYcbcr10Limited(red);
    EXPECT_EQ(rc.y, 294u);
    EXPECT_EQ(rc.cb, 387u);
    EXPECT_EQ(rc.cr, 960u); // narrow-range chroma ceiling
}

TEST(HdrPq, CodesStayWithin10BitRange) {
    for (float r = 0.0f; r <= 1.0f; r += 0.125f) {
        for (float g = 0.0f; g <= 1.0f; g += 0.125f) {
            for (float b = 0.0f; b <= 1.0f; b += 0.125f) {
                const P010Codes c = PqRgbToP010(r, g, b);
                EXPECT_LE(c.y, 1023u);
                EXPECT_LE(c.cb, 1023u);
                EXPECT_LE(c.cr, 1023u);
            }
        }
    }
}

// ---- Full scRGB -> P010 chain golden values --------------------------------

TEST(HdrPq, FullChainGoldenValues) {
    // Neutral greys map to neutral chroma (512) with luma tracking the PQ curve.
    const auto grey = [](float v) { return ScrgbToP010(LinearRgb{v, v, v}); };

    const P010Codes black = grey(0.0f);
    EXPECT_EQ(black.y, 64u); // PQ(0)~0 -> narrow-range black
    EXPECT_EQ(black.cb, 512u);
    EXPECT_EQ(black.cr, 512u);

    const P010Codes white = grey(1.0f); // scRGB reference white (80 nits)
    EXPECT_EQ(white.y, 490u);           // PQ(80 nits) -> mid-scale, not clipped
    EXPECT_EQ(white.cb, 512u);
    EXPECT_EQ(white.cr, 512u);

    const P010Codes nits1000 = grey(12.5f);
    EXPECT_EQ(nits1000.y, 723u);

    const P010Codes ceiling = grey(125.0f); // 10 000 nits
    EXPECT_EQ(ceiling.y, 940u);
    const P010Codes above = grey(400.0f); // clamps to the ceiling, no overshoot
    EXPECT_EQ(above.y, 940u);
}

TEST(HdrPq, PqRgbPassthroughMatchesQuantizer) {
    // The already-PQ path is exactly the Y'CbCr conversion + packing (no
    // re-transfer): an HDR10 R10G10B10A2 desktop is passed straight through.
    for (float v : {0.0f, 0.25f, 0.5081f, 0.75f, 1.0f}) {
        const P010Codes direct = PqRgbToP010(v, v, v);
        const P010Codes viaChain = QuantizeYcbcr10Limited(PqRgbToYcbcr(v, v, v));
        EXPECT_EQ(direct.y, viaChain.y);
        EXPECT_EQ(direct.cb, viaChain.cb);
        EXPECT_EQ(direct.cr, viaChain.cr);
    }
}

} // namespace
