#include "nvenc_encoder.h"

#include <gtest/gtest.h>

// Tests for ComputeNvencRcParams / NvencConstQpForCodec — the pure, GPU-free
// rate-control mapping. The H.264 cases carry the historical expectations,
// which are the 0..51 constQP domain; the AV1 cases cover the ~0..255 qindex
// domain the same canonical CQ maps onto.
// NVENC SDK field names tested here (NV_ENC_RC_PARAMS):
//   rcParams.rateControlMode   (as uint32_t)
//   rcParams.constQP.qpIntra / qpInterP / qpInterB
//   rcParams.averageBitRate / maxBitRate

namespace recorder_core {

// ---------------------------------------------------------------------------
// ConstantQuality — CQP, quality-preset-driven QP values
// ---------------------------------------------------------------------------

TEST(ComputeNvencRcParams, ConstantQuality_High_ModeIsCQP) {
    const RcParams p = ComputeNvencRcParams(VideoCodec::H264, RateControlMode::ConstantQuality,
                                            CanonicalCq(QualityPreset::High), 20000);
    EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_CONSTQP));
}

TEST(ComputeNvencRcParams, ConstantQuality_High_QPValues) {
    const RcParams p = ComputeNvencRcParams(VideoCodec::H264, RateControlMode::ConstantQuality,
                                            CanonicalCq(QualityPreset::High), 20000);
    EXPECT_EQ(p.qpIntra, 19u);
    EXPECT_EQ(p.qpInterP, 21u);
    EXPECT_EQ(p.qpInterB, 21u);
}

TEST(ComputeNvencRcParams, ConstantQuality_Balanced_QPValues) {
    const RcParams p = ComputeNvencRcParams(VideoCodec::H264, RateControlMode::ConstantQuality,
                                            CanonicalCq(QualityPreset::Balanced), 20000);
    EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_CONSTQP));
    EXPECT_EQ(p.qpIntra, 24u);
    EXPECT_EQ(p.qpInterP, 26u);
    EXPECT_EQ(p.qpInterB, 26u);
}

TEST(ComputeNvencRcParams, ConstantQuality_Efficient_QPValues) {
    const RcParams p = ComputeNvencRcParams(VideoCodec::H264, RateControlMode::ConstantQuality,
                                            CanonicalCq(QualityPreset::Low), 20000);
    EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_CONSTQP));
    EXPECT_EQ(p.qpIntra, 30u);
    EXPECT_EQ(p.qpInterP, 32u);
    EXPECT_EQ(p.qpInterB, 32u);
}

TEST(ComputeNvencRcParams, ConstantQuality_BitrateFieldsAreZero) {
    const RcParams p = ComputeNvencRcParams(VideoCodec::H264, RateControlMode::ConstantQuality,
                                            CanonicalCq(QualityPreset::Balanced), 20000);
    EXPECT_EQ(p.averageBitRate, 0u);
    EXPECT_EQ(p.maxBitRate, 0u);
}

// ---------------------------------------------------------------------------
// Codec-aware constQP domain — the canonical CQ is a product scale defined to
// be H.264's quantizer scale; HEVC and AV1 are mapped onto it by a calibrated
// per-codec curve
// ---------------------------------------------------------------------------

TEST(NvencNativeQuantizer, TheFiftyOneCodecsAreTheIdentityOverTheWholeCanonicalRange) {
    // The canonical scale IS H.264's quantizer scale; anything else would
    // silently reinterpret every CQ a user has ever saved. HEVC shares it
    // because that is what the corpus measured, not by definition.
    for (VideoCodec codec : {VideoCodec::H264, VideoCodec::Hevc}) {
        for (uint32_t cq = kCqMin; cq <= kCqMax; ++cq) {
            EXPECT_EQ(NvencNativeQuantizer(codec, cq), cq) << "cq=" << cq;
        }
    }
}

