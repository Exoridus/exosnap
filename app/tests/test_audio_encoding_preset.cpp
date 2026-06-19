// Tests for audio encoding parameters in preset persistence (ADR 0019):
//   - Preset round-trip for audio_bitrate_kbps, opus_frame_duration, opus_complexity
//   - SanitizePresetConfig bounds
//   - NormalizedConfigEquals / ConfigDirtyEquivalent for the new fields
//   - RecordingPresetStore save+load with QTemporaryDir

#include <gtest/gtest.h>

#include <QFile>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>

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
    return tmp.filePath(QStringLiteral("presets_audio_enc.ini"));
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
    // Write a minimal INI without the audio encoding keys.
    const QString path = UniqueTempPath();
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("schemaVersion"), kPresetSchemaVersion);
        settings.setValue(QStringLiteral("selectedId"), QStringLiteral("preset.default"));
        settings.setValue(QStringLiteral("defaultId"), QStringLiteral("preset.default"));
        settings.beginWriteArray(QStringLiteral("items"), 1);
        settings.setArrayIndex(0);
        settings.setValue(QStringLiteral("id"), QStringLiteral("preset.default"));
        settings.setValue(QStringLiteral("name"), QStringLiteral("Default"));
        settings.endArray();
    }
    RecordingPresetStore store(path);
    const auto state = store.Load();
    ASSERT_FALSE(state.presets.empty());
    // Missing keys must fall back to defaults: 160, Ms20, 10.
    EXPECT_EQ(state.presets.front().config.audio.audio_bitrate_kbps, 160u);
    EXPECT_EQ(state.presets.front().config.audio.opus_frame_duration, OpusFrameDuration::Ms20);
    EXPECT_EQ(state.presets.front().config.audio.opus_complexity, 10);
    QFile::remove(path);
}

} // namespace
} // namespace exosnap
