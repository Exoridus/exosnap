#include "nvenc_encoder.h"

#include <gtest/gtest.h>

// Tests for ComputeNvencRcParams — the pure, GPU-free rate-control mapping function.
// NVENC SDK field names tested here (NV_ENC_RC_PARAMS):
//   rcParams.rateControlMode   (as uint32_t)
//   rcParams.constQP.qpIntra / qpInterP / qpInterB
//   rcParams.averageBitRate / maxBitRate

namespace recorder_core {

// ---------------------------------------------------------------------------
// ConstantQuality — CQP, quality-preset-driven QP values
// ---------------------------------------------------------------------------

TEST(ComputeNvencRcParams, ConstantQuality_High_ModeIsCQP) {
    const RcParams p = ComputeNvencRcParams(RateControlMode::ConstantQuality, CanonicalCq(QualityPreset::High), 20000);
    EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_CONSTQP));
}

TEST(ComputeNvencRcParams, ConstantQuality_High_QPValues) {
    const RcParams p = ComputeNvencRcParams(RateControlMode::ConstantQuality, CanonicalCq(QualityPreset::High), 20000);
    EXPECT_EQ(p.qpIntra, 19u);
    EXPECT_EQ(p.qpInterP, 21u);
    EXPECT_EQ(p.qpInterB, 21u);
}

TEST(ComputeNvencRcParams, ConstantQuality_Balanced_QPValues) {
    const RcParams p =
        ComputeNvencRcParams(RateControlMode::ConstantQuality, CanonicalCq(QualityPreset::Balanced), 20000);
    EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_CONSTQP));
    EXPECT_EQ(p.qpIntra, 24u);
    EXPECT_EQ(p.qpInterP, 26u);
    EXPECT_EQ(p.qpInterB, 26u);
}

TEST(ComputeNvencRcParams, ConstantQuality_Efficient_QPValues) {
    const RcParams p =
        ComputeNvencRcParams(RateControlMode::ConstantQuality, CanonicalCq(QualityPreset::Efficient), 20000);
    EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_CONSTQP));
    EXPECT_EQ(p.qpIntra, 30u);
    EXPECT_EQ(p.qpInterP, 32u);
    EXPECT_EQ(p.qpInterB, 32u);
}

TEST(ComputeNvencRcParams, ConstantQuality_BitrateFieldsAreZero) {
    const RcParams p =
        ComputeNvencRcParams(RateControlMode::ConstantQuality, CanonicalCq(QualityPreset::Balanced), 20000);
    EXPECT_EQ(p.averageBitRate, 0u);
    EXPECT_EQ(p.maxBitRate, 0u);
}

// ---------------------------------------------------------------------------
// VariableBitrate — VBR, bitrate-driven
// ---------------------------------------------------------------------------

TEST(ComputeNvencRcParams, VariableBitrate_ModeIsVBR) {
    const RcParams p =
        ComputeNvencRcParams(RateControlMode::VariableBitrate, CanonicalCq(QualityPreset::Balanced), 20000);
    EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_VBR));
}

TEST(ComputeNvencRcParams, VariableBitrate_AverageBitRate) {
    // averageBitRate must be bitrate_kbps * 1000 (bps)
    const RcParams p =
        ComputeNvencRcParams(RateControlMode::VariableBitrate, CanonicalCq(QualityPreset::Balanced), 20000);
    EXPECT_EQ(p.averageBitRate, 20000u * 1000u);
}

TEST(ComputeNvencRcParams, VariableBitrate_MaxBitRateIs1_5x) {
    // maxBitRate must be averageBitRate * 3/2
    const RcParams p =
        ComputeNvencRcParams(RateControlMode::VariableBitrate, CanonicalCq(QualityPreset::Balanced), 20000);
    EXPECT_EQ(p.maxBitRate, 20000u * 1000u * 3u / 2u);
}

TEST(ComputeNvencRcParams, VariableBitrate_QPFieldsAreZero) {
    const RcParams p =
        ComputeNvencRcParams(RateControlMode::VariableBitrate, CanonicalCq(QualityPreset::Balanced), 10000);
    EXPECT_EQ(p.qpIntra, 0u);
    EXPECT_EQ(p.qpInterP, 0u);
    EXPECT_EQ(p.qpInterB, 0u);
}

// ---------------------------------------------------------------------------
// ConstantBitrate — CBR, strict bitrate
// ---------------------------------------------------------------------------

TEST(ComputeNvencRcParams, ConstantBitrate_ModeIsCBR) {
    const RcParams p =
        ComputeNvencRcParams(RateControlMode::ConstantBitrate, CanonicalCq(QualityPreset::Balanced), 10000);
    EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_CBR));
}

