// test_preset_export_import.cpp
//
// Tests for recording preset export / import (TOML round-trip).
// Covers: ExportPresetToFile, ExportAllUserPresetsToFile, ImportPresetsFromFile,
// RecordingPresetRegistry::ImportPreset.

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <QTextStream>
#include <QVector>

#include <string>
#include <vector>

#include <capability/audio_ui_state.h>
#include <recorder_core/audio_track_model.h>
#include <recorder_core/recorder_session.h>

#include "models/RecordingPreset.h"
#include "models/RecordingPresetRegistry.h"
#include "settings/RecordingPresetStore.h"

namespace exosnap {
namespace {

// ===========================================================================
// Helpers
// ===========================================================================

static int s_counter = 0;

QString UniqueTempPath(const char* suffix = ".toml") {
    const QString temp_dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    return QDir(temp_dir).filePath(QStringLiteral("exosnap_export_import_test_%1%2").arg(++s_counter).arg(suffix));
}

// Write a TOML string to a file (UTF-8).
bool WriteTomlString(const QString& path, const QString& toml_content) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts << toml_content;
    return true;
}

void CleanupFile(const QString& path) {
    if (!path.isEmpty() && QFileInfo::exists(path))
        QFile::remove(path);
}

RecordingPreset MakeCustomPreset(const std::string& name, capability::Container container,
                                 capability::VideoCodec video_codec, capability::AudioCodec audio_codec) {
    RecordingPreset p;
    p.id = GeneratePresetId();
    p.name = name;
    p.config = MakeDefaultPreset().config;
    p.config.output.container = container;
    p.config.output.video_codec = video_codec;
    p.config.output.audio_codec = audio_codec;
    p.config.countdown_seconds = 3;
    return p;
}

// ===========================================================================
// Single preset export → import round-trip
// ===========================================================================

TEST(PresetExportImport, SinglePreset_ExportImport_ConfigEqual) {
    const RecordingPreset original = MakeCustomPreset("Test MKV Preset", capability::Container::Matroska,
                                                      capability::VideoCodec::Av1Nvenc, capability::AudioCodec::Opus);

    const QString path = UniqueTempPath();

    QString err;
    ASSERT_TRUE(RecordingPresetStore::ExportPresetToFile(original, path, &err)) << err.toStdString();

    const std::vector<std::string> no_existing;
    const QVector<RecordingPreset> imported = RecordingPresetStore::ImportPresetsFromFile(path, no_existing, &err);
    ASSERT_EQ(imported.size(), 1) << "Expected 1 imported preset, err=" << err.toStdString();

    const RecordingPreset& loaded = imported[0];
    EXPECT_EQ(loaded.id, original.id);
    EXPECT_EQ(loaded.name, original.name);
    EXPECT_TRUE(NormalizedConfigEquals(loaded.config, original.config));

    CleanupFile(path);
}

// ===========================================================================
// Export-all → import-all round-trip
// ===========================================================================

TEST(PresetExportImport, ExportAll_ImportAll_CountAndConfigEqual) {
    QVector<RecordingPreset> originals;
    originals.push_back(MakeCustomPreset("Preset A", capability::Container::Matroska, capability::VideoCodec::Av1Nvenc,
                                         capability::AudioCodec::Opus));
    originals.push_back(MakeCustomPreset("Preset B", capability::Container::Mp4, capability::VideoCodec::H264Nvenc,
                                         capability::AudioCodec::AacMf));
    originals.push_back(MakeDefaultPreset());

    const QString path = UniqueTempPath();

    QString err;
    ASSERT_TRUE(RecordingPresetStore::ExportAllUserPresetsToFile(originals, path, &err)) << err.toStdString();

    const std::vector<std::string> no_existing;
    const QVector<RecordingPreset> imported = RecordingPresetStore::ImportPresetsFromFile(path, no_existing, &err);
    ASSERT_EQ(imported.size(), originals.size()) << "err=" << err.toStdString();

    for (int i = 0; i < originals.size(); ++i) {
        EXPECT_EQ(imported[i].id, originals[i].id) << "id mismatch at index " << i;
        EXPECT_EQ(imported[i].name, originals[i].name) << "name mismatch at index " << i;
        EXPECT_TRUE(NormalizedConfigEquals(imported[i].config, originals[i].config))
            << "config mismatch at index " << i;
    }

    CleanupFile(path);
}

// ===========================================================================
// Import id-collision → new id assigned, original config preserved
// ===========================================================================

TEST(PresetExportImport, IdCollision_NewIdAssigned_ConfigPreserved) {
    RecordingPreset original = MakeCustomPreset("Colliding Preset", capability::Container::Matroska,
                                                capability::VideoCodec::Av1Nvenc, capability::AudioCodec::Opus);

    const QString path = UniqueTempPath();

    QString err;
    ASSERT_TRUE(RecordingPresetStore::ExportPresetToFile(original, path, &err)) << err.toStdString();

    // Simulate the id already being in use.
    const std::vector<std::string> existing_ids = {original.id};
    const QVector<RecordingPreset> imported = RecordingPresetStore::ImportPresetsFromFile(path, existing_ids, &err);
    ASSERT_EQ(imported.size(), 1) << "err=" << err.toStdString();

    const RecordingPreset& loaded = imported[0];
    // Id must be different (fresh id generated on collision).
    EXPECT_NE(loaded.id, original.id);
    // The new id must be a valid generated id (starts with "preset.").
    EXPECT_EQ(loaded.id.substr(0, 7), std::string("preset."));
    // Config must be preserved.
    EXPECT_TRUE(NormalizedConfigEquals(loaded.config, original.config));

    CleanupFile(path);
}

// ===========================================================================
// Import of a missing file → clean failure, err set, no crash
// ===========================================================================

TEST(PresetExportImport, MissingFile_ReturnsEmpty_ErrSet) {
    const QString path = QStringLiteral("/nonexistent/path/no_such_file_12345.toml");
    QString err;
    const QVector<RecordingPreset> imported = RecordingPresetStore::ImportPresetsFromFile(path, {}, &err);
    EXPECT_TRUE(imported.isEmpty());
    EXPECT_FALSE(err.isEmpty());
}

// ===========================================================================
// Import of an empty path → clean failure
// ===========================================================================

TEST(PresetExportImport, EmptyPath_ReturnsEmpty_ErrSet) {
    QString err;
    const QVector<RecordingPreset> imported = RecordingPresetStore::ImportPresetsFromFile(QString(), {}, &err);
    EXPECT_TRUE(imported.isEmpty());
    EXPECT_FALSE(err.isEmpty());
}

// ===========================================================================
// Import of a garbage (non-TOML) file → clean failure, no crash
// ===========================================================================

TEST(PresetExportImport, GarbageFile_ReturnsEmpty_ErrSet) {
    const QString path = UniqueTempPath();
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
        // Syntactically broken — not valid TOML.
        f.write("THIS IS NOT VALID TOML [[[garbage\x00\xFF\xFE\n");
    }

