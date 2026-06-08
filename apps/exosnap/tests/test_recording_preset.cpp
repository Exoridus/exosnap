#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <unordered_set>

#include <capability/audio_ui_state.h>
#include <recorder_core/audio_track_model.h>
#include <recorder_core/recorder_session.h>

#include "models/RecordingPreset.h"

namespace exosnap {
namespace {

// ===========================================================================
// Helpers
// ===========================================================================

// Returns a copy of `cfg` with the PiP overlay shifted by `delta` on x and y.
RecordingPresetConfig WithPipDelta(RecordingPresetConfig cfg, float delta) {
    cfg.webcam.overlay.x_norm += delta;
    cfg.webcam.overlay.y_norm += delta;
    return cfg;
}

// Returns a copy of `cfg` with `mic_gain_linear` offset by `delta`.
RecordingPresetConfig WithMicGainDelta(RecordingPresetConfig cfg, float delta) {
    cfg.audio.mic_gain_linear += delta;
    return cfg;
}

// ===========================================================================
// MakeDefaultPreset — exact canonical values
// ===========================================================================

TEST(RecordingPreset, DefaultPreset_IdAndName) {
    const RecordingPreset p = MakeDefaultPreset();
    EXPECT_EQ(p.id, std::string(kDefaultPresetId));
    EXPECT_EQ(p.id, "preset.default");
    EXPECT_EQ(p.name, "Default");
}

TEST(RecordingPreset, DefaultPreset_Container_Video_Audio) {
    const RecordingPreset p = MakeDefaultPreset();
    EXPECT_EQ(p.config.output.container, capability::Container::Matroska);
    EXPECT_EQ(p.config.output.video_codec, capability::VideoCodec::Av1Nvenc);
    EXPECT_EQ(p.config.output.audio_codec, capability::AudioCodec::Opus);
}

TEST(RecordingPreset, DefaultPreset_VideoSettings) {
    const RecordingPreset p = MakeDefaultPreset();
    EXPECT_EQ(p.config.video.quality, recorder_core::NvencQualityPreset::High);
    EXPECT_TRUE(p.config.video.cfr);
    EXPECT_TRUE(p.config.video.capture_cursor);
    EXPECT_EQ(p.config.video.frame_rate_num, 60u);
    EXPECT_EQ(p.config.video.frame_rate_den, 1u);
}

TEST(RecordingPreset, DefaultPreset_Countdown) {
    const RecordingPreset p = MakeDefaultPreset();
    EXPECT_EQ(p.config.countdown_seconds, 0);
}

TEST(RecordingPreset, DefaultPreset_CaptureKind) {
    const RecordingPreset p = MakeDefaultPreset();
    EXPECT_EQ(p.config.capture.kind, PresetCaptureKind::Display);
    EXPECT_TRUE(p.config.capture.display_key.empty());
    EXPECT_TRUE(p.config.capture.window_key.empty());
    EXPECT_FALSE(p.config.capture.has_region);
}

TEST(RecordingPreset, DefaultPreset_Webcam) {
    const RecordingPreset p = MakeDefaultPreset();
    EXPECT_FALSE(p.config.webcam.enabled);
    EXPECT_FALSE(p.config.webcam.mirror);
    EXPECT_EQ(p.config.webcam.width, 1280);
    EXPECT_EQ(p.config.webcam.height, 720);
    EXPECT_EQ(p.config.webcam.fps, 30);
    EXPECT_TRUE(p.config.webcam.aspect_ratio_locked);
    EXPECT_FALSE(p.config.webcam.overlay_user_placed);
}

TEST(RecordingPreset, DefaultPreset_WebcamPip_BottomRight) {
    const RecordingPreset p = MakeDefaultPreset();
    constexpr float kPipSize = 0.25f;
    const float expected_x = 1.0f - kPipSize - kDefaultPipInsetNorm;
    const float expected_y = 1.0f - kPipSize - kDefaultPipInsetNorm;

    EXPECT_FLOAT_EQ(p.config.webcam.overlay.w_norm, kPipSize);
    EXPECT_FLOAT_EQ(p.config.webcam.overlay.h_norm, kPipSize);
    EXPECT_FLOAT_EQ(p.config.webcam.overlay.x_norm, expected_x);
    EXPECT_FLOAT_EQ(p.config.webcam.overlay.y_norm, expected_y);
}

TEST(RecordingPreset, DefaultPreset_AudioRows) {
    const RecordingPreset p = MakeDefaultPreset();
    // Computer audio (SystemOutput) enabled; mic disabled.
    ASSERT_EQ(p.config.audio.source_rows.size(), 2u);
    EXPECT_EQ(p.config.audio.source_rows[0].kind, recorder_core::AudioSourceKind::SystemOutput);
    EXPECT_TRUE(p.config.audio.source_rows[0].enabled);
    EXPECT_FALSE(p.config.audio.source_rows[0].merge_with_above);
    EXPECT_EQ(p.config.audio.source_rows[1].kind, recorder_core::AudioSourceKind::Mic);
    EXPECT_FALSE(p.config.audio.source_rows[1].enabled);
}

TEST(RecordingPreset, DefaultPreset_AudioMiscFields) {
    const RecordingPreset p = MakeDefaultPreset();
    EXPECT_EQ(p.config.audio.target_kind, capability::CaptureTargetKind::Display);
    EXPECT_EQ(p.config.audio.mic_channel_mode, recorder_core::MicChannelMode::Auto);
    EXPECT_FALSE(p.config.audio.selected_mic_device_id.has_value());
    EXPECT_FLOAT_EQ(p.config.audio.mic_gain_linear, 1.0f);
    EXPECT_FALSE(p.config.audio.selected_window_pid.has_value());
}

TEST(RecordingPreset, DefaultPreset_ChromaKey) {
    const RecordingPreset p = MakeDefaultPreset();
    EXPECT_FALSE(p.config.webcam.chroma_key.enabled);
    EXPECT_EQ(p.config.webcam.chroma_key.r, 0u);
    EXPECT_EQ(p.config.webcam.chroma_key.g, 177u);
    EXPECT_EQ(p.config.webcam.chroma_key.b, 64u);
    EXPECT_FLOAT_EQ(p.config.webcam.chroma_key.tolerance, 0.30f);
    EXPECT_FLOAT_EQ(p.config.webcam.chroma_key.softness, 0.05f);
}

// ===========================================================================
// Default is stable under reconciliation and sanitization
// ===========================================================================

TEST(RecordingPreset, DefaultPreset_ReconcileDoesNotChangeAv1Opus_MKV) {
    RecordingPreset p = MakeDefaultPreset();
    const auto container_before = p.config.output.container;
    const auto video_before = p.config.output.video_codec;
    const auto audio_before = p.config.output.audio_codec;

    ReconcileContainerCodecs(p.config.output);

    EXPECT_EQ(p.config.output.container, container_before);
    EXPECT_EQ(p.config.output.video_codec, video_before);
    EXPECT_EQ(p.config.output.audio_codec, audio_before);
    EXPECT_EQ(p.config.output.container, capability::Container::Matroska);
    EXPECT_EQ(p.config.output.video_codec, capability::VideoCodec::Av1Nvenc);
    EXPECT_EQ(p.config.output.audio_codec, capability::AudioCodec::Opus);
}

TEST(RecordingPreset, DefaultPreset_SanitizeRoundTrip) {
    const RecordingPreset p = MakeDefaultPreset();
    const RecordingPresetConfig sanitized = SanitizePresetConfig(p.config);
    EXPECT_TRUE(NormalizedConfigEquals(p.config, sanitized));
}

// ===========================================================================
// SanitizePresetConfig — countdown
// ===========================================================================

TEST(RecordingPreset, Sanitize_Countdown_Invalid4_ResetTo0) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.countdown_seconds = 4;
    const RecordingPresetConfig s = SanitizePresetConfig(cfg);
    EXPECT_EQ(s.countdown_seconds, 0);
}

TEST(RecordingPreset, Sanitize_Countdown_Invalid7_ResetTo0) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.countdown_seconds = 7;
    const RecordingPresetConfig s = SanitizePresetConfig(cfg);
    EXPECT_EQ(s.countdown_seconds, 0);
}

