#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    // Unique temp dir per test process (gtest_discover_tests = one process per
    // test); a shared fixed name races under ctest -j.
    static QTemporaryDir s_dir;
    static int s_counter = 0;
    return s_dir.filePath(QStringLiteral("exosnap_test_presets_%1.toml").arg(++s_counter));
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
    p.config.capture.region_x_norm = 0.1f;
    p.config.capture.region_y_norm = 0.2f;
    p.config.capture.region_w_norm = 0.5f;
    p.config.capture.region_h_norm = 0.4f;
    p.config.capture.region_display_id.device_path = "monitor-0";
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
    p.config.webcam.opacity = 0.35f;
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

    {
        RecordingPresetStore store(path);
        store.Save({r1, r2, r3}, sel_id, MakeDefaultPreset().config);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.repaired);
        ASSERT_EQ(state.user_presets.size(), 3u);
        EXPECT_EQ(state.selected_id, sel_id);

        // Match by id (order is preserved by Save/Load).
        const auto find = [&](const std::string& id) -> const RecordingPreset* {
            for (const auto& p : state.user_presets)
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

        // Webcam PiP opacity — persisted explicitly since NormalizedConfigEquals
        // is not the target of this proof; check the round-tripped value directly.
        EXPECT_FLOAT_EQ(loaded_r3->config.webcam.opacity, 0.35f);
    }

    CleanupFile(path);
}

// ===========================================================================
// Stable display identity sub-table round-trips (schema 25)
// ===========================================================================

TEST(RecordingPresetStore, StableDisplayId_SubTable_RoundTrips) {
    const QString path = UniqueTempPath();

    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "Stable id";
    p.config = MakeDefaultPreset().config;
    p.config.capture.kind = PresetCaptureKind::Display;
    p.config.capture.display_id.device_path = "\\\\?\\DISPLAY#GSM5B09#5&abcd&UID4352#{guid}";
    p.config.capture.display_id.edid_vendor = "GSM";
    p.config.capture.display_id.edid_product = 23305;
    p.config.capture.display_id.serial = "PANEL-SERIAL-1";
    p.config.capture.display_id.friendly_name = "LG HDR 4K";
    p.config.capture.display_id.gdi_name = "\\\\.\\DISPLAY6";
    p.config.capture.display_id.seq_hint = 2;

    {
        RecordingPresetStore store(path);
        store.Save({p}, p.id, MakeDefaultPreset().config);
    }
    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.repaired);
        ASSERT_EQ(state.user_presets.size(), 1u);
        const auto& id = state.user_presets[0].config.capture.display_id;
        EXPECT_EQ(id.device_path, p.config.capture.display_id.device_path);
        EXPECT_EQ(id.edid_vendor, "GSM");
        EXPECT_EQ(id.edid_product, 23305u);
        EXPECT_EQ(id.serial, "PANEL-SERIAL-1");
        EXPECT_EQ(id.friendly_name, "LG HDR 4K");
        EXPECT_EQ(id.gdi_name, "\\\\.\\DISPLAY6");
        EXPECT_EQ(id.seq_hint, 2);
    }

    CleanupFile(path);
}

// ===========================================================================
// Video bit depth persists (0.7.0 — S7, schema 17)
// ===========================================================================

TEST(RecordingPresetStore, VideoBitDepthPersists_HevcTenBit) {
    const QString path = UniqueTempPath();

    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "HEVC 10-bit";
    p.config = MakeDefaultPreset().config;
    // 10-bit is only valid for HEVC/AV1 — use MKV + HEVC so sanitize keeps it.
    p.config.output.container = capability::Container::Matroska;
    p.config.output.video_codec = capability::VideoCodec::HevcNvenc;
    p.config.output.audio_codec = capability::AudioCodec::Opus;
    p.config.output.bit_depth = capability::BitDepth::Bit10;

    {
        RecordingPresetStore store(path);
        store.Save({p}, p.id, p.config);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.repaired);
        ASSERT_EQ(state.user_presets.size(), 1u);
        EXPECT_EQ(state.user_presets[0].config.output.bit_depth, capability::BitDepth::Bit10);
        EXPECT_EQ(state.user_presets[0].config.output.video_codec, capability::VideoCodec::HevcNvenc);
    }

    CleanupFile(path);
}

// Default preset (and any non-HEVC/AV1 codec) round-trips as 8-bit.
TEST(RecordingPresetStore, VideoBitDepthPersists_DefaultEightBit) {
    const QString path = UniqueTempPath();

    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "Default depth";
    p.config = MakeDefaultPreset().config; // AV1, Bit8 by default

    {
        RecordingPresetStore store(path);
        store.Save({p}, p.id, p.config);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.repaired);
        ASSERT_EQ(state.user_presets.size(), 1u);
        EXPECT_EQ(state.user_presets[0].config.output.bit_depth, capability::BitDepth::Bit8);
    }

    CleanupFile(path);
}

// ===========================================================================
// Colour range persists (introduced schema 18; default flipped Full->Limited
// with the schema-20 migration by fix/color-range-signaling — see ADR 0032)
// ===========================================================================