TEST(ComputeNvencRcParams, ConstantBitrate_AverageEqualMax) {
    // For CBR: averageBitRate == maxBitRate
    const RcParams p =
        ComputeNvencRcParams(RateControlMode::ConstantBitrate, CanonicalCq(QualityPreset::Balanced), 10000);
    EXPECT_EQ(p.averageBitRate, 10000u * 1000u);
    EXPECT_EQ(p.maxBitRate, 10000u * 1000u);
    EXPECT_EQ(p.averageBitRate, p.maxBitRate);
}

// ---------------------------------------------------------------------------
// Lossless — not implemented; falls back to ConstantQuality/Balanced
// ---------------------------------------------------------------------------

TEST(ComputeNvencRcParams, Lossless_FallsBackToConstantQuality) {
    const RcParams p = ComputeNvencRcParams(RateControlMode::Lossless, CanonicalCq(QualityPreset::High), 20000);
    // Must fall back to CQP, not pass through
    EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_CONSTQP));
}

// --- Arbitrary CQ values (the expert spinbox is no longer limited to 19/24/30) --

TEST(ComputeNvencRcParams, ConstantQuality_ArbitraryCqReachesTheEncoder) {
    // The regression: only three CQ values used to survive to the encoder.
    for (uint32_t cq : {17u, 20u, 22u, 27u, 33u, 45u}) {
        const RcParams p = ComputeNvencRcParams(RateControlMode::ConstantQuality, cq, 20000);
        EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_CONSTQP));
        EXPECT_EQ(p.qpIntra, cq);
        EXPECT_EQ(p.qpInterP, cq + 2);
        EXPECT_EQ(p.qpInterB, cq + 2);
    }
}

TEST(ComputeNvencRcParams, ConstantQuality_ClampsOutOfRangeAndInterQp) {
    const RcParams low = ComputeNvencRcParams(RateControlMode::ConstantQuality, 0, 20000);
    EXPECT_EQ(low.qpIntra, kCqMin);
    const RcParams high = ComputeNvencRcParams(RateControlMode::ConstantQuality, 99, 20000);
    EXPECT_EQ(high.qpIntra, kCqMax);
    // Inter QP (+2) must never exceed the maximum either.
    EXPECT_EQ(high.qpInterP, kCqMax);
    const RcParams near_max = ComputeNvencRcParams(RateControlMode::ConstantQuality, 50, 20000);
    EXPECT_EQ(near_max.qpInterP, kCqMax);
}

// --- Preset <-> CQ mapping (single source of truth) -------------------------

TEST(QualityPresetMapping, CanonicalCqMatchesTheFiveTierLadder) {
    EXPECT_EQ(recorder_core::CanonicalCq(recorder_core::QualityPreset::Ultra), 16u);
    EXPECT_EQ(recorder_core::CanonicalCq(recorder_core::QualityPreset::High), 19u);
    EXPECT_EQ(recorder_core::CanonicalCq(recorder_core::QualityPreset::Balanced), 24u);
    EXPECT_EQ(recorder_core::CanonicalCq(recorder_core::QualityPreset::Efficient), 30u);
    EXPECT_EQ(recorder_core::CanonicalCq(recorder_core::QualityPreset::Draft), 35u);
}

TEST(QualityPresetMapping, NearestPresetRoundTripsAndSnapsBetweenValues) {
    using recorder_core::NearestQualityPreset;
    using recorder_core::QualityPreset;
    for (auto p : {QualityPreset::Ultra, QualityPreset::High, QualityPreset::Balanced, QualityPreset::Efficient,
                   QualityPreset::Draft}) {
        EXPECT_EQ(NearestQualityPreset(recorder_core::CanonicalCq(p)), p);
    }
    EXPECT_EQ(NearestQualityPreset(1u), QualityPreset::Ultra);
    EXPECT_EQ(NearestQualityPreset(17u), QualityPreset::Ultra);
    EXPECT_EQ(NearestQualityPreset(20u), QualityPreset::High);
    EXPECT_EQ(NearestQualityPreset(23u), QualityPreset::Balanced);
    EXPECT_EQ(NearestQualityPreset(28u), QualityPreset::Efficient);
    EXPECT_EQ(NearestQualityPreset(33u), QualityPreset::Draft);
    EXPECT_EQ(NearestQualityPreset(51u), QualityPreset::Draft);
    // Midpoints resolve toward the lower CQ (higher quality):
    EXPECT_EQ(NearestQualityPreset(27u), QualityPreset::Balanced);
    EXPECT_EQ(NearestQualityPreset(32u), QualityPreset::Efficient);
}

TEST(QualityPresetMapping, IsCanonicalCqOnlyForTheFiveNamedValues) {
    for (uint32_t cq = recorder_core::kCqMin; cq <= recorder_core::kCqMax; ++cq) {
        const bool expected = (cq == 16u || cq == 19u || cq == 24u || cq == 30u || cq == 35u);
        EXPECT_EQ(recorder_core::IsCanonicalCq(cq), expected) << "cq=" << cq;
    }
}

} // namespace recorder_core