    QString err;
    const QVector<RecordingPreset> imported = RecordingPresetStore::ImportPresetsFromFile(path, {}, &err);
    // TOML parse failure → empty result + err set, no crash.
    EXPECT_TRUE(imported.isEmpty());
    EXPECT_FALSE(err.isEmpty());

    CleanupFile(path);
}

// ===========================================================================
// Export to an empty path → returns false, err set
// ===========================================================================

TEST(PresetExportImport, ExportToEmptyPath_Fails) {
    const RecordingPreset p = MakeDefaultPreset();
    QString err;
    EXPECT_FALSE(RecordingPresetStore::ExportPresetToFile(p, QString(), &err));
    EXPECT_FALSE(err.isEmpty());
}

// ===========================================================================
// Schema version mismatch → best-effort parse; empty result with err
// ===========================================================================

TEST(PresetExportImport, SchemaMismatch_ErrSet_EmptyResult) {
    const QString path = UniqueTempPath();

    // Write a TOML file with a schema_version newer than kPresetSchemaVersion
    // and no presets array entries.  ImportPresetsFromFile must not crash.
    const QString toml = QStringLiteral("schema_version = %1\n"
                                        "export_kind = \"single\"\n"
                                        "presets = []\n")
                             .arg(kPresetSchemaVersion + 99);

    ASSERT_TRUE(WriteTomlString(path, toml));

    QString err;
    const QVector<RecordingPreset> imported = RecordingPresetStore::ImportPresetsFromFile(path, {}, &err);
    EXPECT_TRUE(imported.isEmpty());
    EXPECT_FALSE(err.isEmpty());

    CleanupFile(path);
}