// Default preset round-trips as Limited range (the current default).
TEST(RecordingPresetStore, ColorRangePersists_DefaultLimited) {
    const QString path = UniqueTempPath();

    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "Default range";
    p.config = MakeDefaultPreset().config; // Limited by default

    {
        RecordingPresetStore store(path);
        store.Save({p}, p.id, p.config);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.repaired);
        ASSERT_EQ(state.user_presets.size(), 1u);
        EXPECT_EQ(state.user_presets[0].config.output.color_range, capability::ColorRange::Limited);
    }

    CleanupFile(path);
}

// An explicit Full selection (the opt-in) survives the TOML round-trip too.
TEST(RecordingPresetStore, ColorRangePersists_Full) {
    const QString path = UniqueTempPath();

    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "Full range";
    p.config = MakeDefaultPreset().config;
    p.config.output.color_range = capability::ColorRange::Full;

    {
        RecordingPresetStore store(path);
        store.Save({p}, p.id, p.config);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.repaired);
        ASSERT_EQ(state.user_presets.size(), 1u);
        EXPECT_EQ(state.user_presets[0].config.output.color_range, capability::ColorRange::Full);
    }

    CleanupFile(path);
}

// ===========================================================================
// NVENC encoder preset (P1..P7) persists (NVENC-PRESET-R1). Additive field —
// schema stays 20; a preset file missing the key loads the struct default (P4).
// ===========================================================================

TEST(RecordingPresetStore, NvencPresetPersists_DefaultP4) {
    const QString path = UniqueTempPath();

    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "Default preset";
    p.config = MakeDefaultPreset().config; // P4 by default

    {
        RecordingPresetStore store(path);
        store.Save({p}, p.id, p.config);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.repaired);
        ASSERT_EQ(state.user_presets.size(), 1u);
        EXPECT_EQ(state.user_presets[0].config.output.nvenc_preset, recorder_core::NvencPreset::P4);
    }

    CleanupFile(path);
}

TEST(RecordingPresetStore, NvencPresetPersists_P7) {
    const QString path = UniqueTempPath();

    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "P7 preset";
    p.config = MakeDefaultPreset().config;
    p.config.output.nvenc_preset = recorder_core::NvencPreset::P7;

    {
        RecordingPresetStore store(path);
        store.Save({p}, p.id, p.config);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.repaired);
        ASSERT_EQ(state.user_presets.size(), 1u);
        EXPECT_EQ(state.user_presets[0].config.output.nvenc_preset, recorder_core::NvencPreset::P7);
    }

    CleanupFile(path);
}

// ===========================================================================
// HDR handling mode persists (preset schema 20->21).
// ===========================================================================

TEST(RecordingPresetStore, HdrModePersists_DefaultTonemapSdr) {
    const QString path = UniqueTempPath();

    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "Default preset";
    p.config = MakeDefaultPreset().config; // TonemapSdr by default

    {
        RecordingPresetStore store(path);
        store.Save({p}, p.id, p.config);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.repaired);
        ASSERT_EQ(state.user_presets.size(), 1u);
        EXPECT_EQ(state.user_presets[0].config.output.hdr_mode, recorder_core::HdrMode::TonemapSdr);
    }

    CleanupFile(path);
}

TEST(RecordingPresetStore, HdrModePersists_Hdr10) {
    const QString path = UniqueTempPath();

    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "HDR10 preset";
    p.config = MakeDefaultPreset().config;
    p.config.output.hdr_mode = recorder_core::HdrMode::Hdr10;

    {
        RecordingPresetStore store(path);
        store.Save({p}, p.id, p.config);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.repaired);
        ASSERT_EQ(state.user_presets.size(), 1u);
        EXPECT_EQ(state.user_presets[0].config.output.hdr_mode, recorder_core::HdrMode::Hdr10);
    }

    CleanupFile(path);
}

// ===========================================================================
// Schema v19 -> v20 colour-range migration (fix/color-range-signaling)
//
// Under schema <=19 "full" was the MATERIALIZED old code default, not an
// informed user choice (the colour-range combo had a hydration bug and always
// displayed "Full (PC)" regardless of the stored value), so Load() rewrites
// schema-19-and-older color_range=="full" to "limited" as a field-wise repair
// instead of resetting the store. A schema-20-and-newer file with explicit
// "full" is a deliberate post-flip opt-in and is respected.
// ===========================================================================

