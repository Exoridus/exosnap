#include "nvenc_encoder.h"

#include <vector>

#include <gtest/gtest.h>

// Tests for the pure, GPU-free GOP + AQ helpers backing the keyframe-interval
// selector and the explicit adaptive-quantization setting:
//   ComputeGopLength       — round(interval_secs * fps) with a degenerate-fps fallback
//   ApplyGopToNvenc        — gopLength + codec-specific idrPeriod, kept consistent
//   ApplySpatialAqToNvenc  — enableAQ=1, enableTemporalAQ=0, aqStrength=0
//   ComputeFrameIntervalNs — nominal frame duration feeding the media-time cadence
//   NextGopKeyframePhase   — media-time IDR cadence, robust against CFR timeline
//                            gaps that never reach the encoder
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
    ApplyGopToNvenc(cfg, VideoCodec::H264, 60u);
    EXPECT_EQ(cfg.gopLength, 60u);
    EXPECT_EQ(cfg.encodeCodecConfig.h264Config.idrPeriod, 60u);
}

TEST(ApplyGopToNvenc, Hevc_SetsGopLengthAndIdrPeriod) {
    NV_ENC_CONFIG cfg{};
    ApplyGopToNvenc(cfg, VideoCodec::Hevc, 30u);
    EXPECT_EQ(cfg.gopLength, 30u);
    EXPECT_EQ(cfg.encodeCodecConfig.hevcConfig.idrPeriod, 30u);
}

TEST(ApplyGopToNvenc, Av1_SetsGopLengthAndIdrPeriod) {
    NV_ENC_CONFIG cfg{};
    ApplyGopToNvenc(cfg, VideoCodec::Av1, 120u);
    EXPECT_EQ(cfg.gopLength, 120u);
    EXPECT_EQ(cfg.encodeCodecConfig.av1Config.idrPeriod, 120u);
}