TEST(RecordingPreset, Sanitize_Countdown_InvalidNegative1_ResetTo0) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.countdown_seconds = -1;
    const RecordingPresetConfig s = SanitizePresetConfig(cfg);
    EXPECT_EQ(s.countdown_seconds, 0);
}

TEST(RecordingPreset, Sanitize_Countdown_Invalid99_ResetTo0) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.countdown_seconds = 99;
    const RecordingPresetConfig s = SanitizePresetConfig(cfg);
    EXPECT_EQ(s.countdown_seconds, 0);
}

TEST(RecordingPreset, Sanitize_Countdown_3_Preserved) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.countdown_seconds = 3;
    EXPECT_EQ(SanitizePresetConfig(cfg).countdown_seconds, 3);
}

TEST(RecordingPreset, Sanitize_Countdown_5_Preserved) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.countdown_seconds = 5;
    EXPECT_EQ(SanitizePresetConfig(cfg).countdown_seconds, 5);
}

TEST(RecordingPreset, Sanitize_Countdown_10_Preserved) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.countdown_seconds = 10;
    EXPECT_EQ(SanitizePresetConfig(cfg).countdown_seconds, 10);
}

// ===========================================================================
// SanitizePresetConfig — frame rate
// ===========================================================================

TEST(RecordingPreset, Sanitize_FrameRateNum0_ResetTo60_1) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.video.frame_rate_num = 0;
    cfg.video.frame_rate_den = 1;
    const RecordingPresetConfig s = SanitizePresetConfig(cfg);
    EXPECT_EQ(s.video.frame_rate_num, 60u);
    EXPECT_EQ(s.video.frame_rate_den, 1u);
}