namespace {

// Minimal single-preset TOML with parametrized schema version and colour range.
QString MakeSinglePresetToml(int schema_version, const QString& color_range) {
    return QStringLiteral("schema_version = %1\n"
                          "selected_id = \"preset.migrate0011\"\n"
                          "\n"
                          "[[presets]]\n"
                          "id   = \"preset.migrate0011\"\n"
                          "name = \"User Preset\"\n"
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
                          "color_range = \"%2\"\n"
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
        .arg(schema_version)
        .arg(color_range);
}

} // namespace

// NVENC-PRESET-R1 additive-load proof: MakeSinglePresetToml() writes a schema-current
// preset file that never had an "nvenc_preset" key (it predates this feature, same
// as it never had a "bit_depth" key). Loading it must NOT reset the store and must
// leave the field at its struct default (P4) — proving the additive-TOML contract
// ("missing key -> P4, no schema bump") holds against the real Load() code path.
TEST(RecordingPresetStore, NvencPresetMissingKey_DefaultsToP4) {
    const QString path = UniqueTempPath();
    ASSERT_TRUE(WriteTomlString(path, MakeSinglePresetToml(kPresetSchemaVersion, QStringLiteral("limited"))));

    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    EXPECT_FALSE(state.repaired);
    ASSERT_EQ(state.user_presets.size(), 1u);
    EXPECT_EQ(state.user_presets[0].config.output.nvenc_preset, recorder_core::NvencPreset::P4);

    CleanupFile(path);
}

// A present-but-invalid value ("p9" — not a real NVENC preset) falls back to the
// struct default (P4) without resetting the store, same as every other unknown
// enum string in the loader.
TEST(RecordingPresetStore, NvencPresetInvalidValue_DefaultsToP4_NoReset) {
    const QString path = UniqueTempPath();
    QString toml = MakeSinglePresetToml(kPresetSchemaVersion, QStringLiteral("limited"));
    toml.replace(QStringLiteral("color_range = \"limited\"\n"),
                 QStringLiteral("color_range = \"limited\"\nnvenc_preset = \"p9\"\n"));
    ASSERT_TRUE(toml.contains(QStringLiteral("nvenc_preset = \"p9\"")));
    ASSERT_TRUE(WriteTomlString(path, toml));

    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    EXPECT_FALSE(state.repaired);
    ASSERT_EQ(state.user_presets.size(), 1u);
    EXPECT_EQ(state.user_presets[0].config.output.nvenc_preset, recorder_core::NvencPreset::P4);

    CleanupFile(path);
}

// A schema-current file that never had an "hdr_mode" key leaves the field
// at its struct default (TonemapSdr) instead of resetting the store — same
// additive-TOML contract as NvencPresetMissingKey_DefaultsToP4 above.
TEST(RecordingPresetStore, HdrModeMissingKey_DefaultsToTonemapSdr) {
    const QString path = UniqueTempPath();
    ASSERT_TRUE(WriteTomlString(path, MakeSinglePresetToml(kPresetSchemaVersion, QStringLiteral("limited"))));

    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    EXPECT_FALSE(state.repaired);
    ASSERT_EQ(state.user_presets.size(), 1u);
    EXPECT_EQ(state.user_presets[0].config.output.hdr_mode, recorder_core::HdrMode::TonemapSdr);

    CleanupFile(path);
}

// A present-but-invalid value ("hdr9000" — not a real HdrMode) falls back to
// the struct default (TonemapSdr) without resetting the store.
TEST(RecordingPresetStore, HdrModeInvalidValue_DefaultsToTonemapSdr_NoReset) {
    const QString path = UniqueTempPath();
    QString toml = MakeSinglePresetToml(kPresetSchemaVersion, QStringLiteral("limited"));
    toml.replace(QStringLiteral("color_range = \"limited\"\n"),
                 QStringLiteral("color_range = \"limited\"\nhdr_mode = \"hdr9000\"\n"));
    ASSERT_TRUE(toml.contains(QStringLiteral("hdr_mode = \"hdr9000\"")));
    ASSERT_TRUE(WriteTomlString(path, toml));

    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    EXPECT_FALSE(state.repaired);
    ASSERT_EQ(state.user_presets.size(), 1u);
    EXPECT_EQ(state.user_presets[0].config.output.hdr_mode, recorder_core::HdrMode::TonemapSdr);

    CleanupFile(path);
}

// Additive-load proof (mirrors HdrModeMissingKey_DefaultsToTonemapSdr above):
// MakeSinglePresetToml() writes a [presets.webcam] table that never had an
// "opacity" key (it predates this feature). Loading it must NOT reset the
// store and must leave the field at its struct default (1.0f, fully opaque).
TEST(RecordingPresetStore, WebcamOpacityMissingKey_DefaultsTo1) {
    const QString path = UniqueTempPath();
    ASSERT_TRUE(WriteTomlString(path, MakeSinglePresetToml(kPresetSchemaVersion, QStringLiteral("limited"))));

    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    EXPECT_FALSE(state.repaired);
    ASSERT_EQ(state.user_presets.size(), 1u);
    EXPECT_FLOAT_EQ(state.user_presets[0].config.webcam.opacity, 1.0f);

    CleanupFile(path);
}

// A schema-22 file — one version behind current — is kept field-wise (not
// reset) per pre-1.0 policy. Every field still parses cleanly, so this is a
// routine version re-stamp, not a repair — the flag stays false.
TEST(RecordingPresetStore, SchemaV22_OneVersionBehindCurrent_KeepsData_NotRepaired) {
    const QString path = UniqueTempPath();
    ASSERT_TRUE(WriteTomlString(path, MakeSinglePresetToml(kPresetSchemaVersion - 1, QStringLiteral("limited"))));

    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    EXPECT_FALSE(state.repaired);
    ASSERT_EQ(state.user_presets.size(), 1u);
    EXPECT_EQ(state.user_presets[0].id, "preset.migrate0011");

    CleanupFile(path);
}

// A schema-19 file with the materialized old default ("full") keeps the user
// preset and migrates the colour range to Limited. Every field still parses
// cleanly and nothing is dropped, so this is not a repair.
TEST(RecordingPresetStore, MigrationV19_MaterializedFullBecomesLimited) {
    const QString path = UniqueTempPath();
    ASSERT_TRUE(WriteTomlString(path, MakeSinglePresetToml(19, QStringLiteral("full"))));

    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    EXPECT_FALSE(state.repaired);
    ASSERT_EQ(state.user_presets.size(), 1u);
    EXPECT_EQ(state.user_presets[0].id, "preset.migrate0011");
    EXPECT_EQ(state.user_presets[0].name, "User Preset");
    EXPECT_EQ(state.user_presets[0].config.output.color_range, capability::ColorRange::Limited)
        << "materialized old-default \"full\" must be rewritten to Limited";

    CleanupFile(path);
}

// A schema-19 file with "limited" stays Limited (nothing to migrate, nothing
// dropped — not a repair).
TEST(RecordingPresetStore, MigrationV19_LimitedStaysLimited) {
    const QString path = UniqueTempPath();
    ASSERT_TRUE(WriteTomlString(path, MakeSinglePresetToml(19, QStringLiteral("limited"))));

    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    EXPECT_FALSE(state.repaired);
    ASSERT_EQ(state.user_presets.size(), 1u);
    EXPECT_EQ(state.user_presets[0].config.output.color_range, capability::ColorRange::Limited);

    CleanupFile(path);
}

// A schema-current file with explicit "full" is a deliberate post-flip
// opt-in and must be respected — the migration only applies at or below
// kPresetSchemaColorRangeMigratedThrough.
TEST(RecordingPresetStore, MigrationV20_ExplicitFullRespected) {
    const QString path = UniqueTempPath();
    ASSERT_TRUE(WriteTomlString(path, MakeSinglePresetToml(kPresetSchemaVersion, QStringLiteral("full"))));

    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    EXPECT_FALSE(state.repaired);
    ASSERT_EQ(state.user_presets.size(), 1u);
    EXPECT_EQ(state.user_presets[0].config.output.color_range, capability::ColorRange::Full)
        << "explicit Full under the current schema is a deliberate opt-in and must survive";

    CleanupFile(path);
}

// Idempotence: migrated state saved back (as the current schema) and
// reloaded stays Limited — the migration is one-shot and a second load
// changes nothing.
TEST(RecordingPresetStore, MigrationV19_IdempotentAfterSaveReload) {
    const QString path = UniqueTempPath();
    ASSERT_TRUE(WriteTomlString(path, MakeSinglePresetToml(19, QStringLiteral("full"))));

    RecordingPresetStore store(path);
    const PersistedPresetState migrated = store.Load();
    ASSERT_EQ(migrated.user_presets.size(), 1u);
    ASSERT_EQ(migrated.user_presets[0].config.output.color_range, capability::ColorRange::Limited);

    // Persist the migrated state (writes the current schema version).
    store.Save(migrated.user_presets, migrated.selected_id, MakeDefaultPreset().config);

    const PersistedPresetState reloaded = store.Load();
    EXPECT_FALSE(reloaded.repaired);
    ASSERT_EQ(reloaded.user_presets.size(), 1u);
    EXPECT_EQ(reloaded.user_presets[0].id, "preset.migrate0011");
    EXPECT_EQ(reloaded.user_presets[0].config.output.color_range, capability::ColorRange::Limited);

    CleanupFile(path);
}

// Schema versions well below the current one still get the same field-wise
// treatment (kept, not reset, not flagged as repaired); a schema this old
// also falls at-or-below the color-range migration ceiling, so "full" is
// rewritten to Limited too.
TEST(RecordingPresetStore, Migration_Schema18_FieldwiseKept_AndColorRangeRewritten) {
    const QString path = UniqueTempPath();
    ASSERT_TRUE(WriteTomlString(path, MakeSinglePresetToml(18, QStringLiteral("full"))));

    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    EXPECT_FALSE(state.repaired);
    ASSERT_EQ(state.user_presets.size(), 1u);
    EXPECT_EQ(state.user_presets[0].id, "preset.migrate0011");
    EXPECT_EQ(state.user_presets[0].config.output.color_range, capability::ColorRange::Limited);

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
        store.Save({p}, p.id, p.config);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.repaired);
        ASSERT_EQ(state.user_presets.size(), 1u);
        EXPECT_EQ(state.user_presets[0].config.video.frame_rate_num, 50u);
        EXPECT_EQ(state.user_presets[0].config.video.frame_rate_den, 1u);
    }

    CleanupFile(path);
}

