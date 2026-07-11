#include "nvenc_encoder.h"

#include <vector>

#include <gtest/gtest.h>

// Tests for the pure, GPU-free GOP + AQ helpers backing the keyframe-interval
// selector and the explicit adaptive-quantization setting:
//   ComputeGopLength      — round(interval_secs * fps) with a degenerate-fps fallback
//   ApplyGopToNvenc       — gopLength + codec-specific idrPeriod, kept consistent
//   ApplySpatialAqToNvenc — enableAQ=1, enableTemporalAQ=0, aqStrength=0
//   NextGopKeyframePhase  — deterministic IDR predictor with non-default GOP
//
// NVENC SDK fields under test (NV_ENC_CONFIG):
//   gopLength
//   encodeCodecConfig.{h264Config,hevcConfig,av1Config}.idrPeriod
//   rcParams.{enableAQ, enableTemporalAQ, aqStrength}

namespace recorder_core {

// ---------------------------------------------------------------------------
// ComputeGopLength — interval X @ fps -> gopLength (round-to-nearest)
// ---------------------------------------------------------------------------

TEST(ComputeGopLength, Default2s60fps_Is120) {
    EXPECT_EQ(ComputeGopLength(2.0f, 60, 1), 120u);
}

TEST(ComputeGopLength, OneSecond60fps_Is60) {
    EXPECT_EQ(ComputeGopLength(1.0f, 60, 1), 60u);
}

TEST(ComputeGopLength, HalfSecond60fps_Is30) {
    EXPECT_EQ(ComputeGopLength(0.5f, 60, 1), 30u);
}

TEST(ComputeGopLength, Default2s30fps_Is60) {
    EXPECT_EQ(ComputeGopLength(2.0f, 30, 1), 60u);
}

TEST(ComputeGopLength, FractionalFrameRate_5994_RoundsToNearest) {
    // 60000/1001 ≈ 59.94 fps; 2 s -> round(119.88) = 120.
    EXPECT_EQ(ComputeGopLength(2.0f, 60000, 1001), 120u);
    // 0.5 s -> round(29.97) = 30.
    EXPECT_EQ(ComputeGopLength(0.5f, 60000, 1001), 30u);
}

TEST(ComputeGopLength, DegenerateFrameRate_FallsBackTo120) {
    EXPECT_EQ(ComputeGopLength(2.0f, 0, 1), 120u);
    EXPECT_EQ(ComputeGopLength(2.0f, 60, 0), 120u);
}

TEST(ComputeGopLength, NonPositiveInterval_TreatedAs2s) {
    EXPECT_EQ(ComputeGopLength(0.0f, 60, 1), 120u);
    EXPECT_EQ(ComputeGopLength(-1.0f, 60, 1), 120u);
}

TEST(ComputeGopLength, NeverReturnsZero) {
    // A tiny interval that would round to 0 is clamped to 1.
    EXPECT_EQ(ComputeGopLength(0.001f, 60, 1), 1u);
}

// ---------------------------------------------------------------------------
// ApplyGopToNvenc — gopLength + per-codec idrPeriod stay consistent
// ---------------------------------------------------------------------------

TEST(ApplyGopToNvenc, H264_SetsGopLengthAndIdrPeriod) {
    NV_ENC_CONFIG cfg{};
    ApplyGopToNvenc(cfg, VideoCodec::H264Nvenc, 60u);
    EXPECT_EQ(cfg.gopLength, 60u);
    EXPECT_EQ(cfg.encodeCodecConfig.h264Config.idrPeriod, 60u);
}

TEST(ApplyGopToNvenc, Hevc_SetsGopLengthAndIdrPeriod) {
    NV_ENC_CONFIG cfg{};
    ApplyGopToNvenc(cfg, VideoCodec::HevcNvenc, 30u);
    EXPECT_EQ(cfg.gopLength, 30u);
    EXPECT_EQ(cfg.encodeCodecConfig.hevcConfig.idrPeriod, 30u);
}

TEST(ApplyGopToNvenc, Av1_SetsGopLengthAndIdrPeriod) {
    NV_ENC_CONFIG cfg{};
    ApplyGopToNvenc(cfg, VideoCodec::Av1Nvenc, 120u);
    EXPECT_EQ(cfg.gopLength, 120u);
    EXPECT_EQ(cfg.encodeCodecConfig.av1Config.idrPeriod, 120u);
}

TEST(ApplyGopToNvenc, IdrPeriodEqualsGopLengthForEveryCodec) {
    for (const VideoCodec codec : {VideoCodec::H264Nvenc, VideoCodec::HevcNvenc, VideoCodec::Av1Nvenc}) {
        NV_ENC_CONFIG cfg{};
        ApplyGopToNvenc(cfg, codec, 45u);
        EXPECT_EQ(cfg.gopLength, 45u);
        const uint32_t idr = (codec == VideoCodec::H264Nvenc)   ? cfg.encodeCodecConfig.h264Config.idrPeriod
                             : (codec == VideoCodec::HevcNvenc) ? cfg.encodeCodecConfig.hevcConfig.idrPeriod
                                                                : cfg.encodeCodecConfig.av1Config.idrPeriod;
        EXPECT_EQ(idr, cfg.gopLength);
    }
}

// ---------------------------------------------------------------------------
// ApplySpatialAqToNvenc — spatial AQ on, temporal off, auto strength
// ---------------------------------------------------------------------------

TEST(ApplySpatialAqToNvenc, EnablesSpatialAqOnly) {
    NV_ENC_CONFIG cfg{};
    ApplySpatialAqToNvenc(cfg);
    EXPECT_EQ(cfg.rcParams.enableAQ, 1u) << "spatial AQ must be explicitly enabled";
    EXPECT_EQ(cfg.rcParams.enableTemporalAQ, 0u) << "temporal AQ must stay off (no lookahead)";
    EXPECT_EQ(cfg.rcParams.aqStrength, 0u) << "aqStrength 0 keeps driver auto-selection";
}

TEST(ApplySpatialAqToNvenc, OverridesInheritedTemporalAqAndStrength) {
    // Simulate a preset config that arrived with temporal AQ / a strength set;
    // the explicit apply must pin it back to the deterministic spatial-only state.
    NV_ENC_CONFIG cfg{};
    cfg.rcParams.enableAQ = 0;
    cfg.rcParams.enableTemporalAQ = 1;
    cfg.rcParams.aqStrength = 8;
    ApplySpatialAqToNvenc(cfg);
    EXPECT_EQ(cfg.rcParams.enableAQ, 1u);
    EXPECT_EQ(cfg.rcParams.enableTemporalAQ, 0u);
    EXPECT_EQ(cfg.rcParams.aqStrength, 0u);
}

// ---------------------------------------------------------------------------
// NextGopKeyframePhase — deterministic IDR cadence, incl. non-default GOP
// ---------------------------------------------------------------------------

// Drive the predictor across `frames` submissions with the given gopLength and
// collect the 0-based submission indices that come back flagged as keyframes.
static std::vector<uint32_t> KeyframeIndices(uint32_t gop_length, uint32_t frames) {
    std::vector<uint32_t> keys;
    uint32_t frame_in_gop = 0; // session starts at phase 0 (first frame is always an IDR)
    for (uint32_t i = 0; i < frames; ++i) {
        const GopKeyframePhase phase = NextGopKeyframePhase(frame_in_gop, gop_length, /*forced_idr=*/false);
        if (phase.is_keyframe)
            keys.push_back(i);
        frame_in_gop = phase.frame_in_gop;
    }
    return keys;
}

TEST(NextGopKeyframePhase, FirstFrameIsAlwaysKeyframe) {
    const GopKeyframePhase phase = NextGopKeyframePhase(0u, 60u, false);
    EXPECT_TRUE(phase.is_keyframe);
    EXPECT_EQ(phase.frame_in_gop, 1u);
}

TEST(NextGopKeyframePhase, DefaultGop120_KeyframesEvery120) {
    const std::vector<uint32_t> keys = KeyframeIndices(120u, 361);
    EXPECT_EQ(keys, (std::vector<uint32_t>{0u, 120u, 240u, 360u}));
}

TEST(NextGopKeyframePhase, NonDefaultGop30_KeyframesEvery30) {
    const std::vector<uint32_t> keys = KeyframeIndices(30u, 121);
    EXPECT_EQ(keys, (std::vector<uint32_t>{0u, 30u, 60u, 90u, 120u}));
}

TEST(NextGopKeyframePhase, NonDefaultGop60_KeyframesEvery60) {
    const std::vector<uint32_t> keys = KeyframeIndices(60u, 181);
    EXPECT_EQ(keys, (std::vector<uint32_t>{0u, 60u, 120u, 180u}));
}

TEST(NextGopKeyframePhase, ForcedIdrResetsPhase) {
    // GOP 60: run to submission index 20, force an IDR there, then the next IDR
    // must land 60 frames later (index 80), not at the original-cadence 60.
    uint32_t frame_in_gop = 0;
    std::vector<uint32_t> keys;
    for (uint32_t i = 0; i < 141; ++i) {
        const bool force = (i == 20u);
        const GopKeyframePhase phase = NextGopKeyframePhase(frame_in_gop, 60u, force);
        if (phase.is_keyframe)
            keys.push_back(i);
        frame_in_gop = phase.frame_in_gop;
    }
    EXPECT_EQ(keys, (std::vector<uint32_t>{0u, 20u, 80u, 140u}));
}

TEST(NextGopKeyframePhase, ZeroGopLength_OnlyFirstFrameIsKeyframe) {
    // Defensive: gopLength 0 (never produced by ComputeGopLength) means only the
    // phase-0 first frame is an IDR; the counter climbs without wrapping.
    const std::vector<uint32_t> keys = KeyframeIndices(0u, 200);
    EXPECT_EQ(keys, (std::vector<uint32_t>{0u}));
}

} // namespace recorder_core