TEST(RecordingPreset, Sanitize_FrameRateDen0_ResetTo60_1) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.video.frame_rate_num = 30;
    cfg.video.frame_rate_den = 0;
    const RecordingPresetConfig s = SanitizePresetConfig(cfg);
    EXPECT_EQ(s.video.frame_rate_num, 60u);
    EXPECT_EQ(s.video.frame_rate_den, 1u);
}

// ===========================================================================
// SanitizePresetConfig — mic gain
// ===========================================================================

TEST(RecordingPreset, Sanitize_MicGain_NonFiniteNaN_ResetTo1) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.audio.mic_gain_linear = std::numeric_limits<float>::quiet_NaN();
    const RecordingPresetConfig s = SanitizePresetConfig(cfg);
    EXPECT_FLOAT_EQ(s.audio.mic_gain_linear, 1.0f);
}

TEST(RecordingPreset, Sanitize_MicGain_Inf_ResetTo1) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.audio.mic_gain_linear = std::numeric_limits<float>::infinity();
    const RecordingPresetConfig s = SanitizePresetConfig(cfg);
    EXPECT_FLOAT_EQ(s.audio.mic_gain_linear, 1.0f);
}

TEST(RecordingPreset, Sanitize_MicGain_NegInf_ResetTo1) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.audio.mic_gain_linear = -std::numeric_limits<float>::infinity();
    const RecordingPresetConfig s = SanitizePresetConfig(cfg);
    EXPECT_FLOAT_EQ(s.audio.mic_gain_linear, 1.0f);
}

TEST(RecordingPreset, Sanitize_MicGain_Zero_ResetTo1) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.audio.mic_gain_linear = 0.0f;
    const RecordingPresetConfig s = SanitizePresetConfig(cfg);
    EXPECT_FLOAT_EQ(s.audio.mic_gain_linear, 1.0f);
}

TEST(RecordingPreset, Sanitize_MicGain_Negative_ResetTo1) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.audio.mic_gain_linear = -0.5f;
    const RecordingPresetConfig s = SanitizePresetConfig(cfg);
    EXPECT_FLOAT_EQ(s.audio.mic_gain_linear, 1.0f);
}

// ===========================================================================
// SanitizePresetConfig — webcam (NaN/Inf PiP and chroma delegated)
// ===========================================================================

