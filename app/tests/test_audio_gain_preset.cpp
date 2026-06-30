// test_audio_gain_preset.cpp
// Tests for per-row gain_db + muted preset round-trip, dirty-tracking,
// and sanitize (ADR 0018 — Audio v2, 0.6.0).

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>

#include <capability/audio_ui_state.h>
#include <recorder_core/audio_track_model.h>

#include "models/RecordingPreset.h"
#include "settings/RecordingPresetStore.h"

#include <cmath>
#include <vector>

namespace exosnap {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int s_counter = 0;

QString UniqueTempPath() {
    // Unique temp dir per test process (gtest_discover_tests = one process per
    // test); a shared fixed name races under ctest -j.
    static QTemporaryDir s_dir;
    return s_dir.filePath(QStringLiteral("exosnap_gain_test_%1.ini").arg(++s_counter));
}

void CleanupFile(const QString& path) {
    if (!path.isEmpty() && QFileInfo::exists(path))
        QFile::remove(path);
}

// Build a preset with custom per-row gain/mute.
RecordingPreset MakeGainPreset(float app_gain, bool app_muted, float mic_gain, bool mic_muted) {
    RecordingPreset p = MakeDefaultPreset();
    p.config.audio.source_rows = {
        {recorder_core::AudioSourceKind::App, true, false, app_gain, app_muted},
        {recorder_core::AudioSourceKind::Mic, true, false, mic_gain, mic_muted},
        {recorder_core::AudioSourceKind::Sys, true, false, 0.0f, false},
    };
    return p;
}

// ---------------------------------------------------------------------------
// Round-trip: save → load preserves gain_db and muted.
// ---------------------------------------------------------------------------

TEST(AudioGainPreset, RoundTrip_GainAndMuted) {
    const QString path = UniqueTempPath();

    RecordingPreset preset = MakeGainPreset(6.0f, false, -12.0f, true);
    preset.id = std::string(kDefaultPresetId);
    preset.name = "Gain Test";

    RecordingPresetStore store(path);
    store.Save({preset}, preset.id, preset.id);

    const PersistedPresetState loaded = store.Load();
    ASSERT_FALSE(loaded.presets.empty());
    const auto& lp = loaded.presets.front();

    ASSERT_EQ(lp.config.audio.source_rows.size(), 3u);

    const auto& app_row = lp.config.audio.source_rows[0];
    EXPECT_NEAR(app_row.gain_db, 6.0f, 0.01f);
    EXPECT_FALSE(app_row.muted);

    const auto& mic_row = lp.config.audio.source_rows[1];
    EXPECT_NEAR(mic_row.gain_db, -12.0f, 0.01f);
    EXPECT_TRUE(mic_row.muted);

    const auto& sys_row = lp.config.audio.source_rows[2];
    EXPECT_NEAR(sys_row.gain_db, 0.0f, 0.01f);
    EXPECT_FALSE(sys_row.muted);

    CleanupFile(path);
}

TEST(AudioGainPreset, RoundTrip_DefaultGain_UnchangedBehavior) {
    // Default preset uses 0 dB / not muted — must survive round-trip unchanged.
    const QString path = UniqueTempPath();

    RecordingPreset preset = MakeDefaultPreset();
    RecordingPresetStore store(path);
    store.Save({preset}, preset.id, preset.id);

    const PersistedPresetState loaded = store.Load();
    ASSERT_FALSE(loaded.presets.empty());
    const auto& lp = loaded.presets.front();

    for (const auto& row : lp.config.audio.source_rows) {
        EXPECT_NEAR(row.gain_db, 0.0f, 0.01f);
        EXPECT_FALSE(row.muted);
    }

    CleanupFile(path);
}

// ---------------------------------------------------------------------------
// Dirty tracking detects gain_db and muted changes.
// ---------------------------------------------------------------------------

TEST(AudioGainPreset, DirtyTracking_GainDbChange) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;

    // Identical configs → not dirty.
    EXPECT_TRUE(ConfigDirtyEquivalent(a, b));

    // Change gain on first row.
    b.audio.source_rows[0].gain_db = 6.0f;
    EXPECT_FALSE(ConfigDirtyEquivalent(a, b));
}

TEST(AudioGainPreset, DirtyTracking_MutedChange) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;

    EXPECT_TRUE(ConfigDirtyEquivalent(a, b));

    b.audio.source_rows[0].muted = true;
    EXPECT_FALSE(ConfigDirtyEquivalent(a, b));
}

