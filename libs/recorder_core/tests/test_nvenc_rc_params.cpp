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
    const RcParams p = ComputeNvencRcParams(RateControlMode::ConstantQuality, NvencQualityPreset::High, 20000);
    EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_CONSTQP));
}

TEST(ComputeNvencRcParams, ConstantQuality_High_QPValues) {
    const RcParams p = ComputeNvencRcParams(RateControlMode::ConstantQuality, NvencQualityPreset::High, 20000);
    EXPECT_EQ(p.qpIntra, 19u);
    EXPECT_EQ(p.qpInterP, 21u);
    EXPECT_EQ(p.qpInterB, 21u);
}

TEST(ComputeNvencRcParams, ConstantQuality_Balanced_QPValues) {
    const RcParams p = ComputeNvencRcParams(RateControlMode::ConstantQuality, NvencQualityPreset::Balanced, 20000);
    EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_CONSTQP));
    EXPECT_EQ(p.qpIntra, 24u);
    EXPECT_EQ(p.qpInterP, 26u);
    EXPECT_EQ(p.qpInterB, 26u);
}

TEST(ComputeNvencRcParams, ConstantQuality_Small_QPValues) {
    const RcParams p = ComputeNvencRcParams(RateControlMode::ConstantQuality, NvencQualityPreset::Small, 20000);
    EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_CONSTQP));
    EXPECT_EQ(p.qpIntra, 30u);
    EXPECT_EQ(p.qpInterP, 32u);
    EXPECT_EQ(p.qpInterB, 32u);
}

TEST(ComputeNvencRcParams, ConstantQuality_BitrateFieldsAreZero) {
    const RcParams p = ComputeNvencRcParams(RateControlMode::ConstantQuality, NvencQualityPreset::Balanced, 20000);
    EXPECT_EQ(p.averageBitRate, 0u);
    EXPECT_EQ(p.maxBitRate, 0u);
}

// ---------------------------------------------------------------------------
// VariableBitrate — VBR, bitrate-driven
// ---------------------------------------------------------------------------

TEST(ComputeNvencRcParams, VariableBitrate_ModeIsVBR) {
    const RcParams p = ComputeNvencRcParams(RateControlMode::VariableBitrate, NvencQualityPreset::Balanced, 20000);
    EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_VBR));
}

TEST(ComputeNvencRcParams, VariableBitrate_AverageBitRate) {
    // averageBitRate must be bitrate_kbps * 1000 (bps)
    const RcParams p = ComputeNvencRcParams(RateControlMode::VariableBitrate, NvencQualityPreset::Balanced, 20000);
    EXPECT_EQ(p.averageBitRate, 20000u * 1000u);
}

TEST(ComputeNvencRcParams, VariableBitrate_MaxBitRateIs1_5x) {
    // maxBitRate must be averageBitRate * 3/2
    const RcParams p = ComputeNvencRcParams(RateControlMode::VariableBitrate, NvencQualityPreset::Balanced, 20000);
    EXPECT_EQ(p.maxBitRate, 20000u * 1000u * 3u / 2u);
}

TEST(ComputeNvencRcParams, VariableBitrate_QPFieldsAreZero) {
    const RcParams p = ComputeNvencRcParams(RateControlMode::VariableBitrate, NvencQualityPreset::Balanced, 10000);
    EXPECT_EQ(p.qpIntra, 0u);
    EXPECT_EQ(p.qpInterP, 0u);
    EXPECT_EQ(p.qpInterB, 0u);
}

// ---------------------------------------------------------------------------
// ConstantBitrate — CBR, strict bitrate
// ---------------------------------------------------------------------------

TEST(ComputeNvencRcParams, ConstantBitrate_ModeIsCBR) {
    const RcParams p = ComputeNvencRcParams(RateControlMode::ConstantBitrate, NvencQualityPreset::Balanced, 10000);
    EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_CBR));
}

TEST(ComputeNvencRcParams, ConstantBitrate_AverageEqualMax) {
    // For CBR: averageBitRate == maxBitRate
    const RcParams p = ComputeNvencRcParams(RateControlMode::ConstantBitrate, NvencQualityPreset::Balanced, 10000);
    EXPECT_EQ(p.averageBitRate, 10000u * 1000u);
    EXPECT_EQ(p.maxBitRate, 10000u * 1000u);
    EXPECT_EQ(p.averageBitRate, p.maxBitRate);
}

// ---------------------------------------------------------------------------
// Lossless — not implemented; falls back to ConstantQuality/Balanced
// ---------------------------------------------------------------------------

TEST(ComputeNvencRcParams, Lossless_FallsBackToConstantQuality) {
    const RcParams p = ComputeNvencRcParams(RateControlMode::Lossless, NvencQualityPreset::High, 20000);
    // Must fall back to CQP, not pass through
    EXPECT_EQ(p.rateControlMode, static_cast<uint32_t>(NV_ENC_PARAMS_RC_CONSTQP));
}

} // namespace recorder_core