TEST(RecordingPreset, Sanitize_Webcam_NanOverlay_BecomesFinite) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.webcam.overlay.x_norm = std::numeric_limits<float>::quiet_NaN();
    cfg.webcam.overlay.y_norm = std::numeric_limits<float>::infinity();
    cfg.webcam.overlay.w_norm = std::numeric_limits<float>::quiet_NaN();
    cfg.webcam.overlay.h_norm = -std::numeric_limits<float>::infinity();

    const RecordingPresetConfig s = SanitizePresetConfig(cfg);
    EXPECT_TRUE(std::isfinite(s.webcam.overlay.x_norm));
    EXPECT_TRUE(std::isfinite(s.webcam.overlay.y_norm));
    EXPECT_TRUE(std::isfinite(s.webcam.overlay.w_norm));
    EXPECT_TRUE(std::isfinite(s.webcam.overlay.h_norm));
}

TEST(RecordingPreset, Sanitize_Webcam_NanChroma_BecomesFinite) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.webcam.chroma_key.tolerance = std::numeric_limits<float>::quiet_NaN();
    cfg.webcam.chroma_key.softness = std::numeric_limits<float>::infinity();

    const RecordingPresetConfig s = SanitizePresetConfig(cfg);
    EXPECT_TRUE(std::isfinite(s.webcam.chroma_key.tolerance));
    EXPECT_TRUE(std::isfinite(s.webcam.chroma_key.softness));
}

// ===========================================================================
// ReconcileContainerCodecs — all rules
// ===========================================================================

TEST(RecordingPreset, Reconcile_Mp4_ForcesH264Aac) {
    OutputSettingsModel out;
    out.container = capability::Container::Mp4;
    out.video_codec = capability::VideoCodec::Av1Nvenc;
    out.audio_codec = capability::AudioCodec::Opus;
    ReconcileContainerCodecs(out);
    EXPECT_EQ(out.video_codec, capability::VideoCodec::H264Nvenc);
    EXPECT_EQ(out.audio_codec, capability::AudioCodec::AacMf);
}

TEST(RecordingPreset, Reconcile_WebM_ForcesAv1Opus) {
    OutputSettingsModel out;
    out.container = capability::Container::WebM;
    out.video_codec = capability::VideoCodec::H264Nvenc;
    out.audio_codec = capability::AudioCodec::AacMf;
    ReconcileContainerCodecs(out);
    EXPECT_EQ(out.video_codec, capability::VideoCodec::Av1Nvenc);
    EXPECT_EQ(out.audio_codec, capability::AudioCodec::Opus);
}

TEST(RecordingPreset, Reconcile_Mkv_H264Opus_ForcesAac) {
    OutputSettingsModel out;
    out.container = capability::Container::Matroska;
    out.video_codec = capability::VideoCodec::H264Nvenc;
    out.audio_codec = capability::AudioCodec::Opus;
    ReconcileContainerCodecs(out);
    EXPECT_EQ(out.video_codec, capability::VideoCodec::H264Nvenc);
    EXPECT_EQ(out.audio_codec, capability::AudioCodec::AacMf);
}

TEST(RecordingPreset, Reconcile_Mkv_Hevc_ForcesH264) {
    OutputSettingsModel out;
    out.container = capability::Container::Matroska;
    out.video_codec = capability::VideoCodec::HevcNvenc;
    out.audio_codec = capability::AudioCodec::AacMf;
    ReconcileContainerCodecs(out);
    EXPECT_EQ(out.video_codec, capability::VideoCodec::H264Nvenc);
    EXPECT_EQ(out.audio_codec, capability::AudioCodec::AacMf);
}

TEST(RecordingPreset, Reconcile_Mkv_Pcm_ForcesAac) {
    OutputSettingsModel out;
    out.container = capability::Container::Matroska;
    out.video_codec = capability::VideoCodec::H264Nvenc;
    out.audio_codec = capability::AudioCodec::Pcm;
    ReconcileContainerCodecs(out);
    EXPECT_EQ(out.audio_codec, capability::AudioCodec::AacMf);
}

