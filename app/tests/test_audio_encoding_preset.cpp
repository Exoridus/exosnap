// Tests for audio encoding parameters in preset persistence (ADR 0019):
//   - Preset round-trip for audio_bitrate_kbps, opus_frame_duration, opus_complexity
//   - SanitizePresetConfig bounds
//   - NormalizedConfigEquals / ConfigDirtyEquivalent for the new fields
//   - RecordingPresetStore save+load with QTemporaryDir

#include <gtest/gtest.h>

#include <cmath>

#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QTextStream>

#include <capability/audio_ui_state.h>
#include <recorder_core/recorder_session.h>

#include "models/RecordingPreset.h"
#include "settings/RecordingPresetStore.h"

namespace exosnap {
namespace {

using recorder_core::OpusFrameDuration;

// ===========================================================================
// Default preset audio encoding defaults
// ===========================================================================

TEST(AudioEncodingPreset, DefaultPreset_BitratIs160) {
    const RecordingPreset p = MakeDefaultPreset();
    EXPECT_EQ(p.config.audio.audio_bitrate_kbps, 160u);
}

TEST(AudioEncodingPreset, DefaultPreset_FrameDuration_Is20ms) {
    const RecordingPreset p = MakeDefaultPreset();
    EXPECT_EQ(p.config.audio.opus_frame_duration, OpusFrameDuration::Ms20);
}

TEST(AudioEncodingPreset, DefaultPreset_Complexity_Is10) {
    const RecordingPreset p = MakeDefaultPreset();
    EXPECT_EQ(p.config.audio.opus_complexity, 10);
}

// ===========================================================================
// SanitizePresetConfig — bounds checks for new audio encoding fields
// ===========================================================================

TEST(AudioEncodingPreset, Sanitize_BitrateTooHigh_ClampsTo510) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.audio.audio_bitrate_kbps = 9999u;
    const auto sanitized = SanitizePresetConfig(cfg);
    EXPECT_LE(sanitized.audio.audio_bitrate_kbps, 510u);
}

TEST(AudioEncodingPreset, Sanitize_Complexity_AboveMax_ClampsTo10) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.audio.opus_complexity = 999;
    const auto sanitized = SanitizePresetConfig(cfg);
    EXPECT_EQ(sanitized.audio.opus_complexity, 10);
}

TEST(AudioEncodingPreset, Sanitize_Complexity_BelowMin_ClampsTo0) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.audio.opus_complexity = -5;
    const auto sanitized = SanitizePresetConfig(cfg);
    EXPECT_EQ(sanitized.audio.opus_complexity, 0);
}

TEST(AudioEncodingPreset, Sanitize_ValidValues_PassThrough) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.audio.audio_bitrate_kbps = 128u;
    cfg.audio.opus_frame_duration = OpusFrameDuration::Ms10;
    cfg.audio.opus_complexity = 7;
    const auto sanitized = SanitizePresetConfig(cfg);
    EXPECT_EQ(sanitized.audio.audio_bitrate_kbps, 128u);
    EXPECT_EQ(sanitized.audio.opus_frame_duration, OpusFrameDuration::Ms10);
    EXPECT_EQ(sanitized.audio.opus_complexity, 7);
}

// ===========================================================================
// NormalizedConfigEquals — audio encoding fields are compared
// ===========================================================================

TEST(AudioEncodingPreset, NormalizedEquals_DifferentBitrate_NotEqual) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.audio.audio_bitrate_kbps = a.audio.audio_bitrate_kbps + 32u;
    EXPECT_FALSE(NormalizedConfigEquals(a, b));
}

TEST(AudioEncodingPreset, NormalizedEquals_DifferentFrameDuration_NotEqual) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.audio.opus_frame_duration = OpusFrameDuration::Ms10;
    EXPECT_FALSE(NormalizedConfigEquals(a, b));
}

TEST(AudioEncodingPreset, NormalizedEquals_DifferentComplexity_NotEqual) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.audio.opus_complexity = 5;
    EXPECT_FALSE(NormalizedConfigEquals(a, b));
}