TEST(NvencNativeQuantizer, EveryCodecIsMonotonicAndStaysInsideItsOwnDomain) {
    for (VideoCodec codec : {VideoCodec::H264, VideoCodec::Hevc, VideoCodec::Av1}) {
        const uint32_t ceiling = codec == VideoCodec::Av1 ? 255u : 51u;
        uint32_t previous = 0;
        for (uint32_t cq = kCqMin; cq <= kCqMax; ++cq) {
            const uint32_t native = NvencNativeQuantizer(codec, cq);
            EXPECT_GE(native, previous) << "cq=" << cq;
            EXPECT_LE(native, ceiling) << "cq=" << cq;
            previous = native;
        }
        EXPECT_EQ(NvencNativeQuantizer(codec, kCqMax), ceiling);
    }
}

TEST(NvencNativeQuantizer, Av1SpansItsWholeDomainAcrossTheShippedLadder) {
    // The product symptom the calibration fixes: at a flat 5x the five tiers
    // occupied 80..175 of ~255 and produced one visual result at the top.
    const uint32_t ultra = NvencNativeQuantizer(VideoCodec::Av1, CanonicalCq(QualityPreset::Ultra));
    const uint32_t draft = NvencNativeQuantizer(VideoCodec::Av1, CanonicalCq(QualityPreset::Draft));
    EXPECT_LT(ultra, draft);
    EXPECT_GT(draft - ultra, 100u);
}

TEST(NvencNativeQuantizer, ShippedTiersMapToTheirCalibratedQuantizers) {
    // The five tier mappings are a product contract, not a benchmark artifact:
    // they are what "High on AV1" means. Changing one changes recorded output.
    EXPECT_EQ(NvencNativeQuantizer(VideoCodec::Av1, CanonicalCq(QualityPreset::Ultra)), 42u);
    EXPECT_EQ(NvencNativeQuantizer(VideoCodec::Av1, CanonicalCq(QualityPreset::High)), 65u);
    EXPECT_EQ(NvencNativeQuantizer(VideoCodec::Av1, CanonicalCq(QualityPreset::Balanced)), 94u);
    EXPECT_EQ(NvencNativeQuantizer(VideoCodec::Av1, CanonicalCq(QualityPreset::Low)), 135u);
    EXPECT_EQ(NvencNativeQuantizer(VideoCodec::Av1, CanonicalCq(QualityPreset::Draft)), 167u);
}

TEST(NvencConstQpForCodec, InterFramesCarryTwoCanonicalStepsInEachCodecsDomain) {
    for (VideoCodec codec : {VideoCodec::H264, VideoCodec::Hevc, VideoCodec::Av1}) {
        const ConstQp qp = NvencConstQpForCodec(codec, 19u);
        EXPECT_EQ(qp.intra, NvencNativeQuantizer(codec, 19u));
        EXPECT_EQ(qp.inter, NvencNativeQuantizer(codec, 21u));
        EXPECT_GT(qp.inter, qp.intra);
    }
}

TEST(NvencConstQpForCodec, ClampsInTheCanonicalDomainSoNoCodecCeilingIsExceeded) {
    // Clamped to kCqMax = 51 before conversion, so AV1 tops out at 255 and never
    // above it, and the inter offset cannot push past the ceiling either.
    const ConstQp av1 = NvencConstQpForCodec(VideoCodec::Av1, 4000u);
    EXPECT_EQ(av1.intra, 255u);
    EXPECT_EQ(av1.inter, 255u);
    const ConstQp h264 = NvencConstQpForCodec(VideoCodec::H264, 4000u);
    EXPECT_EQ(h264.intra, 51u);
    EXPECT_EQ(h264.inter, 51u);
    const ConstQp low = NvencConstQpForCodec(VideoCodec::Av1, 0u);
    EXPECT_EQ(low.intra, 5u);
}

TEST(ComputeNvencRcParams, ConstantQuality_Av1UsesTheCalibratedDomain) {
    const RcParams p = ComputeNvencRcParams(VideoCodec::Av1, RateControlMode::ConstantQuality,
                                            CanonicalCq(QualityPreset::High), 20000);
    EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_CONSTQP));
    EXPECT_EQ(p.qpIntra, NvencNativeQuantizer(VideoCodec::Av1, CanonicalCq(QualityPreset::High)));
    EXPECT_EQ(p.qpInterP, NvencNativeQuantizer(VideoCodec::Av1, CanonicalCq(QualityPreset::High) + 2u));
    EXPECT_EQ(p.qpInterB, p.qpInterP);
}