TEST(RecordingPreset, Reconcile_Mkv_Av1Opus_Unchanged) {
    OutputSettingsModel out;
    out.container = capability::Container::Matroska;
    out.video_codec = capability::VideoCodec::Av1Nvenc;
    out.audio_codec = capability::AudioCodec::Opus;
    ReconcileContainerCodecs(out);
    EXPECT_EQ(out.video_codec, capability::VideoCodec::Av1Nvenc);
    EXPECT_EQ(out.audio_codec, capability::AudioCodec::Opus);
}

TEST(RecordingPreset, Sanitize_Mp4Container_ForcesH264Aac) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.output.container = capability::Container::Mp4;
    cfg.output.video_codec = capability::VideoCodec::Av1Nvenc;
    cfg.output.audio_codec = capability::AudioCodec::Opus;
    const RecordingPresetConfig s = SanitizePresetConfig(cfg);
    EXPECT_EQ(s.output.video_codec, capability::VideoCodec::H264Nvenc);
    EXPECT_EQ(s.output.audio_codec, capability::AudioCodec::AacMf);
}

TEST(RecordingPreset, Sanitize_WebMContainer_ForcesAv1Opus) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.output.container = capability::Container::WebM;
    cfg.output.video_codec = capability::VideoCodec::H264Nvenc;
    cfg.output.audio_codec = capability::AudioCodec::AacMf;
    const RecordingPresetConfig s = SanitizePresetConfig(cfg);
    EXPECT_EQ(s.output.video_codec, capability::VideoCodec::Av1Nvenc);
    EXPECT_EQ(s.output.audio_codec, capability::AudioCodec::Opus);
}

TEST(RecordingPreset, Sanitize_Mkv_H264Opus_ForcesAac) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.output.container = capability::Container::Matroska;
    cfg.output.video_codec = capability::VideoCodec::H264Nvenc;
    cfg.output.audio_codec = capability::AudioCodec::Opus;
    const RecordingPresetConfig s = SanitizePresetConfig(cfg);
    EXPECT_EQ(s.output.video_codec, capability::VideoCodec::H264Nvenc);
    EXPECT_EQ(s.output.audio_codec, capability::AudioCodec::AacMf);
}

TEST(RecordingPreset, Sanitize_Mkv_Hevc_ForcesH264) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.output.container = capability::Container::Matroska;
    cfg.output.video_codec = capability::VideoCodec::HevcNvenc;
    cfg.output.audio_codec = capability::AudioCodec::AacMf;
    const RecordingPresetConfig s = SanitizePresetConfig(cfg);
    EXPECT_EQ(s.output.video_codec, capability::VideoCodec::H264Nvenc);
}

// ===========================================================================
// NormalizedConfigEquals — identical configs
// ===========================================================================

TEST(RecordingPreset, NormalizedEquals_IdenticalConfigs_Equal) {
    const RecordingPresetConfig a = MakeDefaultPreset().config;
    const RecordingPresetConfig b = MakeDefaultPreset().config;
    EXPECT_TRUE(NormalizedConfigEquals(a, b));
}

// ===========================================================================
// NormalizedConfigEquals — PiP tolerance
// ===========================================================================

TEST(RecordingPreset, NormalizedEquals_PipDelta_5e4_Equal) {
    const RecordingPresetConfig a = MakeDefaultPreset().config;
    const RecordingPresetConfig b = WithPipDelta(a, 5e-4f);
    EXPECT_TRUE(NormalizedConfigEquals(a, b));
}

TEST(RecordingPreset, NormalizedEquals_PipDelta_5e3_NotEqual) {
    const RecordingPresetConfig a = MakeDefaultPreset().config;
    const RecordingPresetConfig b = WithPipDelta(a, 5e-3f);
    EXPECT_FALSE(NormalizedConfigEquals(a, b));
}

// ===========================================================================
// NormalizedConfigEquals — mic_gain tolerance
// ===========================================================================

TEST(RecordingPreset, NormalizedEquals_MicGainDelta_1e4_Equal) {
    const RecordingPresetConfig a = MakeDefaultPreset().config;
    const RecordingPresetConfig b = WithMicGainDelta(a, 1e-4f);
    EXPECT_TRUE(NormalizedConfigEquals(a, b));
}

