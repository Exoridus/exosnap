#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>

#include <capability/audio_ui_state.h>
#include <recorder_core/audio_track_model.h>
#include <recorder_core/recorder_session.h>

#include "models/RecordingPreset.h"
#include "settings/RecordingPresetStore.h"

namespace exosnap {
namespace {

// ===========================================================================
// Helpers
// ===========================================================================

// Returns a unique temp file path under QStandardPaths::TempLocation.
// Each call returns a different filename so tests never share state.
QString UniqueTempPath() {
    const QString temp_dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    static int s_counter = 0;
    return QDir(temp_dir).filePath(QStringLiteral("exosnap_test_presets_%1.ini").arg(++s_counter));
}

// Cleanup helper — deletes the temp file if it exists.
void CleanupFile(const QString& path) {
    if (!path.isEmpty() && QFileInfo::exists(path)) {
        QFile::remove(path);
    }
}

// Build a Region preset.
RecordingPreset MakeRegionPreset() {
    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "Region Preset";
    p.config = MakeDefaultPreset().config;
    p.config.capture.kind = PresetCaptureKind::Region;
    p.config.capture.has_region = true;
    p.config.capture.region.x = 100;
    p.config.capture.region.y = 200;
    p.config.capture.region.width = 1280;
    p.config.capture.region.height = 720;
    p.config.capture.region_display_key = "monitor-0";
    p.config.output.resolution.mode = OutputResolutionMode::FHD1080;
    return p;
}

// Build a Window preset with PID.
RecordingPreset MakeWindowWithPidPreset() {
    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "Window Preset";
    p.config = MakeDefaultPreset().config;
    p.config.capture.kind = PresetCaptureKind::Window;
    p.config.capture.window_key = "chrome.exe";
    p.config.audio.target_kind = capability::CaptureTargetKind::Window;
    p.config.audio.selected_window_pid = 12345u;
    return p;
}

// Build a Webcam preset with mirror + chroma.
RecordingPreset MakeWebcamPreset() {
    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "Webcam Preset";
    p.config = MakeDefaultPreset().config;
    p.config.webcam.enabled = true;
    p.config.webcam.device_id = "\\\\?\\usb#vid_046d";
    p.config.webcam.width = 1920;
    p.config.webcam.height = 1080;
    p.config.webcam.fps = 60;
    p.config.webcam.overlay.x_norm = 0.1f;
    p.config.webcam.overlay.y_norm = 0.2f;
    p.config.webcam.overlay.w_norm = 0.3f;
    p.config.webcam.overlay.h_norm = 0.3f;
    p.config.webcam.overlay_user_placed = true;
    p.config.webcam.aspect_ratio_locked = false;
    p.config.webcam.mirror = true;
    p.config.webcam.chroma_key.enabled = true;
    p.config.webcam.chroma_key.r = 10;
    p.config.webcam.chroma_key.g = 200;
    p.config.webcam.chroma_key.b = 30;
    p.config.webcam.chroma_key.tolerance = 0.40f;
    p.config.webcam.chroma_key.softness = 0.10f;
    return p;
}

// Compare two presets by id+name+NormalizedConfigEquals.
bool PresetsEqual(const RecordingPreset& a, const RecordingPreset& b) {
    return a.id == b.id && a.name == b.name && NormalizedConfigEquals(a.config, b.config);
}

// ===========================================================================
// Round-trip: 3 diverse presets
// ===========================================================================

TEST(RecordingPresetStore, RoundTrip_3Presets_AllFieldsPreserved) {
    const QString path = UniqueTempPath();

    RecordingPreset r1 = MakeRegionPreset();
    RecordingPreset r2 = MakeWindowWithPidPreset();
    RecordingPreset r3 = MakeWebcamPreset();

    const std::string sel_id = r2.id;
    const std::string def_id = r1.id;

    {
        RecordingPresetStore store(path);
        store.Save({r1, r2, r3}, sel_id, def_id);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.was_reset);
        ASSERT_EQ(state.presets.size(), 3u);
        EXPECT_EQ(state.selected_id, sel_id);
        EXPECT_EQ(state.default_id, def_id);

        // Match by id (order is preserved by Save/Load).
        const auto find = [&](const std::string& id) -> const RecordingPreset* {
            for (const auto& p : state.presets)
                if (p.id == id)
                    return &p;
            return nullptr;
        };

        const RecordingPreset* loaded_r1 = find(r1.id);
        const RecordingPreset* loaded_r2 = find(r2.id);
        const RecordingPreset* loaded_r3 = find(r3.id);
        ASSERT_NE(loaded_r1, nullptr);
        ASSERT_NE(loaded_r2, nullptr);
        ASSERT_NE(loaded_r3, nullptr);

        EXPECT_TRUE(PresetsEqual(r1, *loaded_r1));
        EXPECT_TRUE(PresetsEqual(r2, *loaded_r2));
        EXPECT_TRUE(PresetsEqual(r3, *loaded_r3));
    }