TEST(RecordingPresetStore, FrameRate120_PersistsThroughStore) {
    const QString path = UniqueTempPath();

    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "120fps Preset";
    p.config = MakeDefaultPreset().config;
    p.config.video.frame_rate_num = 120;
    p.config.video.frame_rate_den = 1;

    {
        RecordingPresetStore store(path);
        store.Save({p}, p.id, p.config);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.repaired);
        ASSERT_EQ(state.user_presets.size(), 1u);
        EXPECT_EQ(state.user_presets[0].config.video.frame_rate_num, 120u);
        EXPECT_EQ(state.user_presets[0].config.video.frame_rate_den, 1u);
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
        store.Save({p}, p.id, p.config);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.repaired);
        ASSERT_EQ(state.user_presets.size(), 1u);
        EXPECT_EQ(state.user_presets[0].config.output.resolution.mode, OutputResolutionMode::QHD1440);
        EXPECT_EQ(state.user_presets[0].config.video.frame_rate_num, 30u);
        EXPECT_FALSE(state.user_presets[0].config.video.cfr);
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
        store.Save({p}, p.id, p.config);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.repaired);
        ASSERT_EQ(state.user_presets.size(), 1u);
        EXPECT_EQ(state.user_presets[0].config.output.resolution.mode, OutputResolutionMode::Custom);
        EXPECT_EQ(state.user_presets[0].config.output.resolution.custom_width, 2560u);
        EXPECT_EQ(state.user_presets[0].config.output.resolution.custom_height, 1440u);
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
        store.Save({p}, p.id, p.config);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.repaired);
        ASSERT_EQ(state.user_presets.size(), 1u);
        EXPECT_EQ(state.user_presets[0].config.output.split.mode, SplitRecordingMode::Custom);
        EXPECT_EQ(state.user_presets[0].config.output.split.custom_minutes, 45u);
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
        store.Save({p}, p.id, p.config);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        ASSERT_EQ(state.user_presets.size(), 1u);
        EXPECT_EQ(state.user_presets[0].config.output.split.mode, SplitRecordingMode::Every30Min);
    }

    CleanupFile(path);
}

