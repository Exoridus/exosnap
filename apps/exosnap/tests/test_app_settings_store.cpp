#include <gtest/gtest.h>

#include <QDir>
#include <QSettings>
#include <QTemporaryDir>

#include <filesystem>
#include <string>

#include "models/OutputSettingsModel.h"
#include "settings/AppSettingsStore.h"

namespace exosnap {
namespace {

QString TempSettingsPath(const QTemporaryDir& temp_dir) {
    return QDir(temp_dir.path()).filePath(QStringLiteral("settings.ini"));
}

PersistedAppSettings MakeSettingsSnapshot() {
    PersistedAppSettings settings;
    settings.output = OutputSettingsModel::Defaults();
    settings.audio_ui_state = capability::AudioUiState{};
    return settings;
}

TEST(AppSettingsStoreTest, AppSettingsStore_LoadMissingFile_ReturnsDefaults) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    const PersistedAppSettings loaded = store.Load();
    const OutputSettingsModel output_defaults = OutputSettingsModel::Defaults();
    const capability::AudioUiState audio_defaults{};

    EXPECT_EQ(loaded.output.output_folder, output_defaults.output_folder);
    EXPECT_EQ(loaded.output.naming_pattern, output_defaults.naming_pattern);
    EXPECT_EQ(loaded.output.container, output_defaults.container);
    EXPECT_EQ(loaded.output.audio_codec, output_defaults.audio_codec);

    EXPECT_EQ(loaded.audio_ui_state.record_application_audio, audio_defaults.record_application_audio);
    EXPECT_EQ(loaded.audio_ui_state.record_system_audio, audio_defaults.record_system_audio);
    EXPECT_FALSE(loaded.audio_ui_state.separate_output_tracks);
    EXPECT_EQ(loaded.audio_ui_state.record_microphone, audio_defaults.record_microphone);
    EXPECT_EQ(loaded.audio_ui_state.mic_channel_mode, audio_defaults.mic_channel_mode);
    EXPECT_EQ(loaded.audio_ui_state.selected_mic_device_id, audio_defaults.selected_mic_device_id);
    EXPECT_FLOAT_EQ(loaded.audio_ui_state.mic_gain_linear, 1.0f);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_OutputFolder) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings = MakeSettingsSnapshot();
    settings.output.output_folder = std::filesystem::path(L"C:/tmp/exosnap-recordings");

    store.Save(settings);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_EQ(loaded.output.output_folder, settings.output.output_folder);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_NamingPattern) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings = MakeSettingsSnapshot();
    settings.output.naming_pattern = L"session_{date}_{time}_qa";

    store.Save(settings);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_EQ(loaded.output.naming_pattern, settings.output.naming_pattern);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_Container) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings = MakeSettingsSnapshot();
    settings.output.container = capability::Container::WebM;

    store.Save(settings);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_EQ(loaded.output.container, capability::Container::WebM);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_AudioCodec) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings = MakeSettingsSnapshot();
    // AAC roundtrip requires MP4 container (WebM+AAC is reconciled to Opus).
    settings.output.container = capability::Container::Mp4;
    settings.output.audio_codec = capability::AudioCodec::AacMf;

    store.Save(settings);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_EQ(loaded.output.audio_codec, capability::AudioCodec::AacMf);
    EXPECT_EQ(loaded.output.container, capability::Container::Mp4);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_AudioBooleans) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings = MakeSettingsSnapshot();
    settings.audio_ui_state.record_application_audio = false;
    settings.audio_ui_state.record_system_audio = true;
    settings.audio_ui_state.separate_output_tracks = false;
    settings.audio_ui_state.record_microphone = true;

    store.Save(settings);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_FALSE(loaded.audio_ui_state.record_application_audio);
    EXPECT_TRUE(loaded.audio_ui_state.record_system_audio);
    EXPECT_FALSE(loaded.audio_ui_state.separate_output_tracks);
    EXPECT_TRUE(loaded.audio_ui_state.record_microphone);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_MicGainDbValues) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));

    PersistedAppSettings settings = MakeSettingsSnapshot();
    settings.audio_ui_state.mic_gain_linear = 1.0f;
    store.Save(settings);
    EXPECT_FLOAT_EQ(store.Load().audio_ui_state.mic_gain_linear, 1.0f);

    // 0/6/12 dB round-trip: stored as integer dB, small rounding error tolerated
    settings.audio_ui_state.mic_gain_linear = 2.0f; // +6 dB
    store.Save(settings);
    EXPECT_NEAR(store.Load().audio_ui_state.mic_gain_linear, 2.0f, 0.02f);

    settings.audio_ui_state.mic_gain_linear = 4.0f; // +12 dB
    store.Save(settings);
    EXPECT_NEAR(store.Load().audio_ui_state.mic_gain_linear, 4.0f, 0.02f);
}