TEST(AudioEncodingPreset, NormalizedEquals_SameAudioParams_Equal) {
    const RecordingPresetConfig a = MakeDefaultPreset().config;
    const RecordingPresetConfig b = a; // exact copy
    EXPECT_TRUE(NormalizedConfigEquals(a, b));
}

// ===========================================================================
// ConfigDirtyEquivalent — audio encoding fields are compared
// ===========================================================================

TEST(AudioEncodingPreset, DirtyEquivalent_DifferentBitrate_NotEquivalent) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.audio.audio_bitrate_kbps = 256u;
    EXPECT_FALSE(ConfigDirtyEquivalent(a, b));
}

TEST(AudioEncodingPreset, DirtyEquivalent_DifferentFrameDuration_NotEquivalent) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.audio.opus_frame_duration = OpusFrameDuration::Ms5;
    EXPECT_FALSE(ConfigDirtyEquivalent(a, b));
}

TEST(AudioEncodingPreset, DirtyEquivalent_SameAudioParams_IsEquivalent) {
    const RecordingPresetConfig a = MakeDefaultPreset().config;
    const RecordingPresetConfig b = a;
    EXPECT_TRUE(ConfigDirtyEquivalent(a, b));
}

// ===========================================================================
// RecordingPresetStore save + load round-trip
// ===========================================================================

static QString UniqueTempPath() {
    QTemporaryDir tmp;
    tmp.setAutoRemove(false); // we'll let the preset store write here
    return tmp.filePath(QStringLiteral("presets_audio_enc.toml"));
}

// Write a TOML string to a file (UTF-8).
static bool WriteTomlString(const QString& path, const QString& toml_content) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts << toml_content;
    return true;
}

TEST(AudioEncodingPreset, StoreRoundTrip_Bitrate) {
    const QString path = UniqueTempPath();
    RecordingPresetStore store(path);

    RecordingPreset p = MakeDefaultPreset();
    p.config.audio.audio_bitrate_kbps = 256u;
    store.Save({p}, std::string(kDefaultPresetId), std::string(kDefaultPresetId));

    const auto state = store.Load();
    ASSERT_FALSE(state.presets.empty());
    EXPECT_EQ(state.presets.front().config.audio.audio_bitrate_kbps, 256u);
    QFile::remove(path);
}

TEST(AudioEncodingPreset, StoreRoundTrip_OpusFrameDuration_Ms10) {
    const QString path = UniqueTempPath();
    RecordingPresetStore store(path);

    RecordingPreset p = MakeDefaultPreset();
    p.config.audio.opus_frame_duration = OpusFrameDuration::Ms10;
    store.Save({p}, std::string(kDefaultPresetId), std::string(kDefaultPresetId));

    const auto state = store.Load();
    ASSERT_FALSE(state.presets.empty());
    EXPECT_EQ(state.presets.front().config.audio.opus_frame_duration, OpusFrameDuration::Ms10);
    QFile::remove(path);
}

TEST(AudioEncodingPreset, StoreRoundTrip_OpusFrameDuration_Ms5) {
    const QString path = UniqueTempPath();
    RecordingPresetStore store(path);

    RecordingPreset p = MakeDefaultPreset();
    p.config.audio.opus_frame_duration = OpusFrameDuration::Ms5;
    store.Save({p}, std::string(kDefaultPresetId), std::string(kDefaultPresetId));

    const auto state = store.Load();
    ASSERT_FALSE(state.presets.empty());
    EXPECT_EQ(state.presets.front().config.audio.opus_frame_duration, OpusFrameDuration::Ms5);
    QFile::remove(path);
}

TEST(AudioEncodingPreset, StoreRoundTrip_OpusFrameDuration_Ms2_5) {
    const QString path = UniqueTempPath();
    RecordingPresetStore store(path);

    RecordingPreset p = MakeDefaultPreset();
    p.config.audio.opus_frame_duration = OpusFrameDuration::Ms2_5;
    store.Save({p}, std::string(kDefaultPresetId), std::string(kDefaultPresetId));

    const auto state = store.Load();
    ASSERT_FALSE(state.presets.empty());
    EXPECT_EQ(state.presets.front().config.audio.opus_frame_duration, OpusFrameDuration::Ms2_5);
    QFile::remove(path);
}