// SPLIT-BY-SIZE-R1: size threshold survives save→load roundtrip.
TEST(RecordingPresetStore, SplitSizeSettingsPersist_Custom) {
    const QString path = UniqueTempPath();

    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "Size Split Preset";
    p.config = MakeDefaultPreset().config;
    p.config.output.split.size_mode = SplitSizeMode::Custom;
    p.config.output.split.custom_size_mb = 512;

    {
        RecordingPresetStore store(path);
        store.Save({p}, p.id, p.config);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.repaired);
        ASSERT_EQ(state.user_presets.size(), 1u);
        EXPECT_EQ(state.user_presets[0].config.output.split.size_mode, SplitSizeMode::Custom);
        EXPECT_EQ(state.user_presets[0].config.output.split.custom_size_mb, 512u);
    }

    CleanupFile(path);
}

TEST(RecordingPresetStore, SplitSizeSettingsPersist_Off) {
    const QString path = UniqueTempPath();

    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "Size Split Off Preset";
    p.config = MakeDefaultPreset().config;
    p.config.output.split.size_mode = SplitSizeMode::Off;

    {
        RecordingPresetStore store(path);
        store.Save({p}, p.id, p.config);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        ASSERT_EQ(state.user_presets.size(), 1u);
        EXPECT_EQ(state.user_presets[0].config.output.split.size_mode, SplitSizeMode::Off);
    }

    CleanupFile(path);
}

// ===========================================================================
// Absent file → empty defaults, not a repair (first run)
// ===========================================================================

TEST(RecordingPresetStore, AbsentFile_ReturnsEmptyDefaults_NotRepaired) {
    // A path that does not exist.
    const QString path = UniqueTempPath(); // created unique, but never written.

    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    EXPECT_FALSE(state.repaired);
    EXPECT_TRUE(state.user_presets.empty());
    EXPECT_EQ(state.selected_id, std::string(kDefaultPresetId));
    EXPECT_FALSE(state.live.has_value());
}

// ===========================================================================
// Malformed item skipped — one valid + one with empty id
// ===========================================================================

TEST(RecordingPresetStore, MalformedItem_EmptyId_Skipped_ValidKept) {
    const QString path = UniqueTempPath();

    // Hand-write a TOML fixture: one valid preset + one with empty id.
    const QString toml = QStringLiteral("schema_version = %1\n"
                                        "selected_id = \"preset.aabbccddeeff0011\"\n"
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
    EXPECT_TRUE(state.repaired); // the empty-id item was dropped
    ASSERT_EQ(state.user_presets.size(), 1u);
    EXPECT_EQ(state.user_presets[0].id, "preset.aabbccddeeff0011");
    EXPECT_EQ(state.user_presets[0].name, "Good Item");

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
                                        "\n")
                             .arg(kPresetSchemaVersion) +
                         preset_block.arg(QStringLiteral("First")) +
                         preset_block.arg(QStringLiteral("Second")); // Duplicate id.

    ASSERT_TRUE(WriteTomlString(path, toml));

    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    EXPECT_TRUE(state.repaired); // the duplicate later occurrence was dropped
    ASSERT_EQ(state.user_presets.size(), 1u);
    EXPECT_EQ(state.user_presets[0].name, "First");

    CleanupFile(path);
}

// ===========================================================================
// Invalid selectedId falls back to the built-in Default preset
// ===========================================================================

TEST(RecordingPresetStore, InvalidSelectedId_FallsBackToDefaultPreset) {
    const QString path = UniqueTempPath();

    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "Solo Preset";
    p.config = MakeDefaultPreset().config;

    {
        RecordingPresetStore store(path);
        store.Save({p}, "preset.doesnotexist1234567", p.config); // selected invalid
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.repaired);
        EXPECT_EQ(state.selected_id, std::string(kDefaultPresetId)); // falls back to the built-in Default
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
        store.Save({p1, p2, p3}, p1.id, p1.config);
    }

    // Save only p1.
    {
        RecordingPresetStore store(path);
        store.Save({p1}, p1.id, p1.config);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.repaired);
        ASSERT_EQ(state.user_presets.size(), 1u);
        EXPECT_EQ(state.user_presets[0].id, p1.id);
    }

    CleanupFile(path);
}

// ===========================================================================
// Empty-path store — Load returns defaults, no crash; Save no-op
//
// These two are the ONLY tests allowed to use an empty path — they exist
// purely for the no-crash contract. Every other test runs against a real
// temp file so it exercises the same parse path MainWindow's ctor hits.
// ===========================================================================