TEST(AppSettingsStoreTest, AppSettingsStore_MicGainDb24_RoundTrips) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    QSettings settings(settings_path, QSettings::IniFormat);
    settings.setValue(QStringLiteral("settings_version"), 2);
    settings.beginGroup(QStringLiteral("audio"));
    settings.setValue(QStringLiteral("mic_gain_db"), 24);
    settings.endGroup();
    settings.sync();

    AppSettingsStore store(settings_path);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_NEAR(loaded.audio_ui_state.mic_gain_linear, 15.85f, 0.05f);
}

TEST(AppSettingsStoreTest, AppSettingsStore_MicGainDbAbove24_ClampsToMax) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    QSettings settings(settings_path, QSettings::IniFormat);
    settings.setValue(QStringLiteral("settings_version"), 2);
    settings.beginGroup(QStringLiteral("audio"));
    settings.setValue(QStringLiteral("mic_gain_db"), 99);
    settings.endGroup();
    settings.sync();

    AppSettingsStore store(settings_path);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_NEAR(loaded.audio_ui_state.mic_gain_linear, 15.85f, 0.05f);
}

TEST(AppSettingsStoreTest, AppSettingsStore_MicGainDbBelow0_ClampsToMin) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    QSettings settings(settings_path, QSettings::IniFormat);
    settings.setValue(QStringLiteral("settings_version"), 2);
    settings.beginGroup(QStringLiteral("audio"));
    settings.setValue(QStringLiteral("mic_gain_db"), -6);
    settings.endGroup();
    settings.sync();

    AppSettingsStore store(settings_path);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_FLOAT_EQ(loaded.audio_ui_state.mic_gain_linear, 1.0f);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_MicChannelMode) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings = MakeSettingsSnapshot();
    settings.audio_ui_state.mic_channel_mode = recorder_core::MicChannelMode::LeftToStereo;

    store.Save(settings);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_EQ(loaded.audio_ui_state.mic_channel_mode, recorder_core::MicChannelMode::LeftToStereo);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_SelectedMicDeviceId) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings = MakeSettingsSnapshot();
    settings.audio_ui_state.selected_mic_device_id = std::string("mic-device-123");

    store.Save(settings);
    const PersistedAppSettings loaded = store.Load();
    ASSERT_TRUE(loaded.audio_ui_state.selected_mic_device_id.has_value());
    EXPECT_EQ(*loaded.audio_ui_state.selected_mic_device_id, "mic-device-123");
}

TEST(AppSettingsStoreTest, AppSettingsStore_EmptySelectedMicDeviceId_LoadsNullopt) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    QSettings settings(settings_path, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("audio"));
    settings.setValue(QStringLiteral("selected_mic_device_id"), QString());
    settings.endGroup();
    settings.sync();

    AppSettingsStore store(settings_path);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_FALSE(loaded.audio_ui_state.selected_mic_device_id.has_value());
}

TEST(AppSettingsStoreTest, AppSettingsStore_InvalidEnumStrings_FallBackToDefaults) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    QSettings settings(settings_path, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("output"));
    settings.setValue(QStringLiteral("container"), QStringLiteral("invalid_container"));
    settings.setValue(QStringLiteral("audio_codec"), QStringLiteral("invalid_codec"));
    settings.endGroup();
    settings.beginGroup(QStringLiteral("audio"));
    settings.setValue(QStringLiteral("mic_channel_mode"), QStringLiteral("invalid_mode"));
    settings.endGroup();
    settings.sync();

    AppSettingsStore store(settings_path);
    const PersistedAppSettings loaded = store.Load();
    const OutputSettingsModel output_defaults = OutputSettingsModel::Defaults();
    const capability::AudioUiState audio_defaults{};

    EXPECT_EQ(loaded.output.container, output_defaults.container);
    EXPECT_EQ(loaded.output.audio_codec, output_defaults.audio_codec);
    EXPECT_EQ(loaded.audio_ui_state.mic_channel_mode, audio_defaults.mic_channel_mode);
}