// ===========================================================================
// New: Malformed TOML → ImportPresetsFromFile returns empty + err, no crash
// ===========================================================================

TEST(PresetExportImport, MalformedToml_ImportReturnsEmpty_ErrSet_NoCrash) {
    const QString path = UniqueTempPath();
    ASSERT_TRUE(WriteTomlString(path, QStringLiteral("schema_version = !!BAD [[[TOML garbage")));

    QString err;
    const QVector<RecordingPreset> imported = RecordingPresetStore::ImportPresetsFromFile(path, {}, &err);
    EXPECT_TRUE(imported.isEmpty());
    EXPECT_FALSE(err.isEmpty());

    CleanupFile(path);
}

// ===========================================================================
// RecordingPresetRegistry::ImportPreset — basic insertion
// ===========================================================================

TEST(PresetExportImport, Registry_ImportPreset_InsertsWithoutSelectingIt) {
    RecordingPresetRegistry reg;
    const std::string original_selected = reg.SelectedId();

    RecordingPreset imported_preset;
    imported_preset.id = GeneratePresetId();
    imported_preset.name = "Imported One";
    imported_preset.config = MakeDefaultPreset().config;

    reg.ImportPreset(imported_preset);

    EXPECT_EQ(reg.Count(), 2u);
    // Selection must not change.
    EXPECT_EQ(reg.SelectedId(), original_selected);
    // The imported preset must be findable by id.
    const RecordingPreset* found = reg.FindById(imported_preset.id);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->name, "Imported One");
}

// ===========================================================================
// Registry::ImportPreset — name deduplication
// ===========================================================================

TEST(PresetExportImport, Registry_ImportPreset_DeduplicatesName) {
    RecordingPresetRegistry reg;
    // reg already has "Default" (kDefaultPresetId preset name).  Import a
    // preset with the same name — the registry must deduplicate.
    const std::string default_name = reg.SelectedPreset().name;

    RecordingPreset dup;
    dup.id = GeneratePresetId();
    dup.name = default_name; // collides with existing
    dup.config = MakeDefaultPreset().config;

    reg.ImportPreset(dup);

    EXPECT_EQ(reg.Count(), 2u);
    const RecordingPreset* found = reg.FindById(dup.id);
    ASSERT_NE(found, nullptr);
    // Name must have been changed to avoid collision.
    EXPECT_NE(found->name, default_name);
}

// ===========================================================================
// Full pipeline: export → import → registry insert → verify config
// ===========================================================================

TEST(PresetExportImport, FullPipeline_ExportImportRegistry_ConfigEqual) {
    RecordingPreset source = MakeCustomPreset("Pipeline Preset", capability::Container::Matroska,
                                              capability::VideoCodec::Av1Nvenc, capability::AudioCodec::Opus);
    source.config.countdown_seconds = 5;
    source.config.video.frame_rate_num = 30;
    source.config.video.frame_rate_den = 1;

    const QString path = UniqueTempPath();

    QString err;
    ASSERT_TRUE(RecordingPresetStore::ExportPresetToFile(source, path, &err)) << err.toStdString();

    RecordingPresetRegistry reg;
    const std::vector<std::string> existing_ids = {reg.SelectedPreset().id};
    const QVector<RecordingPreset> imported = RecordingPresetStore::ImportPresetsFromFile(path, existing_ids, &err);
    ASSERT_EQ(imported.size(), 1) << err.toStdString();

    reg.ImportPreset(imported[0]);
    EXPECT_EQ(reg.Count(), 2u);

    const RecordingPreset* found = reg.FindById(imported[0].id);
    ASSERT_NE(found, nullptr);
    EXPECT_TRUE(NormalizedConfigEquals(found->config, source.config));

    CleanupFile(path);
}

} // namespace
} // namespace exosnap