TEST(AudioEncodingPreset, StoreRoundTrip_Complexity) {
    const QString path = UniqueTempPath();
    RecordingPresetStore store(path);

    RecordingPreset p = MakeDefaultPreset();
    p.config.audio.opus_complexity = 3;
    store.Save({p}, std::string(kDefaultPresetId), std::string(kDefaultPresetId));

    const auto state = store.Load();
    ASSERT_FALSE(state.presets.empty());
    EXPECT_EQ(state.presets.front().config.audio.opus_complexity, 3);
    QFile::remove(path);
}

TEST(AudioEncodingPreset, StoreRoundTrip_AllThreeFields) {
    const QString path = UniqueTempPath();
    RecordingPresetStore store(path);

    RecordingPreset p = MakeDefaultPreset();
    p.config.audio.audio_bitrate_kbps = 128u;
    p.config.audio.opus_frame_duration = OpusFrameDuration::Ms10;
    p.config.audio.opus_complexity = 7;
    store.Save({p}, std::string(kDefaultPresetId), std::string(kDefaultPresetId));

    const auto state = store.Load();
    ASSERT_FALSE(state.presets.empty());
    const auto& loaded = state.presets.front().config.audio;
    EXPECT_EQ(loaded.audio_bitrate_kbps, 128u);
    EXPECT_EQ(loaded.opus_frame_duration, OpusFrameDuration::Ms10);
    EXPECT_EQ(loaded.opus_complexity, 7);
    QFile::remove(path);
}

TEST(AudioEncodingPreset, StoreLoad_MissingKeys_FallsBackToDefaults) {
    // Write a minimal TOML fixture without the audio encoding keys.
    // Missing keys must fall back to defaults inside PresetFromToml.
    const QString path = UniqueTempPath();
    const QString toml =
        QStringLiteral("schema_version = %1\n"
                       "selected_id = \"preset.default\"\n"
                       "default_id  = \"preset.default\"\n"
                       "\n"
                       "[[presets]]\n"
                       "id   = \"preset.default\"\n"
                       "name = \"Default\"\n"
                       "countdown_seconds = 0\n"
                       "[presets.capture]\n"
                       "kind = \"display\"\n"
                       "display_key = \"\"\n"
                       "window_key  = \"\"\n"
                       "has_region  = false\n"
                       "region_x = 0\n"
                       "region_y = 0\n"
                       "region_w = 0\n"
                       "region_h = 0\n"
                       "region_display_key = \"\"\n"
                       "[presets.output]\n"
                       "folder = \"\"\n"
                       "naming_pattern = \"\"\n"
                       "container = \"mkv\"\n"
                       "video_codec = \"av1\"\n"
                       "audio_codec = \"opus\"\n"
                       "resolution_mode = \"native\"\n"
                       "custom_width = 0\n"
                       "custom_height = 0\n"
                       "fit_mode = \"contain\"\n"
                       "split_mode = \"off\"\n"
                       "split_custom_minutes = 30\n"
                       "[presets.video]\n"
                       "quality = \"balanced\"\n"
                       "rate_control = \"cq\"\n"
                       "bitrate_kbps = 20000\n"
                       "cfr = true\n"
                       "capture_cursor = true\n"
                       "frame_rate_num = 60\n"
                       "frame_rate_den = 1\n"
                       "[presets.audio]\n"
                       "target_kind = \"display\"\n"
                       "mic_channel_mode = \"auto\"\n"
                       "selected_mic_device_id = \"\"\n"
                       "mic_gain_linear = 1.0\n"
                       "has_window_pid = false\n"
                       "window_pid = 0\n"
                       "# audio_bitrate_kbps, opus_frame_duration, opus_complexity intentionally omitted\n"
                       "sources = []\n"
                       "[presets.webcam]\n"
                       "enabled = false\n"
                       "device_id = \"\"\n"
                       "width = 1280\n"
                       "height = 720\n"
                       "fps = 30\n"
                       "overlay_x = 0.0\n"
                       "overlay_y = 0.0\n"
                       "overlay_w = 0.25\n"
                       "overlay_h = 0.25\n"
                       "overlay_user_placed = false\n"
                       "aspect_ratio_locked = true\n"
                       "mirror = false\n"
                       "[presets.webcam.chroma_key]\n"
                       "enabled = false\n"
                       "color_mode = \"green\"\n"
                       "custom_r = 0\n"
                       "custom_g = 255\n"
                       "custom_b = 0\n"
                       "tolerance = 0.4\n"
                       "softness = 0.15\n"
                       "spill = 0.3\n")
            .arg(kPresetSchemaVersion);

    ASSERT_TRUE(WriteTomlString(path, toml));

    RecordingPresetStore store(path);
    const auto state = store.Load();
    ASSERT_FALSE(state.presets.empty());
    // Missing keys must fall back to defaults: 160, Ms20, 10.
    EXPECT_EQ(state.presets.front().config.audio.audio_bitrate_kbps, 160u);
    EXPECT_EQ(state.presets.front().config.audio.opus_frame_duration, OpusFrameDuration::Ms20);
    EXPECT_EQ(state.presets.front().config.audio.opus_complexity, 10);
    // Limiter keys also omitted → enabled / 0.0 dBFS defaults.
    EXPECT_TRUE(state.presets.front().config.audio.limiter_enabled);
    EXPECT_FLOAT_EQ(state.presets.front().config.audio.limiter_ceiling_db, 0.0f);
    // Mic HPF keys also omitted → disabled / 80 Hz defaults.
    EXPECT_FALSE(state.presets.front().config.audio.mic_hpf_enabled);
    EXPECT_FLOAT_EQ(state.presets.front().config.audio.mic_hpf_cutoff_hz, 80.0f);
    QFile::remove(path);
}