TEST(ApplyGopToNvenc, IdrPeriodEqualsGopLengthForEveryCodec) {
    for (const VideoCodec codec : {VideoCodec::H264, VideoCodec::Hevc, VideoCodec::Av1}) {
        NV_ENC_CONFIG cfg{};
        ApplyGopToNvenc(cfg, codec, 45u);
        EXPECT_EQ(cfg.gopLength, 45u);
        const uint32_t idr = (codec == VideoCodec::H264)   ? cfg.encodeCodecConfig.h264Config.idrPeriod
                             : (codec == VideoCodec::Hevc) ? cfg.encodeCodecConfig.hevcConfig.idrPeriod
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
// ComputeFrameIntervalNs — nominal frame duration for the PTS-based cadence
// ---------------------------------------------------------------------------

TEST(ComputeFrameIntervalNs, SixtyFps_Is16666666ns) {
    EXPECT_EQ(ComputeFrameIntervalNs(60, 1), 16666666ull);
}

TEST(ComputeFrameIntervalNs, ThirtyFps_Is33333333ns) {
    EXPECT_EQ(ComputeFrameIntervalNs(30, 1), 33333333ull);
}

TEST(ComputeFrameIntervalNs, FractionalFrameRate_5994) {
    // 60000/1001: 1e9 * 1001 / 60000 = 16683333 (truncated).
    EXPECT_EQ(ComputeFrameIntervalNs(60000, 1001), 16683333ull);
}

TEST(ComputeFrameIntervalNs, DegenerateFrameRate_FallsBackTo60Fps) {
    EXPECT_EQ(ComputeFrameIntervalNs(0, 1), 16666666ull);
    EXPECT_EQ(ComputeFrameIntervalNs(60, 0), 16666666ull);
}

// ---------------------------------------------------------------------------
// NextGopKeyframePhase — media-time IDR cadence, incl. timeline gaps
// ---------------------------------------------------------------------------

namespace {

// Drives the cadence like EncodeFrame does: each submitted tick index maps to
// pts = index * interval; the anchor state threads through the calls. Returns
// the tick indices flagged as keyframes. `submitted` lists the CFR tick
// indices that actually reach the encoder — gaps model ticks where the
// scheduler advanced the timeline without submitting a frame (composite-error
// drop, sustained-lag resync skip).
std::vector<uint64_t> KeyframeTicks(const std::vector<uint64_t>& submitted, uint32_t gop_length, uint64_t interval_ns) {
    std::vector<uint64_t> keys;
    bool have_start = false;
    uint64_t gop_start = 0;
    const uint64_t gop_duration = interval_ns * gop_length;
    for (const uint64_t tick : submitted) {
        const GopKeyframePhase phase = NextGopKeyframePhase(tick * interval_ns, have_start, gop_start, gop_duration,
                                                            interval_ns, /*forced_idr=*/false);
        if (phase.is_keyframe)
            keys.push_back(tick);
        gop_start = phase.gop_start_pts_ns;
        have_start = have_start || phase.is_keyframe;
    }
    return keys;
}

std::vector<uint64_t> ContiguousTicks(uint64_t count) {
    std::vector<uint64_t> t(count);
    for (uint64_t i = 0; i < count; ++i)
        t[i] = i;
    return t;
}

constexpr uint64_t kInterval60 = 16666666ull; // ComputeFrameIntervalNs(60, 1)

} // namespace

TEST(NextGopKeyframePhase, FirstFrameIsAlwaysKeyframe) {
    const GopKeyframePhase phase = NextGopKeyframePhase(/*pts_ns=*/0u, /*have_gop_start=*/false,
                                                        /*gop_start_pts_ns=*/0u, /*gop_duration_ns=*/60u * kInterval60,
                                                        kInterval60, /*forced_idr=*/false);
    EXPECT_TRUE(phase.is_keyframe);
    EXPECT_EQ(phase.gop_start_pts_ns, 0u);
}

TEST(NextGopKeyframePhase, DefaultGop120_KeyframesEvery120) {
    const std::vector<uint64_t> keys = KeyframeTicks(ContiguousTicks(361), 120u, kInterval60);
    EXPECT_EQ(keys, (std::vector<uint64_t>{0u, 120u, 240u, 360u}));
}

TEST(NextGopKeyframePhase, NonDefaultGop30_KeyframesEvery30) {
    const std::vector<uint64_t> keys = KeyframeTicks(ContiguousTicks(121), 30u, kInterval60);
    EXPECT_EQ(keys, (std::vector<uint64_t>{0u, 30u, 60u, 90u, 120u}));
}

TEST(NextGopKeyframePhase, NonDefaultGop60_KeyframesEvery60) {
    const std::vector<uint64_t> keys = KeyframeTicks(ContiguousTicks(181), 60u, kInterval60);
    EXPECT_EQ(keys, (std::vector<uint64_t>{0u, 60u, 120u, 180u}));
}

TEST(NextGopKeyframePhase, FractionalFrameRate_5994_StaysOnFrameCadence) {
    // 59.94 fps: pts rounding (truncated interval) must not slip the boundary
    // frame past the threshold — the half-frame tolerance absorbs it.
    const std::vector<uint64_t> keys = KeyframeTicks(ContiguousTicks(361), 120u, ComputeFrameIntervalNs(60000, 1001));
    EXPECT_EQ(keys, (std::vector<uint64_t>{0u, 120u, 240u, 360u}));
}

TEST(NextGopKeyframePhase, DroppedTickDoesNotStretchTheKeyframeInterval) {
    // Composite-error drop: CFR tick 119 advances the timeline without
    // submitting a frame. The next keyframe must land on the first submitted
    // frame at/after the media-time boundary (tick 120) — a submitted-frame
    // counter would have pushed it to tick 121.
    std::vector<uint64_t> submitted;
    for (uint64_t i = 0; i < 361; ++i) {
        if (i != 119u)
            submitted.push_back(i);
    }
    const std::vector<uint64_t> keys = KeyframeTicks(submitted, 120u, kInterval60);
    EXPECT_EQ(keys, (std::vector<uint64_t>{0u, 120u, 240u, 360u}));
}

TEST(NextGopKeyframePhase, SustainedLagResyncGapTriggersKeyframeAtOrAfterBoundary) {
    // Sustained-lag resync: 90 ticks (1.5 s @ 60 fps) are skipped mid-GOP
    // (ticks 60..149 never reach the encoder). The first frame after the gap
    // (tick 150) is past the 2 s boundary and must be a keyframe; a
    // submitted-frame counter would have waited another 60 submissions
    // (media time 3.5 s -> a 3.5 s keyframe gap).
    std::vector<uint64_t> submitted;
    for (uint64_t i = 0; i < 60; ++i)
        submitted.push_back(i);
    for (uint64_t i = 150; i < 400; ++i)
        submitted.push_back(i);
    const std::vector<uint64_t> keys = KeyframeTicks(submitted, 120u, kInterval60);
    // Re-anchored at 150; next boundaries 270, 390.
    EXPECT_EQ(keys, (std::vector<uint64_t>{0u, 150u, 270u, 390u}));
}

TEST(NextGopKeyframePhase, ForcedIdrReanchorsTheGop) {
    // GOP 60: force an IDR at tick 20, then the next IDR must land 60 frames
    // of media time later (tick 80), not at the original-cadence 60.
    std::vector<uint64_t> keys;
    bool have_start = false;
    uint64_t gop_start = 0;
    const uint64_t gop_duration = 60u * kInterval60;
    for (uint64_t i = 0; i < 141; ++i) {
        const bool force = (i == 20u);
        const GopKeyframePhase phase =
            NextGopKeyframePhase(i * kInterval60, have_start, gop_start, gop_duration, kInterval60, force);
        if (phase.is_keyframe)
            keys.push_back(i);
        gop_start = phase.gop_start_pts_ns;
        have_start = have_start || phase.is_keyframe;
    }
    EXPECT_EQ(keys, (std::vector<uint64_t>{0u, 20u, 80u, 140u}));
}

TEST(NextGopKeyframePhase, ZeroGopDuration_OnlyFirstFrameIsKeyframe) {
    // Defensive: gop duration 0 (never produced by InitEncoder) means only the
    // stream-opening frame is an IDR.
    const std::vector<uint64_t> keys = KeyframeTicks(ContiguousTicks(200), 0u, kInterval60);
    EXPECT_EQ(keys, (std::vector<uint64_t>{0u}));
}

// ---------------------------------------------------------------------------
// ResyncGopStartFromActual — order/keyframe hardening (warn-first)
// ---------------------------------------------------------------------------

TEST(ResyncGopStartFromActual, ConfirmedIdr_LeavesAheadAnchorUntouched) {
    // Async buffering: the submission-side anchor is already several frames
    // ahead of the packet being consumed. A confirmed prediction must NOT
    // rewind it — doing so stretched every GOP by the in-flight depth
    // (~13 % at 0.5 s / 60 fps).
    EXPECT_EQ(ResyncGopStartFromActual(/*predicted_keyframe=*/true, /*actual_is_idr=*/true,
                                       /*packet_pts_ns=*/1900000000ull, /*gop_start_pts_ns=*/2000000000ull),
              2000000000ull);
}

TEST(ResyncGopStartFromActual, UnpredictedActualIdr_ReanchorsAtPacketPts) {
    // Emergency self-healing: NVENC emitted an IDR we did not force. The
    // submission-side cadence is provably wrong, so the observed IDR's media
    // time becomes the anchor.
    EXPECT_EQ(ResyncGopStartFromActual(/*predicted_keyframe=*/false, /*actual_is_idr=*/true,
                                       /*packet_pts_ns=*/1500000000ull, /*gop_start_pts_ns=*/400000000ull),
              1500000000ull);
}

TEST(ResyncGopStartFromActual, UnpredictedActualIdr_NeverRewindsPastNewerAnchor) {
    // A delayed completion must not rewind an anchor that a newer
    // submission-side keyframe already set (same delayed-viewpoint hazard as
    // the confirmed case).
    EXPECT_EQ(ResyncGopStartFromActual(/*predicted_keyframe=*/false, /*actual_is_idr=*/true,
                                       /*packet_pts_ns=*/1500000000ull, /*gop_start_pts_ns=*/2000000000ull),
              2000000000ull);
}

TEST(ResyncGopStartFromActual, PredictedIdrMissing_LeavesAnchorUnchanged) {
    // Warn-only: a forced IDR that did not materialize is logged/counted, but a
    // single miss is not evidence the whole cadence has shifted.
    EXPECT_EQ(ResyncGopStartFromActual(/*predicted_keyframe=*/true, /*actual_is_idr=*/false,
                                       /*packet_pts_ns=*/1000000000ull, /*gop_start_pts_ns=*/900000000ull),
              900000000ull);
}

TEST(ResyncGopStartFromActual, ConfirmedNonIdr_LeavesAnchorUnchanged) {
    EXPECT_EQ(ResyncGopStartFromActual(/*predicted_keyframe=*/false, /*actual_is_idr=*/false,
                                       /*packet_pts_ns=*/1000000000ull, /*gop_start_pts_ns=*/900000000ull),
              900000000ull);
}

// ---------------------------------------------------------------------------
// Order-validation mismatch message formatters — pure text, no GPU/NVENC session
// ---------------------------------------------------------------------------

TEST(FormatOutputTsMismatchError, IncludesBothTimestampValues) {
    const std::string msg = FormatOutputTsMismatchError(/*expected=*/42u, /*actual=*/43u);
    EXPECT_NE(msg.find("42"), std::string::npos);
    EXPECT_NE(msg.find("43"), std::string::npos);
}

TEST(FormatOutputTsMismatchError, DistinguishesExpectedFromActualValue) {
    // Regression guard: swapping the arguments must change the rendered text,
    // otherwise a future refactor could silently transpose expected/actual.
    const std::string a = FormatOutputTsMismatchError(1u, 2u);
    const std::string b = FormatOutputTsMismatchError(2u, 1u);
    EXPECT_NE(a, b);
}

TEST(FormatKeyframePredictionMismatchWarning, IncludesBothBooleanValues) {
    const std::string predictedTrue = FormatKeyframePredictionMismatchWarning(/*predicted=*/true, /*actual=*/false);
    const std::string predictedFalse = FormatKeyframePredictionMismatchWarning(/*predicted=*/false, /*actual=*/true);
    EXPECT_NE(predictedTrue, predictedFalse);
}

} // namespace recorder_core