TEST(RecordingPreset, NormalizedEquals_MicGainDelta_1e2_NotEqual) {
    const RecordingPresetConfig a = MakeDefaultPreset().config;
    const RecordingPresetConfig b = WithMicGainDelta(a, 1e-2f);
    EXPECT_FALSE(NormalizedConfigEquals(a, b));
}

// ===========================================================================
// NormalizedConfigEquals — individual field checks
// ===========================================================================

TEST(RecordingPreset, NormalizedEquals_CountdownChange_NotEqual) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.countdown_seconds = 5;
    EXPECT_FALSE(NormalizedConfigEquals(a, b));
}

TEST(RecordingPreset, NormalizedEquals_WebcamMirrorChange_NotEqual) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.webcam.mirror = true;
    EXPECT_FALSE(NormalizedConfigEquals(a, b));
}

TEST(RecordingPreset, NormalizedEquals_AudioRowEnabledFlip_NotEqual) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    // Flip the SystemOutput row from enabled to disabled.
    b.audio.source_rows[0].enabled = false;
    EXPECT_FALSE(NormalizedConfigEquals(a, b));
}

TEST(RecordingPreset, NormalizedEquals_OutputContainerChange_NotEqual) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.output.container = capability::Container::Mp4;
    // Also reconcile so we don't fail on codecs alone.
    ReconcileContainerCodecs(b.output);
    EXPECT_FALSE(NormalizedConfigEquals(a, b));
}

TEST(RecordingPreset, NormalizedEquals_VideoQualityChange_NotEqual) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.video.quality = recorder_core::NvencQualityPreset::Small;
    EXPECT_FALSE(NormalizedConfigEquals(a, b));
}

TEST(RecordingPreset, NormalizedEquals_CaptureDisplayKeyChange_NotEqual) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.capture.display_key = "MONITOR-001";
    EXPECT_FALSE(NormalizedConfigEquals(a, b));
}

TEST(RecordingPreset, NormalizedEquals_RegionRectChange_NotEqual) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    a.capture.kind = PresetCaptureKind::Region;
    a.capture.has_region = true;
    a.capture.region.x = 0;
    a.capture.region.y = 0;
    a.capture.region.width = 640;
    a.capture.region.height = 480;

    RecordingPresetConfig b = a;
    b.capture.region.width = 800;
    EXPECT_FALSE(NormalizedConfigEquals(a, b));
}

// ===========================================================================
// NormalizedConfigEquals — semantic audio row ordering
//
// Two row vectors that resolve to the same plan AND have the same enabled-set
// compare equal even if listed in a different plan-equivalent order.
//
// Concrete example: two configurations each have SystemOutput enabled and Mic
// disabled.  Swapping Mic-disabled before/after SystemOutput-enabled should
// produce an equal comparison because:
//   - ResolveAudioTracks ignores disabled rows for plan construction.
//   - EnabledSourceKinds returns the same set {SystemOutput}.
// ===========================================================================

TEST(RecordingPreset, NormalizedEquals_SemanticAudioOrder_PlanEquivalent_Equal) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    // a has [SystemOutput(enabled), Mic(disabled)]

    RecordingPresetConfig b = a;
    // Swap so Mic(disabled) comes first; plan-equivalent because disabled rows
    // do not contribute to tracks or to the enabled-source set.
    b.audio.source_rows = {
        {recorder_core::AudioSourceKind::Mic, false, false},
        {recorder_core::AudioSourceKind::SystemOutput, true, false},
    };

    EXPECT_TRUE(NormalizedConfigEquals(a, b));
}

TEST(RecordingPreset, NormalizedEquals_DifferentEnabledSets_NotEqual) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    // a: [SystemOutput(enabled), Mic(disabled)]

    RecordingPresetConfig b = a;
    // b: [SystemOutput(disabled), Mic(enabled)] — different enabled set even though
    // the row order is the same.
    b.audio.source_rows[0].enabled = false;
    b.audio.source_rows[1].enabled = true;

    EXPECT_FALSE(NormalizedConfigEquals(a, b));
}

// ===========================================================================
// ConfigDirtyEquivalent — capture fields do NOT affect dirty state
// ===========================================================================

