#include "nvenc_encoder.h"

#include <vector>

#include <gtest/gtest.h>

// Tests for the pure, GPU-free GOP + AQ helpers backing the keyframe-interval
// selector and the explicit adaptive-quantization setting:
//   ComputeGopLength        — round(interval_secs * fps) with a degenerate-fps fallback
//   ApplyGopToNvenc         — gopLength + codec-specific idrPeriod, kept consistent
//   ComputeNvencGopBackstop — the frame-count backstop actually programmed into NVENC
//   ApplyAdaptiveQuantizationToNvenc — enableAQ=0, enableTemporalAQ=0, aqStrength=0
//   ComputeFrameIntervalNs  — nominal frame duration feeding the media-time cadence
//   NextGopKeyframePhase    — media-time IDR cadence, robust against CFR timeline
//                             gaps that never reach the encoder
//
// NVENC SDK fields under test (NV_ENC_CONFIG):
//   gopLength
//   encodeCodecConfig.{h264Config,hevcConfig,av1Config}.idrPeriod
//   rcParams.{enableAQ, enableTemporalAQ, aqStrength}

namespace exosnap::engine {

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
// ComputeNvencGopBackstop — the frame counter NVENC itself runs must never be
// able to fire an IDR before the media-time cadence does
// ---------------------------------------------------------------------------

TEST(ComputeNvencGopBackstop, Cfr_KeepsTheFrameCountGop) {
    // CFR submits at most one frame per media-time frame interval, so the
    // submission counter can only reach gop_length at or after the media-time
    // boundary — the backstop stays armed with the exact GOP length.
    EXPECT_EQ(ComputeNvencGopBackstop(120u, /*constant_frame_rate=*/true), 120u);
    EXPECT_EQ(ComputeNvencGopBackstop(30u, /*constant_frame_rate=*/true), 30u);
}

TEST(ComputeNvencGopBackstop, Vfr_DisablesTheFrameCountBackstop) {
    // VFR passes source timestamps through and submits every frame the source
    // produces. A 144 Hz source recorded at a 60 fps profile reaches 120
    // submissions after ~0.83 s of media time — long before the 2 s media-time
    // boundary — so a frame counter would insert an IDR the submission side
    // never predicted. Keyframes are enforced per submission with FORCEIDR, so
    // the counter is disabled rather than guessed at.
    EXPECT_EQ(ComputeNvencGopBackstop(120u, /*constant_frame_rate=*/false), NVENC_INFINITE_GOPLENGTH);
    EXPECT_EQ(ComputeNvencGopBackstop(30u, /*constant_frame_rate=*/false), NVENC_INFINITE_GOPLENGTH);
}

TEST(ApplyGopToNvenc, VfrBackstopReachesEveryCodecsIdrPeriod) {
    // The disabled backstop must land in gopLength AND in the codec's own
    // idrPeriod: leaving gopLength finite would still insert (non-IDR) I-frames
    // on a frame counter, which the muxer would index as seek points.
    for (const VideoCodec codec : {VideoCodec::H264, VideoCodec::Hevc, VideoCodec::Av1}) {
        NV_ENC_CONFIG cfg{};
        ApplyGopToNvenc(cfg, codec, ComputeNvencGopBackstop(120u, /*constant_frame_rate=*/false));
        EXPECT_EQ(cfg.gopLength, NVENC_INFINITE_GOPLENGTH);
        const uint32_t idr = (codec == VideoCodec::H264)   ? cfg.encodeCodecConfig.h264Config.idrPeriod
                             : (codec == VideoCodec::Hevc) ? cfg.encodeCodecConfig.hevcConfig.idrPeriod
                                                           : cfg.encodeCodecConfig.av1Config.idrPeriod;
        EXPECT_EQ(idr, NVENC_INFINITE_GOPLENGTH);
    }
}

// ---------------------------------------------------------------------------
// ApplyAdaptiveQuantizationToNvenc — both AQ flavours pinned off
// ---------------------------------------------------------------------------

TEST(ApplyAdaptiveQuantizationToNvenc, DisablesBothAqFlavours) {
    NV_ENC_CONFIG cfg{};
    ApplyAdaptiveQuantizationToNvenc(cfg);
    EXPECT_EQ(cfg.rcParams.enableAQ, 0u) << "spatial AQ is net-negative under CONSTQP";
    EXPECT_EQ(cfg.rcParams.enableTemporalAQ, 0u) << "temporal AQ is capability-gated and needs lookahead";
    EXPECT_EQ(cfg.rcParams.aqStrength, 0u) << "no strength is implied while AQ is off";
}

TEST(ApplyAdaptiveQuantizationToNvenc, OverridesInheritedAqState) {
    // Simulate a preset config that arrived with AQ / temporal AQ / a strength
    // set; the explicit apply must pin it back to the deterministic off state
    // rather than letting a driver default decide.
    NV_ENC_CONFIG cfg{};
    cfg.rcParams.enableAQ = 1;
    cfg.rcParams.enableTemporalAQ = 1;
    cfg.rcParams.aqStrength = 8;
    ApplyAdaptiveQuantizationToNvenc(cfg);
    EXPECT_EQ(cfg.rcParams.enableAQ, 0u);
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

// Same drive loop, but the PTS grid and the encoder's frame interval come from
// DIFFERENT formulas — which is the production situation, see
// ProductionCfrPtsStepNs. Returns the tick indices flagged as keyframes.
std::vector<uint64_t> KeyframeTicksWithPtsStep(uint64_t tick_count, uint32_t gop_length, uint64_t pts_step_ns,
                                               uint64_t encoder_interval_ns) {
    std::vector<uint64_t> keys;
    bool have_start = false;
    uint64_t gop_start = 0;
    const uint64_t gop_duration = encoder_interval_ns * gop_length;
    for (uint64_t tick = 0; tick < tick_count; ++tick) {
        const GopKeyframePhase phase = NextGopKeyframePhase(tick * pts_step_ns, have_start, gop_start, gop_duration,
                                                            encoder_interval_ns, /*forced_idr=*/false);
        if (phase.is_keyframe)
            keys.push_back(tick);
        gop_start = phase.gop_start_pts_ns;
        have_start = have_start || phase.is_keyframe;
    }
    return keys;
}

// The CFR scheduler's PTS step (video_thread.cpp): frame_interval_100ns is
// 1e7 * den / num truncated to whole 100 ns units, and PTS is tick index times
// that. It is NOT ComputeFrameIntervalNs — 16 666 600 ns vs 16 666 666 ns at
// 60 fps — and deliberately so: changing either formula changes either every
// muxed timestamp or the cadence tolerance, so they are pinned independently
// and the tests below pin the contract BETWEEN them.
uint64_t ProductionCfrPtsStepNs(uint32_t frame_rate_num, uint32_t frame_rate_den) {
    return (10000000ull * frame_rate_den / frame_rate_num) * 100ull;
}

std::vector<uint64_t> EveryNthUpTo(uint64_t step, uint64_t last) {
    std::vector<uint64_t> v;
    for (uint64_t i = 0; i <= last; i += step)
        v.push_back(i);
    return v;
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
// Production PTS grid vs. the encoder's frame interval. The CFR scheduler's PTS
// step and ComputeFrameIntervalNs are two different formulas over the same rate,
// so every frame's PTS runs a few ns short of the cadence's own arithmetic. The
// tolerance absorbs that, and — because each keyframe re-anchors at its own real
// PTS — the per-GOP deficit must not accumulate across GOPs.
// ---------------------------------------------------------------------------

TEST(NextGopKeyframePhase, ProductionPtsStepDiffersFromTheEncoderFrameInterval) {
    // Guard the premise of the tests below: if these ever converge, the cadence
    // is being fed its own grid and the tolerance is no longer being exercised.
    EXPECT_EQ(ProductionCfrPtsStepNs(60, 1), 16666600ull);
    EXPECT_EQ(ComputeFrameIntervalNs(60, 1), 16666666ull);
    EXPECT_NE(ProductionCfrPtsStepNs(60, 1), ComputeFrameIntervalNs(60, 1));
}

TEST(NextGopKeyframePhase, ProductionPtsGrid60fps_DeficitDoesNotAccumulate) {
    // 60 s at 60 fps, 2 s GOP: 30 keyframe boundaries. The per-GOP deficit is
    // 120 * 66 ns ≈ 7.9 µs against a half-frame tolerance of ≈ 8.3 ms, and
    // re-anchoring at the real PTS keeps it per-GOP instead of cumulative.
    const std::vector<uint64_t> keys =
        KeyframeTicksWithPtsStep(3601, 120u, ProductionCfrPtsStepNs(60, 1), ComputeFrameIntervalNs(60, 1));
    EXPECT_EQ(keys, EveryNthUpTo(120u, 3600u));
}

TEST(NextGopKeyframePhase, ProductionPtsGrid60fps_HalfSecondGop_DeficitDoesNotAccumulate) {
    // Shortest selectable keyframe interval: 4x as many boundaries in the same
    // wall time, so 4x as many chances for a rounding deficit to compound.
    const std::vector<uint64_t> keys =
        KeyframeTicksWithPtsStep(3601, 30u, ProductionCfrPtsStepNs(60, 1), ComputeFrameIntervalNs(60, 1));
    EXPECT_EQ(keys, EveryNthUpTo(30u, 3600u));
}

TEST(NextGopKeyframePhase, ProductionPtsGrid30fps_DeficitDoesNotAccumulate) {
    // 30 fps: 33 333 300 ns per tick vs the encoder's 33 333 333 ns.
    const std::vector<uint64_t> keys =
        KeyframeTicksWithPtsStep(1801, 60u, ProductionCfrPtsStepNs(30, 1), ComputeFrameIntervalNs(30, 1));
    EXPECT_EQ(keys, EveryNthUpTo(60u, 1800u));
}

TEST(NextGopKeyframePhase, ProductionPtsGrid5994_DeficitDoesNotAccumulate) {
    // Fractional rate, where both formulas truncate: 16 683 300 ns per tick vs
    // the encoder's 16 683 333 ns.
    const std::vector<uint64_t> keys =
        KeyframeTicksWithPtsStep(3601, 120u, ProductionCfrPtsStepNs(60000, 1001), ComputeFrameIntervalNs(60000, 1001));
    EXPECT_EQ(keys, EveryNthUpTo(120u, 3600u));
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

} // namespace exosnap::engine
