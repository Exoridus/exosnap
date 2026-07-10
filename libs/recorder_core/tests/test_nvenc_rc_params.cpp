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
    const RcParams p =
        ComputeNvencRcParams(RateControlMode::ConstantQuality, CanonicalCq(NvencQualityPreset::High), 20000);
    EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_CONSTQP));
}

TEST(ComputeNvencRcParams, ConstantQuality_High_QPValues) {
    const RcParams p =
        ComputeNvencRcParams(RateControlMode::ConstantQuality, CanonicalCq(NvencQualityPreset::High), 20000);
    EXPECT_EQ(p.qpIntra, 19u);
    EXPECT_EQ(p.qpInterP, 21u);
    EXPECT_EQ(p.qpInterB, 21u);
}

TEST(ComputeNvencRcParams, ConstantQuality_Balanced_QPValues) {
    const RcParams p =
        ComputeNvencRcParams(RateControlMode::ConstantQuality, CanonicalCq(NvencQualityPreset::Balanced), 20000);
    EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_CONSTQP));
    EXPECT_EQ(p.qpIntra, 24u);
    EXPECT_EQ(p.qpInterP, 26u);
    EXPECT_EQ(p.qpInterB, 26u);
}

TEST(ComputeNvencRcParams, ConstantQuality_Small_QPValues) {
    const RcParams p =
        ComputeNvencRcParams(RateControlMode::ConstantQuality, CanonicalCq(NvencQualityPreset::Small), 20000);
    EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_CONSTQP));
    EXPECT_EQ(p.qpIntra, 30u);
    EXPECT_EQ(p.qpInterP, 32u);
    EXPECT_EQ(p.qpInterB, 32u);
}

TEST(ComputeNvencRcParams, ConstantQuality_BitrateFieldsAreZero) {
    const RcParams p =
        ComputeNvencRcParams(RateControlMode::ConstantQuality, CanonicalCq(NvencQualityPreset::Balanced), 20000);
    EXPECT_EQ(p.averageBitRate, 0u);
    EXPECT_EQ(p.maxBitRate, 0u);
}

// ---------------------------------------------------------------------------
// VariableBitrate — VBR, bitrate-driven
// ---------------------------------------------------------------------------

TEST(ComputeNvencRcParams, VariableBitrate_ModeIsVBR) {
    const RcParams p =
        ComputeNvencRcParams(RateControlMode::VariableBitrate, CanonicalCq(NvencQualityPreset::Balanced), 20000);
    EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_VBR));
}

TEST(ComputeNvencRcParams, VariableBitrate_AverageBitRate) {
    // averageBitRate must be bitrate_kbps * 1000 (bps)
    const RcParams p =
        ComputeNvencRcParams(RateControlMode::VariableBitrate, CanonicalCq(NvencQualityPreset::Balanced), 20000);
    EXPECT_EQ(p.averageBitRate, 20000u * 1000u);
}

TEST(ComputeNvencRcParams, VariableBitrate_MaxBitRateIs1_5x) {
    // maxBitRate must be averageBitRate * 3/2
    const RcParams p =
        ComputeNvencRcParams(RateControlMode::VariableBitrate, CanonicalCq(NvencQualityPreset::Balanced), 20000);
    EXPECT_EQ(p.maxBitRate, 20000u * 1000u * 3u / 2u);
}

TEST(ComputeNvencRcParams, VariableBitrate_QPFieldsAreZero) {
    const RcParams p =
        ComputeNvencRcParams(RateControlMode::VariableBitrate, CanonicalCq(NvencQualityPreset::Balanced), 10000);
    EXPECT_EQ(p.qpIntra, 0u);
    EXPECT_EQ(p.qpInterP, 0u);
    EXPECT_EQ(p.qpInterB, 0u);
}

// ---------------------------------------------------------------------------
// ConstantBitrate — CBR, strict bitrate
// ---------------------------------------------------------------------------

TEST(ComputeNvencRcParams, ConstantBitrate_ModeIsCBR) {
    const RcParams p =
        ComputeNvencRcParams(RateControlMode::ConstantBitrate, CanonicalCq(NvencQualityPreset::Balanced), 10000);
    EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_CBR));
}

TEST(ComputeNvencRcParams, ConstantBitrate_AverageEqualMax) {
    // For CBR: averageBitRate == maxBitRate
    const RcParams p =
        ComputeNvencRcParams(RateControlMode::ConstantBitrate, CanonicalCq(NvencQualityPreset::Balanced), 10000);
    EXPECT_EQ(p.averageBitRate, 10000u * 1000u);
    EXPECT_EQ(p.maxBitRate, 10000u * 1000u);
    EXPECT_EQ(p.averageBitRate, p.maxBitRate);
}

// ---------------------------------------------------------------------------
// Lossless — not implemented; falls back to ConstantQuality/Balanced
// ---------------------------------------------------------------------------

TEST(ComputeNvencRcParams, Lossless_FallsBackToConstantQuality) {
    const RcParams p = ComputeNvencRcParams(RateControlMode::Lossless, CanonicalCq(NvencQualityPreset::High), 20000);
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
    EXPECT_EQ(low.qpIntra, kNvencCqMin);
    const RcParams high = ComputeNvencRcParams(RateControlMode::ConstantQuality, 99, 20000);
    EXPECT_EQ(high.qpIntra, kNvencCqMax);
    // Inter QP (+2) must never exceed the maximum either.
    EXPECT_EQ(high.qpInterP, kNvencCqMax);
    const RcParams near_max = ComputeNvencRcParams(RateControlMode::ConstantQuality, 50, 20000);
    EXPECT_EQ(near_max.qpInterP, kNvencCqMax);
}

// --- Preset <-> CQ mapping (single source of truth) -------------------------

TEST(QualityPresetMapping, CanonicalCqMatchesTheHistoricQpValues) {
    EXPECT_EQ(CanonicalCq(NvencQualityPreset::High), 19u);
    EXPECT_EQ(CanonicalCq(NvencQualityPreset::Balanced), 24u);
    EXPECT_EQ(CanonicalCq(NvencQualityPreset::Small), 30u);
}

TEST(QualityPresetMapping, NearestPresetRoundTripsAndSnapsBetweenValues) {
    for (auto p : {NvencQualityPreset::High, NvencQualityPreset::Balanced, NvencQualityPreset::Small}) {
        EXPECT_EQ(NearestQualityPreset(CanonicalCq(p)), p);
    }
    EXPECT_EQ(NearestQualityPreset(1), NvencQualityPreset::High);
    EXPECT_EQ(NearestQualityPreset(20), NvencQualityPreset::High);    // |20-19|=1
    EXPECT_EQ(NearestQualityPreset(23), NvencQualityPreset::Balanced) // |23-24|=1
        << "23 is nearer Balanced than High";
    EXPECT_EQ(NearestQualityPreset(28), NvencQualityPreset::Small); // |28-30|=2 < |28-24|=4
    EXPECT_EQ(NearestQualityPreset(51), NvencQualityPreset::Small);
    // Exact midpoints resolve toward the higher quality (lower CQ).
    EXPECT_EQ(NearestQualityPreset(27), NvencQualityPreset::Balanced);
}

TEST(QualityPresetMapping, IsCanonicalCqOnlyForTheThreeNamedValues) {
    EXPECT_TRUE(IsCanonicalCq(19));
    EXPECT_TRUE(IsCanonicalCq(24));
    EXPECT_TRUE(IsCanonicalCq(30));
    EXPECT_FALSE(IsCanonicalCq(20));
    EXPECT_FALSE(IsCanonicalCq(23));
}

} // namespace recorder_core