TEST(RecordingPreset, DirtyEquivalent_IdenticalConfigs_Equivalent) {
    const RecordingPresetConfig a = MakeDefaultPreset().config;
    const RecordingPresetConfig b = MakeDefaultPreset().config;
    EXPECT_TRUE(ConfigDirtyEquivalent(a, b));
}

TEST(RecordingPreset, DirtyEquivalent_CaptureKindChange_StillEquivalent) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.capture.kind = PresetCaptureKind::Window;
    // Capture kind is excluded from dirty; must still be equivalent.
    EXPECT_TRUE(ConfigDirtyEquivalent(a, b));
}

TEST(RecordingPreset, DirtyEquivalent_CaptureDisplayKeyChange_StillEquivalent) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.capture.display_key = "MONITOR-001";
    // display_key is excluded from dirty; preset must not show dirty on startup.
    EXPECT_TRUE(ConfigDirtyEquivalent(a, b));
}

TEST(RecordingPreset, DirtyEquivalent_CaptureWindowKeyChange_StillEquivalent) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.capture.window_key = "chrome.exe|Google Chrome";
    EXPECT_TRUE(ConfigDirtyEquivalent(a, b));
}

TEST(RecordingPreset, DirtyEquivalent_CaptureRegionChange_StillEquivalent) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    a.capture.kind = PresetCaptureKind::Region;
    a.capture.has_region = true;
    a.capture.region.x = 0;
    a.capture.region.y = 0;
    a.capture.region.width = 640;
    a.capture.region.height = 480;
    a.capture.region_display_key = "\\\\?\\DISPLAY#SAM#001";

    RecordingPresetConfig b = a;
    b.capture.region.width = 800;
    b.capture.region_display_key = "\\\\?\\DISPLAY#SAM#002";
    // Region geometry and region_display_key excluded from dirty.
    EXPECT_TRUE(ConfigDirtyEquivalent(a, b));
}

TEST(RecordingPreset, DirtyEquivalent_VideoQualityChange_NotEquivalent) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.video.quality = recorder_core::NvencQualityPreset::Small;
    EXPECT_FALSE(ConfigDirtyEquivalent(a, b));
}

TEST(RecordingPreset, DirtyEquivalent_AudioRowEnabledFlip_NotEquivalent) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.audio.source_rows[0].enabled = false; // Disable SystemOutput
    EXPECT_FALSE(ConfigDirtyEquivalent(a, b));
}

TEST(RecordingPreset, DirtyEquivalent_CountdownChange_NotEquivalent) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.countdown_seconds = 5;
    EXPECT_FALSE(ConfigDirtyEquivalent(a, b));
}

TEST(RecordingPreset, DirtyEquivalent_WebcamMirrorChange_NotEquivalent) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.webcam.mirror = true;
    EXPECT_FALSE(ConfigDirtyEquivalent(a, b));
}

TEST(RecordingPreset, DirtyEquivalent_OutputContainerChange_NotEquivalent) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.output.container = capability::Container::Mp4;
    ReconcileContainerCodecs(b.output);
    EXPECT_FALSE(ConfigDirtyEquivalent(a, b));
}

TEST(RecordingPreset, DirtyEquivalent_PipWithinTol_Equivalent) {
    const RecordingPresetConfig a = MakeDefaultPreset().config;
    const RecordingPresetConfig b = WithPipDelta(a, 5e-4f);
    EXPECT_TRUE(ConfigDirtyEquivalent(a, b));
}

TEST(RecordingPreset, DirtyEquivalent_PipBeyondTol_NotEquivalent) {
    const RecordingPresetConfig a = MakeDefaultPreset().config;
    const RecordingPresetConfig b = WithPipDelta(a, 5e-3f);
    EXPECT_FALSE(ConfigDirtyEquivalent(a, b));
}

// NormalizedConfigEquals STILL detects capture changes (unchanged behaviour).
TEST(RecordingPreset, NormalizedEquals_StillDetects_CaptureDisplayKeyChange) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.capture.display_key = "MONITOR-001";
    EXPECT_FALSE(NormalizedConfigEquals(a, b));
}

// ===========================================================================
// GeneratePresetId
// ===========================================================================