    CleanupFile(path);
}

// ===========================================================================
// Frame rate persists
// ===========================================================================

TEST(RecordingPresetStore, FrameRatePersists_50fps) {
    const QString path = UniqueTempPath();

    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "50fps Preset";
    p.config = MakeDefaultPreset().config;
    p.config.video.frame_rate_num = 50;
    p.config.video.frame_rate_den = 1;

    {
        RecordingPresetStore store(path);
        store.Save({p}, p.id, p.id);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.was_reset);
        ASSERT_EQ(state.presets.size(), 1u);
        EXPECT_EQ(state.presets[0].config.video.frame_rate_num, 50u);
        EXPECT_EQ(state.presets[0].config.video.frame_rate_den, 1u);
    }

    CleanupFile(path);
}

TEST(RecordingPresetStore, FrameRate120Unavailable_ResetsTo60fps) {
    const QString path = UniqueTempPath();

    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "Unavailable 120fps Preset";
    p.config = MakeDefaultPreset().config;
    p.config.video.frame_rate_num = 120;
    p.config.video.frame_rate_den = 1;

    {
        RecordingPresetStore store(path);
        store.Save({p}, p.id, p.id);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.was_reset);
        ASSERT_EQ(state.presets.size(), 1u);
        EXPECT_EQ(state.presets[0].config.video.frame_rate_num, 60u);
        EXPECT_EQ(state.presets[0].config.video.frame_rate_den, 1u);
    }

    CleanupFile(path);
}

TEST(RecordingPresetStore, OutputResolutionPersists_1440p) {
    const QString path = UniqueTempPath();

    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "1440p Preset";
    p.config = MakeDefaultPreset().config;
    p.config.output.resolution.mode = OutputResolutionMode::QHD1440;
    p.config.video.frame_rate_num = 30;
    p.config.video.frame_rate_den = 1;
    p.config.video.cfr = false;

    {
        RecordingPresetStore store(path);
        store.Save({p}, p.id, p.id);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.was_reset);
        ASSERT_EQ(state.presets.size(), 1u);
        EXPECT_EQ(state.presets[0].config.output.resolution.mode, OutputResolutionMode::QHD1440);
        EXPECT_EQ(state.presets[0].config.video.frame_rate_num, 30u);
        EXPECT_FALSE(state.presets[0].config.video.cfr);
    }

    CleanupFile(path);
}

// ===========================================================================
// Absent file → seeded default
// ===========================================================================

TEST(RecordingPresetStore, AbsentFile_ReturnsResetDefault) {
    // A path that does not exist.
    const QString path = UniqueTempPath(); // created unique, but never written.

    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    EXPECT_TRUE(state.was_reset);
    ASSERT_EQ(state.presets.size(), 1u);
    EXPECT_EQ(state.selected_id, std::string(kDefaultPresetId));
    EXPECT_EQ(state.default_id, std::string(kDefaultPresetId));
}

// ===========================================================================
// Malformed item skipped — one valid + one with empty id
// ===========================================================================