// ===========================================================================
// Brickwall limiter (Audio v2 — 0.6.0)
// ===========================================================================

TEST(AudioEncodingPreset, DefaultPreset_Limiter_EnabledAtZeroDb) {
    const RecordingPreset p = MakeDefaultPreset();
    EXPECT_TRUE(p.config.audio.limiter_enabled);
    EXPECT_FLOAT_EQ(p.config.audio.limiter_ceiling_db, 0.0f);
}

TEST(AudioEncodingPreset, Sanitize_LimiterCeiling_PositiveClampsToZero) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.audio.limiter_ceiling_db = 6.0f; // > 0 dBFS is invalid
    const auto sanitized = SanitizePresetConfig(cfg);
    EXPECT_FLOAT_EQ(sanitized.audio.limiter_ceiling_db, 0.0f);
}

TEST(AudioEncodingPreset, Sanitize_LimiterCeiling_BelowFloorClampsToMinus60) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.audio.limiter_ceiling_db = -200.0f;
    const auto sanitized = SanitizePresetConfig(cfg);
    EXPECT_FLOAT_EQ(sanitized.audio.limiter_ceiling_db, -60.0f);
}

TEST(AudioEncodingPreset, Sanitize_LimiterCeiling_NonFiniteResetsToZero) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.audio.limiter_ceiling_db = std::nanf("");
    const auto sanitized = SanitizePresetConfig(cfg);
    EXPECT_FLOAT_EQ(sanitized.audio.limiter_ceiling_db, 0.0f);
}

TEST(AudioEncodingPreset, NormalizedEquals_DifferentLimiterEnabled_NotEqual) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.audio.limiter_enabled = !a.audio.limiter_enabled;
    EXPECT_FALSE(NormalizedConfigEquals(a, b));
}

TEST(AudioEncodingPreset, NormalizedEquals_DifferentLimiterCeiling_NotEqual) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.audio.limiter_ceiling_db = a.audio.limiter_ceiling_db - 3.0f;
    EXPECT_FALSE(NormalizedConfigEquals(a, b));
}

TEST(AudioEncodingPreset, DirtyEquivalent_DifferentLimiterCeiling_NotEquivalent) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.audio.limiter_ceiling_db = -6.0f;
    EXPECT_FALSE(ConfigDirtyEquivalent(a, b));
}