TEST(RecordingPreset, GeneratePresetId_1000_AllUnique) {
    std::unordered_set<std::string> ids;
    for (int i = 0; i < 1000; ++i) {
        ids.insert(GeneratePresetId());
    }
    EXPECT_EQ(ids.size(), 1000u);
}

TEST(RecordingPreset, GeneratePresetId_AllStartWithPresetDot) {
    for (int i = 0; i < 20; ++i) {
        const std::string id = GeneratePresetId();
        EXPECT_EQ(id.substr(0, 7), "preset.") << "id=" << id;
    }
}

TEST(RecordingPreset, GeneratePresetId_NoneEqualsDefaultPresetId) {
    for (int i = 0; i < 1000; ++i) {
        EXPECT_NE(GeneratePresetId(), std::string(kDefaultPresetId));
    }
}

TEST(RecordingPreset, GeneratePresetId_LengthIs23) {
    // "preset." (7) + 16 hex chars = 23
    for (int i = 0; i < 20; ++i) {
        const std::string id = GeneratePresetId();
        EXPECT_EQ(id.size(), 23u) << "id=" << id;
    }
}

// ===========================================================================
// IsValidPresetName / NormalizePresetName
// ===========================================================================

TEST(RecordingPreset, IsValidPresetName_EmptyString_False) {
    EXPECT_FALSE(IsValidPresetName(""));
}

TEST(RecordingPreset, IsValidPresetName_WhitespaceOnly_False) {
    EXPECT_FALSE(IsValidPresetName("   "));
    EXPECT_FALSE(IsValidPresetName("\t\n"));
}

TEST(RecordingPreset, IsValidPresetName_SingleChar_True) {
    EXPECT_TRUE(IsValidPresetName("x"));
}

TEST(RecordingPreset, IsValidPresetName_NormalName_True) {
    EXPECT_TRUE(IsValidPresetName("My Recording Preset"));
}

TEST(RecordingPreset, NormalizePresetName_TrimsLeadingAndTrailing) {
    EXPECT_EQ(NormalizePresetName("  hello  "), "hello");
    EXPECT_EQ(NormalizePresetName("\t name \n"), "name");
}

TEST(RecordingPreset, NormalizePresetName_AlreadyTrimmed_Unchanged) {
    EXPECT_EQ(NormalizePresetName("My Preset"), "My Preset");
}

TEST(RecordingPreset, NormalizePresetName_EmptyString_ReturnsEmpty) {
    EXPECT_EQ(NormalizePresetName(""), "");
}

// ===========================================================================
// SanitizePreset — name and id handling
// ===========================================================================

TEST(RecordingPreset, SanitizePreset_EmptyName_SetsFallback) {
    RecordingPreset p;
    p.id = "preset.abc123";
    p.name = "";
    const RecordingPreset s = SanitizePreset(p);
    EXPECT_EQ(s.name, "Untitled preset");
}

TEST(RecordingPreset, SanitizePreset_WhitespaceOnlyName_SetsFallback) {
    RecordingPreset p;
    p.id = "preset.abc123";
    p.name = "   ";
    const RecordingPreset s = SanitizePreset(p);
    EXPECT_EQ(s.name, "Untitled preset");
}

TEST(RecordingPreset, SanitizePreset_TrimsName) {
    RecordingPreset p;
    p.id = "preset.abc123";
    p.name = "  My Preset  ";
    const RecordingPreset s = SanitizePreset(p);
    EXPECT_EQ(s.name, "My Preset");
}

TEST(RecordingPreset, SanitizePreset_EmptyId_GeneratesId) {
    RecordingPreset p;
    p.id = "";
    p.name = "test";
    const RecordingPreset s = SanitizePreset(p);
    EXPECT_FALSE(s.id.empty());
    EXPECT_EQ(s.id.substr(0, 7), "preset.");
}

TEST(RecordingPreset, SanitizePreset_NonEmptyId_Preserved) {
    RecordingPreset p;
    p.id = "preset.somespecificid1";
    p.name = "test";
    const RecordingPreset s = SanitizePreset(p);
    EXPECT_EQ(s.id, "preset.somespecificid1");
}

} // namespace
} // namespace exosnap