TEST(ComputeNvencRcParams, BitrateModesIgnoreTheCodecDomain) {
    // VBR/CBR carry no constQP, so the codec must not change anything there.
    const RcParams av1 = ComputeNvencRcParams(VideoCodec::Av1, RateControlMode::VariableBitrate,
                                              CanonicalCq(QualityPreset::High), 20000);
    const RcParams h264 = ComputeNvencRcParams(VideoCodec::H264, RateControlMode::VariableBitrate,
                                               CanonicalCq(QualityPreset::High), 20000);
    EXPECT_EQ(av1.averageBitRate, h264.averageBitRate);
    EXPECT_EQ(av1.maxBitRate, h264.maxBitRate);
    EXPECT_EQ(av1.qpIntra, 0u);
    EXPECT_EQ(h264.qpIntra, 0u);
}

// ---------------------------------------------------------------------------
// VariableBitrate — VBR, bitrate-driven
// ---------------------------------------------------------------------------

TEST(ComputeNvencRcParams, VariableBitrate_ModeIsVBR) {
    const RcParams p = ComputeNvencRcParams(VideoCodec::H264, RateControlMode::VariableBitrate,
                                            CanonicalCq(QualityPreset::Balanced), 20000);
    EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_VBR));
}

TEST(ComputeNvencRcParams, VariableBitrate_AverageBitRate) {
    // averageBitRate must be bitrate_kbps * 1000 (bps)
    const RcParams p = ComputeNvencRcParams(VideoCodec::H264, RateControlMode::VariableBitrate,
                                            CanonicalCq(QualityPreset::Balanced), 20000);
    EXPECT_EQ(p.averageBitRate, 20000u * 1000u);
}

TEST(ComputeNvencRcParams, VariableBitrate_MaxBitRateIs1_5x) {
    // maxBitRate must be averageBitRate * 3/2
    const RcParams p = ComputeNvencRcParams(VideoCodec::H264, RateControlMode::VariableBitrate,
                                            CanonicalCq(QualityPreset::Balanced), 20000);
    EXPECT_EQ(p.maxBitRate, 20000u * 1000u * 3u / 2u);
}

TEST(ComputeNvencRcParams, VariableBitrate_QPFieldsAreZero) {
    const RcParams p = ComputeNvencRcParams(VideoCodec::H264, RateControlMode::VariableBitrate,
                                            CanonicalCq(QualityPreset::Balanced), 10000);
    EXPECT_EQ(p.qpIntra, 0u);
    EXPECT_EQ(p.qpInterP, 0u);
    EXPECT_EQ(p.qpInterB, 0u);
}

// ---------------------------------------------------------------------------
// ConstantBitrate — CBR, strict bitrate
// ---------------------------------------------------------------------------

TEST(ComputeNvencRcParams, ConstantBitrate_ModeIsCBR) {
    const RcParams p = ComputeNvencRcParams(VideoCodec::H264, RateControlMode::ConstantBitrate,
                                            CanonicalCq(QualityPreset::Balanced), 10000);
    EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_CBR));
}

TEST(ComputeNvencRcParams, ConstantBitrate_AverageEqualMax) {
    // For CBR: averageBitRate == maxBitRate
    const RcParams p = ComputeNvencRcParams(VideoCodec::H264, RateControlMode::ConstantBitrate,
                                            CanonicalCq(QualityPreset::Balanced), 10000);
    EXPECT_EQ(p.averageBitRate, 10000u * 1000u);
    EXPECT_EQ(p.maxBitRate, 10000u * 1000u);
    EXPECT_EQ(p.averageBitRate, p.maxBitRate);
}

// ---------------------------------------------------------------------------
// Lossless — not implemented; falls back to ConstantQuality/Balanced
// ---------------------------------------------------------------------------

TEST(ComputeNvencRcParams, Lossless_FallsBackToConstantQuality) {
    const RcParams p =
        ComputeNvencRcParams(VideoCodec::H264, RateControlMode::Lossless, CanonicalCq(QualityPreset::High), 20000);
    // Must fall back to CQP, not pass through
    EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_CONSTQP));
}