TEST(RecordingPresetStore, EmptyPath_Load_SeedsDefault_NoCrash) {
    const QString empty_path;
    RecordingPresetStore store(empty_path);
    const PersistedPresetState state = store.Load();
    EXPECT_FALSE(state.repaired);
    EXPECT_TRUE(state.user_presets.empty());
    EXPECT_EQ(state.selected_id, std::string(kDefaultPresetId));
    EXPECT_FALSE(state.live.has_value());
}

TEST(RecordingPresetStore, EmptyPath_Save_NoCrash) {
    const QString empty_path;
    RecordingPresetStore store(empty_path);
    // Should not crash or throw, and an intentional no-op path is a success.
    QString err;
    EXPECT_TRUE(store.Save({MakeDefaultPreset()}, std::string(kDefaultPresetId), MakeDefaultPreset().config, &err));
    EXPECT_TRUE(err.isEmpty());
}

// ===========================================================================
// Save() reports success/failure instead of swallowing write errors
// ===========================================================================

TEST(RecordingPresetStore, Save_Success_ReturnsTrueWithNoError) {
    const QString path = UniqueTempPath();
    RecordingPresetStore store(path);
    QString err;
    const RecordingPreset p = MakeDefaultPreset();
    EXPECT_TRUE(store.Save({p}, p.id, p.config, &err));
    EXPECT_TRUE(err.isEmpty());
    EXPECT_TRUE(QFileInfo::exists(path));
}

TEST(RecordingPresetStore, Save_WriteFailure_ReturnsFalseWithMessage) {
    // Force the atomic-rename commit to fail: create a directory AT the target
    // path so QSaveFile can write its temp file next to it but can never rename
    // that temp file onto an existing directory.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("presets.toml"));
    ASSERT_TRUE(QDir().mkpath(path));

    RecordingPresetStore store(path);
    QString err;
    const RecordingPreset p = MakeDefaultPreset();
    EXPECT_FALSE(store.Save({p}, p.id, p.config, &err));
    EXPECT_FALSE(err.isEmpty());
}

// ===========================================================================
// Schema version mismatch → field-wise kept, not a reset, not a repair
// ===========================================================================

TEST(RecordingPresetStore, WrongSchemaVersion_KeepsData_NotRepaired) {
    const QString path = UniqueTempPath();

    // Write a TOML file with schema_version one higher than current — must be
    // repaired field-wise (data kept), not reset.
    const QString toml = QStringLiteral("schema_version = %1\n"
                                        "selected_id = \"preset.userpreset0001\"\n"
                                        "\n"
                                        "[[presets]]\n"
                                        "id   = \"preset.userpreset0001\"\n"
                                        "name = \"Mine\"\n"
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
    EXPECT_FALSE(state.repaired);
    ASSERT_EQ(state.user_presets.size(), 1u);
    EXPECT_EQ(state.user_presets[0].id, "preset.userpreset0001");

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
        store.Save({}, std::string(kDefaultPresetId), p.config);
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
    EXPECT_TRUE(content.contains(QStringLiteral("[live]")));
    EXPECT_TRUE(content.contains(QStringLiteral("[live.audio]")));
    EXPECT_TRUE(content.contains(QStringLiteral("[live.webcam]")));
    EXPECT_FALSE(content.contains(QStringLiteral("default_id")));

    CleanupFile(path);
}

// ===========================================================================
// New: Malformed TOML → Load returns defaults, flags repaired; no crash
// ===========================================================================

TEST(RecordingPresetStore, MalformedToml_Load_ReturnsDefaults_FlagsRepaired_NoCrash) {
    const QString path = UniqueTempPath();

    // Syntactically broken TOML.
    ASSERT_TRUE(WriteTomlString(path, QStringLiteral("schema_version = !!INVALID[[[TOML\x00garbage")));

    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    EXPECT_TRUE(state.repaired);
    ASSERT_TRUE(state.user_presets.empty());
    EXPECT_EQ(state.selected_id, std::string(kDefaultPresetId));

    CleanupFile(path);
}

// ===========================================================================
// New: Incompatible schema (old version) → field-wise kept, not a reset,
// not a repair (nothing parsed invalidly and nothing was dropped)
// ===========================================================================

TEST(RecordingPresetStore, OldSchemaVersion_Load_KeepsData_NotRepaired) {
    const QString path = UniqueTempPath();

    // kPresetSchemaVersion - 2: genuinely below the migration ceiling too, so
    // it is kept the same way SchemaV22_OneVersionBehindCurrent is, plus the
    // color-range rewrite (covered separately by Migration_Schema18_*). No
    // item is dropped, so the flag stays false.
    const QString toml = QStringLiteral("schema_version = %1\n"
                                        "selected_id = \"preset.default\"\n")
                             .arg(kPresetSchemaVersion - 2);

    ASSERT_TRUE(WriteTomlString(path, toml));

    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    EXPECT_FALSE(state.repaired);
    EXPECT_TRUE(state.user_presets.empty());                     // no [[presets]] array in this minimal fixture
    EXPECT_EQ(state.selected_id, std::string(kDefaultPresetId)); // "preset.default" is a built-in

    CleanupFile(path);
}

// ===========================================================================
// Frame pacing — default Smooth + Newest round-trip (ADR 0035, schema 19)
// ===========================================================================

TEST(RecordingPresetStore, FramePacingRoundtrips) {
    using recorder_core::FramePacingMode;
    const QString path = UniqueTempPath();

    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = "Pacing Test";
    p.config = MakeDefaultPreset().config;

    // Default must be Smooth.
    EXPECT_EQ(p.config.video.frame_pacing, FramePacingMode::Smooth);

    // Set Newest and verify it survives a save → load roundtrip.
    p.config.video.frame_pacing = FramePacingMode::Newest;

    {
        RecordingPresetStore store(path);
        store.Save({p}, p.id, p.config);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        EXPECT_FALSE(state.repaired);
        ASSERT_EQ(state.user_presets.size(), 1u);
        EXPECT_EQ(state.user_presets[0].config.video.frame_pacing, FramePacingMode::Newest);
    }

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
        store.Save({p}, p.id, p.config);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        ASSERT_FALSE(state.repaired);
        ASSERT_EQ(state.user_presets.size(), 1u);
        const auto& ck = state.user_presets[0].config.webcam.chroma_key;
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
        store.Save({p}, p.id, p.config);
    }

    {
        RecordingPresetStore store(path);
        const PersistedPresetState state = store.Load();
        ASSERT_FALSE(state.repaired);
        ASSERT_EQ(state.user_presets.size(), 1u);
        const auto& ck = state.user_presets[0].config.webcam.chroma_key;
        EXPECT_EQ(ck.color_mode, WebcamChromaKeyColorMode::Magenta);
        EXPECT_NEAR(ck.spill_reduction, 0.20f, 1e-4f);
    }

    CleanupFile(path);
}