TEST(RecordingPresetStore, MalformedItem_EmptyId_Skipped_ValidKept) {
    const QString path = UniqueTempPath();

    // Write the file manually using a second QSettings.
    {
        QSettings s(path, QSettings::IniFormat);
        s.setValue(QStringLiteral("schemaVersion"), kPresetSchemaVersion);
        s.setValue(QStringLiteral("selectedId"), QStringLiteral("preset.aabbccddeeff0011"));
        s.setValue(QStringLiteral("defaultId"), QStringLiteral("preset.aabbccddeeff0011"));

        s.beginWriteArray(QStringLiteral("items"), 2);

        // Item 0: valid.
        s.setArrayIndex(0);
        s.setValue(QStringLiteral("id"), QStringLiteral("preset.aabbccddeeff0011"));
        s.setValue(QStringLiteral("name"), QStringLiteral("Good Item"));
        s.setValue(QStringLiteral("capture_kind"), QStringLiteral("display"));
        s.setValue(QStringLiteral("out_container"), QStringLiteral("mkv"));
        s.setValue(QStringLiteral("out_video_codec"), QStringLiteral("av1"));
        s.setValue(QStringLiteral("out_audio_codec"), QStringLiteral("opus"));

        // Item 1: malformed (empty id).
        s.setArrayIndex(1);
        s.setValue(QStringLiteral("id"), QStringLiteral(""));
        s.setValue(QStringLiteral("name"), QStringLiteral("Bad Item"));

        s.endArray();
        s.sync();
    }

    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    EXPECT_FALSE(state.was_reset);
    ASSERT_EQ(state.presets.size(), 1u);
    EXPECT_EQ(state.presets[0].id, "preset.aabbccddeeff0011");
    EXPECT_EQ(state.presets[0].name, "Good Item");

    CleanupFile(path);
}

// ===========================================================================
// Duplicate ids repaired — two items with same id → exactly one survives
// ===========================================================================

TEST(RecordingPresetStore, DuplicateIds_Repaired_OneKeepedFirst) {
    const QString path = UniqueTempPath();

    {
        QSettings s(path, QSettings::IniFormat);
        s.setValue(QStringLiteral("schemaVersion"), kPresetSchemaVersion);
        s.setValue(QStringLiteral("selectedId"), QStringLiteral("preset.aabbccddeeff0011"));
        s.setValue(QStringLiteral("defaultId"), QStringLiteral("preset.aabbccddeeff0011"));

        s.beginWriteArray(QStringLiteral("items"), 2);

        s.setArrayIndex(0);
        s.setValue(QStringLiteral("id"), QStringLiteral("preset.aabbccddeeff0011"));
        s.setValue(QStringLiteral("name"), QStringLiteral("First"));
        s.setValue(QStringLiteral("capture_kind"), QStringLiteral("display"));
        s.setValue(QStringLiteral("out_container"), QStringLiteral("mkv"));
        s.setValue(QStringLiteral("out_video_codec"), QStringLiteral("av1"));
        s.setValue(QStringLiteral("out_audio_codec"), QStringLiteral("opus"));

        s.setArrayIndex(1);
        s.setValue(QStringLiteral("id"), QStringLiteral("preset.aabbccddeeff0011")); // Duplicate.
        s.setValue(QStringLiteral("name"), QStringLiteral("Second"));
        s.setValue(QStringLiteral("capture_kind"), QStringLiteral("display"));
        s.setValue(QStringLiteral("out_container"), QStringLiteral("mkv"));
        s.setValue(QStringLiteral("out_video_codec"), QStringLiteral("av1"));
        s.setValue(QStringLiteral("out_audio_codec"), QStringLiteral("opus"));

        s.endArray();
        s.sync();
    }

    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    ASSERT_EQ(state.presets.size(), 1u);
    EXPECT_EQ(state.presets[0].name, "First");

    CleanupFile(path);
}

// ===========================================================================
// Invalid selectedId falls back to defaultId
// ===========================================================================

TEST(RecordingPresetStore, InvalidSelectedId_FallsBackToDefaultId) {
    const QString path = UniqueTempPath();

    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "Solo Preset";
    p.config = MakeDefaultPreset().config;

    {
        RecordingPresetStore store(path);
        store.Save({p}, "preset.doesnotexist1234567", p.id); // selected invalid
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.was_reset);
        EXPECT_EQ(state.selected_id, p.id); // falls back to default_id
    }

    CleanupFile(path);
}