TEST(AudioEncodingPreset, StoreRoundTrip_Limiter) {
    const QString path = UniqueTempPath();
    RecordingPresetStore store(path);

    RecordingPreset p = MakeDefaultPreset();
    p.config.audio.limiter_enabled = false;
    p.config.audio.limiter_ceiling_db = -3.0f;
    store.Save({p}, std::string(kDefaultPresetId), std::string(kDefaultPresetId));

    const auto state = store.Load();
    ASSERT_FALSE(state.presets.empty());
    const auto& loaded = state.presets.front().config.audio;
    EXPECT_FALSE(loaded.limiter_enabled);
    EXPECT_NEAR(loaded.limiter_ceiling_db, -3.0f, 0.01f);
    QFile::remove(path);
}

// ===========================================================================
// Microphone high-pass filter (Audio v2 — 0.6.0)
// ===========================================================================

TEST(AudioEncodingPreset, DefaultPreset_MicHpf_DisabledAt80Hz) {
    const RecordingPreset p = MakeDefaultPreset();
    EXPECT_FALSE(p.config.audio.mic_hpf_enabled);
    EXPECT_FLOAT_EQ(p.config.audio.mic_hpf_cutoff_hz, 80.0f);
}

TEST(AudioEncodingPreset, Sanitize_MicHpfCutoff_AboveMaxClampsTo1000) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.audio.mic_hpf_cutoff_hz = 5000.0f;
    const auto sanitized = SanitizePresetConfig(cfg);
    EXPECT_FLOAT_EQ(sanitized.audio.mic_hpf_cutoff_hz, 1000.0f);
}

TEST(AudioEncodingPreset, Sanitize_MicHpfCutoff_BelowMinClampsTo20) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.audio.mic_hpf_cutoff_hz = 5.0f;
    const auto sanitized = SanitizePresetConfig(cfg);
    EXPECT_FLOAT_EQ(sanitized.audio.mic_hpf_cutoff_hz, 20.0f);
}

TEST(AudioEncodingPreset, Sanitize_MicHpfCutoff_NonFiniteResetsTo80) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.audio.mic_hpf_cutoff_hz = std::nanf("");
    const auto sanitized = SanitizePresetConfig(cfg);
    EXPECT_FLOAT_EQ(sanitized.audio.mic_hpf_cutoff_hz, 80.0f);
}

TEST(AudioEncodingPreset, NormalizedEquals_DifferentMicHpfEnabled_NotEqual) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.audio.mic_hpf_enabled = !a.audio.mic_hpf_enabled;
    EXPECT_FALSE(NormalizedConfigEquals(a, b));
}

TEST(AudioEncodingPreset, NormalizedEquals_DifferentMicHpfCutoff_NotEqual) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.audio.mic_hpf_cutoff_hz = a.audio.mic_hpf_cutoff_hz + 40.0f;
    EXPECT_FALSE(NormalizedConfigEquals(a, b));
}

TEST(AudioEncodingPreset, DirtyEquivalent_DifferentMicHpfCutoff_NotEquivalent) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.audio.mic_hpf_cutoff_hz = 200.0f;
    EXPECT_FALSE(ConfigDirtyEquivalent(a, b));
}

TEST(AudioEncodingPreset, StoreRoundTrip_MicHpf) {
    const QString path = UniqueTempPath();
    RecordingPresetStore store(path);

    RecordingPreset p = MakeDefaultPreset();
    p.config.audio.mic_hpf_enabled = true;
    p.config.audio.mic_hpf_cutoff_hz = 120.0f;
    store.Save({p}, std::string(kDefaultPresetId), std::string(kDefaultPresetId));

    const auto state = store.Load();
    ASSERT_FALSE(state.presets.empty());
    const auto& loaded = state.presets.front().config.audio;
    EXPECT_TRUE(loaded.mic_hpf_enabled);
    EXPECT_NEAR(loaded.mic_hpf_cutoff_hz, 120.0f, 0.01f);
    QFile::remove(path);
}

} // namespace
} // namespace exosnap