// ===========================================================================
// [live] table (this task's new persisted unit)
// ===========================================================================

// Production call site: MainWindow::persistPresetState() (Save) and the
// MainWindow ctor preset-load block (Load).
TEST(RecordingPresetStore, LiveTable_RoundTrips) {
    const QString path = UniqueTempPath();
    RecordingPresetStore store(path);

    RecordingPresetConfig live = MakeDefaultPreset().config;
    live.video.cq = 33;
    live.output.container = capability::Container::Mp4;
    live.output.video_codec = capability::VideoCodec::H264Nvenc;
    live.output.audio_codec = capability::AudioCodec::AacMf;
    live.output.bit_depth = capability::BitDepth::Bit8;

    store.Save({}, std::string(kDefaultPresetId), live);
    const PersistedPresetState state = store.Load();

    ASSERT_TRUE(state.live.has_value());
    EXPECT_TRUE(NormalizedConfigEquals(*state.live, SanitizePresetConfig(live)));
    EXPECT_EQ(state.selected_id, kDefaultPresetId);
    EXPECT_FALSE(state.repaired);
    CleanupFile(path);
}

TEST(RecordingPresetStore, Save_ExcludesBuiltIns_KeepsUserPresets) {
    const QString path = UniqueTempPath();
    RecordingPresetStore store(path);

    RecordingPreset user = MakeRegionPreset();
    std::vector<RecordingPreset> all = MakeBuiltInPresets();
    all.push_back(user);

    store.Save(all, user.id, MakeDefaultPreset().config);
    const PersistedPresetState state = store.Load();
    ASSERT_EQ(state.user_presets.size(), 1u);
    EXPECT_EQ(state.user_presets[0].id, user.id);
    EXPECT_EQ(state.selected_id, user.id);
    CleanupFile(path);
}

// default_id must be gone from the on-disk format.
TEST(RecordingPresetStore, TomlOnDisk_HasLiveTable_NoDefaultId) {
    const QString path = UniqueTempPath();
    RecordingPresetStore store(path);
    store.Save({}, std::string(kDefaultPresetId), MakeDefaultPreset().config);

    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString text = QString::fromUtf8(f.readAll());
    EXPECT_TRUE(text.contains(QStringLiteral("[live]")));
    EXPECT_TRUE(text.contains(QStringLiteral("schema_version = 25")));
    EXPECT_FALSE(text.contains(QStringLiteral("default_id")));
    CleanupFile(path);
}

// Field-wise keep replaces the full reset on version mismatch, and a clean
// version re-stamp alone is not reported as a repair — every field here
// parses cleanly and nothing is dropped.
// Production call site: MainWindow ctor load — a schema-22 file keeps its
// user presets and live values instead of resetting.
TEST(RecordingPresetStore, SchemaMismatch_KeepsData_NotRepaired) {
    const QString path = UniqueTempPath();
    RecordingPresetStore store(path);
    RecordingPreset user = MakeRegionPreset();
    RecordingPresetConfig live = MakeDefaultPreset().config;
    live.video.cq = 27;
    store.Save({user}, user.id, live);

    // Rewrite the version stamp to the previous schema.
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    QString text = QString::fromUtf8(f.readAll());
    f.close();
    text.replace(QStringLiteral("schema_version = 25"), QStringLiteral("schema_version = 22"));
    ASSERT_TRUE(WriteTomlString(path, text));

    const PersistedPresetState state = store.Load();
    EXPECT_FALSE(state.repaired);
    ASSERT_EQ(state.user_presets.size(), 1u);
    EXPECT_EQ(state.user_presets[0].id, user.id);
    ASSERT_TRUE(state.live.has_value());
    EXPECT_EQ(state.live->video.cq, 27u);
    CleanupFile(path);
}

