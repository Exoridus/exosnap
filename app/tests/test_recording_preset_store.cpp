#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>
#include <QTextStream>

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
    return QDir(temp_dir).filePath(QStringLiteral("exosnap_test_presets_%1.toml").arg(++s_counter));
}

// Write a TOML string to a file (UTF-8, no BOM).
bool WriteTomlString(const QString& path, const QString& toml_content) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts << toml_content;
    return true;
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
    p.config.webcam.chroma_key.color_mode = WebcamChromaKeyColorMode::Custom;
    p.config.webcam.chroma_key.custom_r = 10;
    p.config.webcam.chroma_key.custom_g = 200;
    p.config.webcam.chroma_key.custom_b = 30;
    p.config.webcam.chroma_key.tolerance = 0.40f;
    p.config.webcam.chroma_key.softness = 0.10f;
    p.config.webcam.chroma_key.spill_reduction = 0.25f;
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

TEST(RecordingPresetStore, OutputResolutionPersists_Custom) {
    const QString path = UniqueTempPath();

    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "Custom Resolution Preset";
    p.config = MakeDefaultPreset().config;
    p.config.output.resolution.mode = OutputResolutionMode::Custom;
    p.config.output.resolution.custom_width = 2560;
    p.config.output.resolution.custom_height = 1440;

    {
        RecordingPresetStore store(path);
        store.Save({p}, p.id, p.id);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.was_reset);
        ASSERT_EQ(state.presets.size(), 1u);
        EXPECT_EQ(state.presets[0].config.output.resolution.mode, OutputResolutionMode::Custom);
        EXPECT_EQ(state.presets[0].config.output.resolution.custom_width, 2560u);
        EXPECT_EQ(state.presets[0].config.output.resolution.custom_height, 1440u);
    }

    CleanupFile(path);
}

TEST(RecordingPresetStore, SplitSettingsPersist_Custom) {
    const QString path = UniqueTempPath();

    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "Split Preset";
    p.config = MakeDefaultPreset().config;
    p.config.output.split.mode = SplitRecordingMode::Custom;
    p.config.output.split.custom_minutes = 45;

    {
        RecordingPresetStore store(path);
        store.Save({p}, p.id, p.id);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.was_reset);
        ASSERT_EQ(state.presets.size(), 1u);
        EXPECT_EQ(state.presets[0].config.output.split.mode, SplitRecordingMode::Custom);
        EXPECT_EQ(state.presets[0].config.output.split.custom_minutes, 45u);
    }

    CleanupFile(path);
}