// --- Arbitrary CQ values (the expert spinbox is no longer limited to 19/24/30) --

TEST(ComputeNvencRcParams, ConstantQuality_ArbitraryCqReachesTheEncoder) {
    // The regression: only three CQ values used to survive to the encoder.
    for (uint32_t cq : {17u, 20u, 22u, 27u, 33u, 45u}) {
        const RcParams p = ComputeNvencRcParams(VideoCodec::H264, RateControlMode::ConstantQuality, cq, 20000);
        EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_CONSTQP));
        EXPECT_EQ(p.qpIntra, cq);
        EXPECT_EQ(p.qpInterP, cq + 2);
        EXPECT_EQ(p.qpInterB, cq + 2);
    }
}

TEST(ComputeNvencRcParams, ConstantQuality_ClampsOutOfRangeAndInterQp) {
    const RcParams low = ComputeNvencRcParams(VideoCodec::H264, RateControlMode::ConstantQuality, 0, 20000);
    EXPECT_EQ(low.qpIntra, kCqMin);
    const RcParams high = ComputeNvencRcParams(VideoCodec::H264, RateControlMode::ConstantQuality, 99, 20000);
    EXPECT_EQ(high.qpIntra, kCqMax);
    // Inter QP (+2) must never exceed the maximum either.
    EXPECT_EQ(high.qpInterP, kCqMax);
    const RcParams near_max = ComputeNvencRcParams(VideoCodec::H264, RateControlMode::ConstantQuality, 50, 20000);
    EXPECT_EQ(near_max.qpInterP, kCqMax);
}

// --- Preset <-> CQ mapping (single source of truth) -------------------------

TEST(QualityPresetMapping, CanonicalCqMatchesTheFiveTierLadder) {
    EXPECT_EQ(recorder_core::CanonicalCq(recorder_core::QualityPreset::Ultra), 16u);
    EXPECT_EQ(recorder_core::CanonicalCq(recorder_core::QualityPreset::High), 19u);
    EXPECT_EQ(recorder_core::CanonicalCq(recorder_core::QualityPreset::Balanced), 24u);
    EXPECT_EQ(recorder_core::CanonicalCq(recorder_core::QualityPreset::Low), 30u);
    EXPECT_EQ(recorder_core::CanonicalCq(recorder_core::QualityPreset::Draft), 35u);
}

TEST(QualityPresetMapping, NearestPresetRoundTripsAndSnapsBetweenValues) {
    using recorder_core::NearestQualityPreset;
    using recorder_core::QualityPreset;
    for (auto p : {QualityPreset::Ultra, QualityPreset::High, QualityPreset::Balanced, QualityPreset::Low,
                   QualityPreset::Draft}) {
        EXPECT_EQ(NearestQualityPreset(recorder_core::CanonicalCq(p)), p);
    }
    EXPECT_EQ(NearestQualityPreset(1u), QualityPreset::Ultra);
    EXPECT_EQ(NearestQualityPreset(17u), QualityPreset::Ultra);
    EXPECT_EQ(NearestQualityPreset(20u), QualityPreset::High);
    EXPECT_EQ(NearestQualityPreset(23u), QualityPreset::Balanced);
    EXPECT_EQ(NearestQualityPreset(28u), QualityPreset::Low);
    EXPECT_EQ(NearestQualityPreset(33u), QualityPreset::Draft);
    EXPECT_EQ(NearestQualityPreset(51u), QualityPreset::Draft);
    // Midpoints resolve toward the lower CQ (higher quality):
    EXPECT_EQ(NearestQualityPreset(27u), QualityPreset::Balanced);
    EXPECT_EQ(NearestQualityPreset(32u), QualityPreset::Low);
}

TEST(QualityPresetMapping, IsCanonicalCqOnlyForTheFiveNamedValues) {
    for (uint32_t cq = recorder_core::kCqMin; cq <= recorder_core::kCqMax; ++cq) {
        const bool expected = (cq == 16u || cq == 19u || cq == 24u || cq == 30u || cq == 35u);
        EXPECT_EQ(recorder_core::IsCanonicalCq(cq), expected) << "cq=" << cq;
    }
}

} // namespace recorder_core