TEST(AppSettingsStoreTest, AppSettingsStore_MissingVersion_MigratesToMvpMergedDefaults) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    QSettings settings(settings_path, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("audio"));
    settings.setValue(QStringLiteral("separate_output_tracks"), true);
    settings.setValue(QStringLiteral("mic_gain_db"), 12);
    settings.endGroup();
    settings.sync();

    AppSettingsStore store(settings_path);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_FALSE(loaded.audio_ui_state.separate_output_tracks);
    EXPECT_FLOAT_EQ(loaded.audio_ui_state.mic_gain_linear, 1.0f);
}

TEST(AppSettingsStoreTest, AppSettingsStore_Save_WritesSettingsVersion2) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    AppSettingsStore store(settings_path);
    PersistedAppSettings settings = MakeSettingsSnapshot();
    settings.audio_ui_state.mic_gain_linear = 4.0f;
    store.Save(settings);

    QSettings raw_settings(settings_path, QSettings::IniFormat);
    EXPECT_EQ(raw_settings.value(QStringLiteral("settings_version")).toInt(), 2);
}

TEST(AppSettingsStoreTest, AppSettingsStore_Mp4ContainerRoundtrip) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings = MakeSettingsSnapshot();
    settings.output.container = capability::Container::Mp4;
    settings.output.audio_codec = capability::AudioCodec::AacMf;

    store.Save(settings);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_EQ(loaded.output.container, capability::Container::Mp4);
    EXPECT_EQ(loaded.output.audio_codec, capability::AudioCodec::AacMf);
}

TEST(AppSettingsStoreTest, AppSettingsStore_Mp4WithOpus_ReconcilesToAac) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    QSettings settings(settings_path, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("output"));
    settings.setValue(QStringLiteral("container"), QStringLiteral("mp4"));
    settings.setValue(QStringLiteral("audio_codec"), QStringLiteral("opus"));
    settings.endGroup();
    settings.sync();

    AppSettingsStore store(settings_path);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_EQ(loaded.output.container, capability::Container::Mp4);
    EXPECT_EQ(loaded.output.audio_codec, capability::AudioCodec::AacMf);
}

TEST(AppSettingsStoreTest, AppSettingsStore_WebMWithAac_ReconcilesToOpus) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    QSettings settings(settings_path, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("output"));
    settings.setValue(QStringLiteral("container"), QStringLiteral("webm"));
    settings.setValue(QStringLiteral("audio_codec"), QStringLiteral("aac"));
    settings.endGroup();
    settings.sync();

    AppSettingsStore store(settings_path);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_EQ(loaded.output.container, capability::Container::WebM);
    EXPECT_EQ(loaded.output.audio_codec, capability::AudioCodec::Opus);
}

TEST(AppSettingsStoreTest, AppSettingsStore_MkvContainer_LoadsAsMatroskaWithAac) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    QSettings settings(settings_path, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("output"));
    settings.setValue(QStringLiteral("container"), QStringLiteral("mkv"));
    settings.endGroup();
    settings.sync();

    AppSettingsStore store(settings_path);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_EQ(loaded.output.container, capability::Container::Matroska);
    EXPECT_EQ(loaded.output.audio_codec, capability::AudioCodec::AacMf);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SeparateTracksTrueDoesNotResurrectMvpState) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    QSettings settings(settings_path, QSettings::IniFormat);
    settings.setValue(QStringLiteral("settings_version"), 2);
    settings.beginGroup(QStringLiteral("audio"));
    settings.setValue(QStringLiteral("separate_output_tracks"), true);
    settings.endGroup();
    settings.sync();

    AppSettingsStore store(settings_path);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_FALSE(loaded.audio_ui_state.separate_output_tracks);
}

} // namespace
} // namespace exosnap