TEST(AudioGainPreset, DirtyTracking_SmallGainDeltaNotDirty) {
    // Tolerance is 1e-2f dB — sub-tolerance change must not mark dirty.
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.audio.source_rows[0].gain_db = 0.005f; // < 1e-2f tolerance
    EXPECT_TRUE(ConfigDirtyEquivalent(a, b));
}

TEST(AudioGainPreset, NormalizedConfigEquals_IncludesGain) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;

    EXPECT_TRUE(NormalizedConfigEquals(a, b));

    b.audio.source_rows[0].gain_db = -3.0f;
    EXPECT_FALSE(NormalizedConfigEquals(a, b));
}

// ---------------------------------------------------------------------------
// Sanitize clamps gain_db.
// ---------------------------------------------------------------------------

TEST(AudioGainPreset, Sanitize_ClampsGainAboveMax) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.audio.source_rows[0].gain_db = 100.0f; // above kMaxGainDb = +24
    const auto sanitized = SanitizePresetConfig(cfg);
    EXPECT_NEAR(sanitized.audio.source_rows[0].gain_db, recorder_core::kMaxGainDb, 0.01f);
}

TEST(AudioGainPreset, Sanitize_ClampsGainBelowMin) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.audio.source_rows[0].gain_db = -200.0f; // below kMinGainDb = -60
    const auto sanitized = SanitizePresetConfig(cfg);
    EXPECT_NEAR(sanitized.audio.source_rows[0].gain_db, recorder_core::kMinGainDb, 0.01f);
}

TEST(AudioGainPreset, Sanitize_ResetsNaN) {
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.audio.source_rows[0].gain_db = std::numeric_limits<float>::quiet_NaN();
    const auto sanitized = SanitizePresetConfig(cfg);
    EXPECT_NEAR(sanitized.audio.source_rows[0].gain_db, 0.0f, 0.01f);
}

TEST(AudioGainPreset, Sanitize_ResetsInf) {
    // Infinity is not finite, so sanitize resets it to 0 dB (the safe default).
    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.audio.source_rows[0].gain_db = std::numeric_limits<float>::infinity();
    const auto sanitized = SanitizePresetConfig(cfg);
    EXPECT_NEAR(sanitized.audio.source_rows[0].gain_db, 0.0f, 0.01f);
}

// ---------------------------------------------------------------------------
// Mic gain composition: mic_gain_linear * GainDbToLinear(row.gain_db, muted)
// The preset layer does not perform this multiplication directly, but we
// verify that building an AudioPlanResult passes mic_gain_linear unchanged
// and that ResolveAudioTracks carries the per-row gain for the Mic row.
// ---------------------------------------------------------------------------

TEST(AudioGainPreset, MicGainComposition_UnityRowGain_PreservesMicGainLinear) {
    // With row at 0 dB, not muted: effective = mic_gain_linear * 1.0 = mic_gain_linear.
    capability::AudioUiState state;
    state.mic_gain_linear = 1.5f;
    state.source_rows = {
        {recorder_core::AudioSourceKind::Mic, true, false, 0.0f, false},
    };

    const auto plan = capability::BuildAudioPlan(state);
    EXPECT_NEAR(plan.mic_gain_linear, 1.5f, 1e-4f);

    // Row gain for Mic should be 1.0 (0 dB, not muted).
    ASSERT_EQ(plan.plan.tracks.size(), 1u);
    ASSERT_EQ(plan.plan.tracks[0].source_gain_linear.size(), 1u);
    EXPECT_NEAR(plan.plan.tracks[0].source_gain_linear[0], 1.0f, 1e-5f);
    // Assembly-site composition: effective = 1.5 * 1.0 = 1.5 (verified conceptually).
}

TEST(AudioGainPreset, MicGainComposition_MutedRow_EffectiveIsZero) {
    // Muted row: effective = mic_gain_linear * 0 = 0 regardless of mic_gain_linear.
    capability::AudioUiState state;
    state.mic_gain_linear = 2.0f;
    state.source_rows = {
        {recorder_core::AudioSourceKind::Mic, true, false, 0.0f, true}, // muted
    };

    const auto plan = capability::BuildAudioPlan(state);
    ASSERT_EQ(plan.plan.tracks.size(), 1u);
    ASSERT_EQ(plan.plan.tracks[0].source_gain_linear.size(), 1u);
    // Row gain is 0 because muted; assembly site will compute 2.0 * 0 = 0.
    EXPECT_FLOAT_EQ(plan.plan.tracks[0].source_gain_linear[0], 0.0f);
}

} // namespace
} // namespace exosnap