TEST(RecordingPresetStore, SplitSettingsPersist_PresetDuration) {
    const QString path = UniqueTempPath();

    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "Split 30 Preset";
    p.config = MakeDefaultPreset().config;
    p.config.output.split.mode = SplitRecordingMode::Every30Min;

    {
        RecordingPresetStore store(path);
        store.Save({p}, p.id, p.id);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        ASSERT_EQ(state.presets.size(), 1u);
        EXPECT_EQ(state.presets[0].config.output.split.mode, SplitRecordingMode::Every30Min);
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

    // Hand-write a TOML fixture: one valid preset + one with empty id.
    const QString toml = QStringLiteral("schema_version = %1\n"
                                        "selected_id = \"preset.aabbccddeeff0011\"\n"
                                        "default_id  = \"preset.aabbccddeeff0011\"\n"
                                        "\n"
                                        "[[presets]]\n"
                                        "id   = \"preset.aabbccddeeff0011\"\n"
                                        "name = \"Good Item\"\n"
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
                                        "audio_bitrate_kbps = 160\n"
                                        "opus_frame_duration = \"20ms\"\n"
                                        "opus_complexity = 10\n"
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
                                        "spill = 0.3\n"
                                        "\n"
                                        "[[presets]]\n"
                                        "id   = \"\"\n"
                                        "name = \"Bad Item\"\n"
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
                                        "audio_bitrate_kbps = 160\n"
                                        "opus_frame_duration = \"20ms\"\n"
                                        "opus_complexity = 10\n"
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

    // Hand-write a TOML fixture with two presets sharing the same id.
    // The minimal required sub-tables are provided; missing optional fields fall
    // back to defaults inside PresetFromToml.
    const QString preset_block = QStringLiteral("[[presets]]\n"
                                                "id   = \"preset.aabbccddeeff0011\"\n"
                                                "name = \"%1\"\n"
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
                                                "audio_bitrate_kbps = 160\n"
                                                "opus_frame_duration = \"20ms\"\n"
                                                "opus_complexity = 10\n"
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
                                                "spill = 0.3\n"
                                                "\n");

    const QString toml = QStringLiteral("schema_version = %1\n"
                                        "selected_id = \"preset.aabbccddeeff0011\"\n"
                                        "default_id  = \"preset.aabbccddeeff0011\"\n"
                                        "\n")
                             .arg(kPresetSchemaVersion) +
                         preset_block.arg(QStringLiteral("First")) +
                         preset_block.arg(QStringLiteral("Second")); // Duplicate id.

    ASSERT_TRUE(WriteTomlString(path, toml));

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

    // Write a TOML file with schema_version one higher than current — must reset.
    const QString toml = QStringLiteral("schema_version = %1\n"
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
                                        "audio_bitrate_kbps = 160\n"
                                        "opus_frame_duration = \"20ms\"\n"
                                        "opus_complexity = 10\n"
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
                             .arg(kPresetSchemaVersion + 1); // Wrong version!

    ASSERT_TRUE(WriteTomlString(path, toml));

    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    EXPECT_TRUE(state.was_reset);

    CleanupFile(path);
}

// ===========================================================================
// New: TOML on-disk human-readability round-trip
// ===========================================================================

TEST(RecordingPresetStore, TomlOnDisk_IsValidTomlWithExpectedKeys) {
    const QString path = UniqueTempPath();

    RecordingPreset p = MakeDefaultPreset();

    {
        RecordingPresetStore store(path);
        store.Save({p}, std::string(kDefaultPresetId), std::string(kDefaultPresetId));
    }

    // Re-parse the file independently and verify expected keys exist.
    ASSERT_TRUE(QFileInfo::exists(path));
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray raw = f.readAll();
    f.close();

    // Must be non-empty and contain expected TOML keys.
    EXPECT_FALSE(raw.isEmpty());
    const QString content = QString::fromUtf8(raw);
    EXPECT_TRUE(content.contains(QStringLiteral("schema_version")));
    EXPECT_TRUE(content.contains(QStringLiteral("[[presets]]")));
    EXPECT_TRUE(content.contains(QStringLiteral("[presets.audio]")));
    EXPECT_TRUE(content.contains(QStringLiteral("[presets.webcam]")));

    CleanupFile(path);
}

// ===========================================================================
// New: Malformed TOML → Load returns reset; no crash
// ===========================================================================

TEST(RecordingPresetStore, MalformedToml_Load_ReturnsReset_NoCrash) {
    const QString path = UniqueTempPath();

    // Syntactically broken TOML.
    ASSERT_TRUE(WriteTomlString(path, QStringLiteral("schema_version = !!INVALID[[[TOML\x00garbage")));

    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    EXPECT_TRUE(state.was_reset);
    ASSERT_EQ(state.presets.size(), 1u);
    EXPECT_EQ(state.selected_id, std::string(kDefaultPresetId));

    CleanupFile(path);
}

// ===========================================================================
// New: Incompatible schema (old version) → Load returns reset
// ===========================================================================

TEST(RecordingPresetStore, OldSchemaVersion_Load_ReturnsReset) {
    const QString path = UniqueTempPath();

    const QString toml = QStringLiteral("schema_version = %1\n"
                                        "selected_id = \"preset.default\"\n"
                                        "default_id  = \"preset.default\"\n")
                             .arg(kPresetSchemaVersion - 1);

    ASSERT_TRUE(WriteTomlString(path, toml));

    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    EXPECT_TRUE(state.was_reset);

    CleanupFile(path);
}

// ===========================================================================
// Chroma key — color mode + spill round-trip
// ===========================================================================

TEST(RecordingPresetStore, ChromaColorMode_RoundTrip_Custom) {
    const QString path = UniqueTempPath();

    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "Chroma Custom";
    p.config = MakeDefaultPreset().config;
    p.config.webcam.chroma_key.enabled = true;
    p.config.webcam.chroma_key.color_mode = WebcamChromaKeyColorMode::Custom;
    p.config.webcam.chroma_key.custom_r = 128;
    p.config.webcam.chroma_key.custom_g = 64;
    p.config.webcam.chroma_key.custom_b = 200;
    p.config.webcam.chroma_key.tolerance = 0.35f;
    p.config.webcam.chroma_key.softness = 0.12f;
    p.config.webcam.chroma_key.spill_reduction = 0.45f;

    {
        RecordingPresetStore store(path);
        store.Save({p}, p.id, p.id);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        ASSERT_FALSE(state.was_reset);
        ASSERT_EQ(state.presets.size(), 1u);
        const auto& ck = state.presets[0].config.webcam.chroma_key;
        EXPECT_TRUE(ck.enabled);
        EXPECT_EQ(ck.color_mode, WebcamChromaKeyColorMode::Custom);
        EXPECT_EQ(ck.custom_r, 128u);
        EXPECT_EQ(ck.custom_g, 64u);
        EXPECT_EQ(ck.custom_b, 200u);
        EXPECT_NEAR(ck.tolerance, 0.35f, 1e-4f);
        EXPECT_NEAR(ck.softness, 0.12f, 1e-4f);
        EXPECT_NEAR(ck.spill_reduction, 0.45f, 1e-4f);
    }

    CleanupFile(path);
}

TEST(RecordingPresetStore, ChromaColorMode_RoundTrip_Magenta) {
    const QString path = UniqueTempPath();

    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "Chroma Magenta";
    p.config = MakeDefaultPreset().config;
    p.config.webcam.chroma_key.color_mode = WebcamChromaKeyColorMode::Magenta;
    p.config.webcam.chroma_key.spill_reduction = 0.20f;

    {
        RecordingPresetStore store(path);
        store.Save({p}, p.id, p.id);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        ASSERT_FALSE(state.was_reset);
        ASSERT_EQ(state.presets.size(), 1u);
        const auto& ck = state.presets[0].config.webcam.chroma_key;
        EXPECT_EQ(ck.color_mode, WebcamChromaKeyColorMode::Magenta);
        EXPECT_NEAR(ck.spill_reduction, 0.20f, 1e-4f);
    }

    CleanupFile(path);
}

} // namespace
} // namespace exosnap