// Case (a) of the spec: a corrupted live VALUE is clamped field-wise; a
// missing/unreadable [live] table yields nullopt (caller boots Default).
TEST(RecordingPresetStore, LiveTable_CorruptField_ClampedNotReset) {
    const QString path = UniqueTempPath();
    ASSERT_TRUE(WriteTomlString(path, QStringLiteral("schema_version = 23\n"
                                                     "selected_id = \"preset.default\"\n"
                                                     "presets = []\n"
                                                     "[live]\n"
                                                     "countdown_seconds = 7\n" // invalid -> clamped to 0
                                                     "[live.video]\n"
                                                     "cq = 9999\n"))); // out of range -> struct default kept
    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    ASSERT_TRUE(state.live.has_value());
    EXPECT_EQ(state.live->countdown_seconds, 0);
    // Out-of-range cq is dropped by the field parser, leaving the struct
    // default (VideoSettingsModel's Balanced CQ), not the built-in Default
    // preset's High CQ — SanitizePresetConfig does not range-clamp cq itself.
    EXPECT_EQ(state.live->video.cq, recorder_core::CanonicalCq(recorder_core::NvencQualityPreset::Balanced));
    CleanupFile(path);
}

// Extends the original contract pin: a file with no [live] table and no
// stored "preset.default" entry leaves the live config absent (the caller
// boots the built-in Default) and every ordinary user preset still loads —
// nothing is dropped, so this is not a repair.
TEST(RecordingPresetStore, LiveTable_Missing_ReturnsNullopt) {
    const QString path = UniqueTempPath();
    ASSERT_TRUE(WriteTomlString(path, QStringLiteral("schema_version = 23\n"
                                                     "selected_id = \"preset.userpreset9999\"\n"
                                                     "\n"
                                                     "[[presets]]\n"
                                                     "id = \"preset.userpreset9999\"\n"
                                                     "name = \"Mine\"\n")));
    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    EXPECT_FALSE(state.live.has_value());
    EXPECT_FALSE(state.repaired);
    ASSERT_EQ(state.user_presets.size(), 1u);
    EXPECT_EQ(state.user_presets[0].id, "preset.userpreset9999");
    CleanupFile(path);
}

// ===========================================================================
// preset.default carry-over (pre-built-in-rework files)
//
// preset.default used to be an ordinary, editable preset before the built-in
// rework seeded four read-only presets under reserved ids. A pre-upgrade
// presets.toml still has no [live] table but does have a stored
// "preset.default" entry holding whatever the user last edited it to. That
// config becomes the live config once; the entry itself never survives into
// the loaded preset list (built-ins are never persisted, carried-over or
// not); and nothing is reported as repaired, since nothing the user would
// miss was lost.
// ===========================================================================

TEST(RecordingPresetStore, PreUpgradeDefaultEntry_NoLiveTable_BecomesLiveConfig) {
    const QString path = UniqueTempPath();
    ASSERT_TRUE(WriteTomlString(path, QStringLiteral("schema_version = 22\n"
                                                     "selected_id = \"preset.default\"\n"
                                                     "\n"
                                                     "[[presets]]\n"
                                                     "id = \"preset.default\"\n"
                                                     "name = \"Default\"\n"
                                                     "[presets.video]\n"
                                                     "cq = 37\n")));

    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();

    EXPECT_FALSE(state.repaired);
    ASSERT_TRUE(state.live.has_value());
    EXPECT_EQ(state.live->video.cq, 37u);

    // The preset.default entry itself does not survive into the loaded list.
    bool has_default_entry = false;
    for (const auto& p : state.user_presets) {
        if (p.id == kDefaultPresetId)
            has_default_entry = true;
    }
    EXPECT_FALSE(has_default_entry);
    EXPECT_TRUE(state.user_presets.empty());

    CleanupFile(path);
}

// When [live] IS present, it wins over a stray preset.default entry (which
// can now only arise from hand-editing, since current Save() never writes a
// built-in id) — the entry is skipped silently, same as the other three
// built-in ids always are.
TEST(RecordingPresetStore, LiveTablePresent_StrayDefaultEntry_LiveWins_NoRepair) {
    const QString path = UniqueTempPath();
    ASSERT_TRUE(WriteTomlString(path, QStringLiteral("schema_version = %1\n"
                                                     "selected_id = \"preset.default\"\n"
                                                     "\n"
                                                     "[live]\n"
                                                     "[live.video]\n"
                                                     "cq = 11\n"
                                                     "\n"
                                                     "[[presets]]\n"
                                                     "id = \"preset.default\"\n"
                                                     "name = \"Default\"\n"
                                                     "[presets.video]\n"
                                                     "cq = 44\n")
                                          .arg(kPresetSchemaVersion)));

    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();

    EXPECT_FALSE(state.repaired);
    ASSERT_TRUE(state.live.has_value());
    EXPECT_EQ(state.live->video.cq, 11u);    // [live] wins over the stray entry
    EXPECT_TRUE(state.user_presets.empty()); // the stray entry is skipped, not carried

    CleanupFile(path);
}

TEST(RecordingPresetStore, UnparseableFile_ReturnsDefaults_Repaired) {
    const QString path = UniqueTempPath();
    ASSERT_TRUE(WriteTomlString(path, QStringLiteral("this is not toml [ ===")));
    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    EXPECT_TRUE(state.user_presets.empty());
    EXPECT_FALSE(state.live.has_value());
    EXPECT_EQ(state.selected_id, kDefaultPresetId);
    EXPECT_TRUE(state.repaired);
    CleanupFile(path);
}

} // namespace
} // namespace exosnap