// ===========================================================================
// Invalid defaultId falls back to first / kDefaultPresetId
// ===========================================================================

TEST(RecordingPresetStore, InvalidDefaultId_FallsBackToFirst) {
    const QString path = UniqueTempPath();

    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "Solo Preset";
    p.config = MakeDefaultPreset().config;

    {
        RecordingPresetStore store(path);
        store.Save({p}, p.id, "preset.doesnotexist1234567"); // default invalid
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.was_reset);
        // No kDefaultPresetId in list → default falls to first preset.
        EXPECT_EQ(state.default_id, p.id);
    }

    CleanupFile(path);
}

TEST(RecordingPresetStore, InvalidDefaultId_FallsBackToKDefaultPresetId_WhenPresent) {
    const QString path = UniqueTempPath();

    RecordingPreset def = MakeDefaultPreset();

    {
        RecordingPresetStore store(path);
        store.Save({def}, std::string(kDefaultPresetId), "preset.doesnotexist1234567");
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.was_reset);
        EXPECT_EQ(state.default_id, std::string(kDefaultPresetId));
    }

    CleanupFile(path);
}

// ===========================================================================
// Save then Save-fewer removes stale items
// ===========================================================================

TEST(RecordingPresetStore, SaveFewer_RemovesStaleItems) {
    const QString path = UniqueTempPath();

    RecordingPreset p1;
    p1.id = GeneratePresetId();
    p1.name = "P1";
    p1.config = MakeDefaultPreset().config;

    RecordingPreset p2;
    p2.id = GeneratePresetId();
    p2.name = "P2";
    p2.config = MakeDefaultPreset().config;

    RecordingPreset p3;
    p3.id = GeneratePresetId();
    p3.name = "P3";
    p3.config = MakeDefaultPreset().config;

    {
        RecordingPresetStore store(path);
        store.Save({p1, p2, p3}, p1.id, p1.id);
    }

    // Save only p1.
    {
        RecordingPresetStore store(path);
        store.Save({p1}, p1.id, p1.id);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.was_reset);
        ASSERT_EQ(state.presets.size(), 1u);
        EXPECT_EQ(state.presets[0].id, p1.id);
    }

    CleanupFile(path);
}

// ===========================================================================
// Empty-path store — Load seeds default, no crash; Save no-op
// ===========================================================================

TEST(RecordingPresetStore, EmptyPath_Load_SeedsDefault_NoCrash) {
    const QString empty_path;
    RecordingPresetStore store(empty_path);
    const PersistedPresetState state = store.Load();
    EXPECT_TRUE(state.was_reset);
    ASSERT_EQ(state.presets.size(), 1u);
    EXPECT_EQ(state.selected_id, std::string(kDefaultPresetId));
}

TEST(RecordingPresetStore, EmptyPath_Save_NoCrash) {
    const QString empty_path;
    RecordingPresetStore store(empty_path);
    // Should not crash or throw.
    store.Save({MakeDefaultPreset()}, std::string(kDefaultPresetId), std::string(kDefaultPresetId));
}

// ===========================================================================
// Schema version mismatch → reset
// ===========================================================================

TEST(RecordingPresetStore, WrongSchemaVersion_ReturnsReset) {
    const QString path = UniqueTempPath();

    {
        QSettings s(path, QSettings::IniFormat);
        s.setValue(QStringLiteral("schemaVersion"), kPresetSchemaVersion + 1);
        s.setValue(QStringLiteral("selectedId"), QStringLiteral("preset.default"));
        s.setValue(QStringLiteral("defaultId"), QStringLiteral("preset.default"));

        s.beginWriteArray(QStringLiteral("items"), 1);
        s.setArrayIndex(0);
        s.setValue(QStringLiteral("id"), QStringLiteral("preset.default"));
        s.setValue(QStringLiteral("name"), QStringLiteral("Default"));
        s.endArray();
        s.sync();
    }

    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    EXPECT_TRUE(state.was_reset);

    CleanupFile(path);
}

} // namespace
} // namespace exosnap
