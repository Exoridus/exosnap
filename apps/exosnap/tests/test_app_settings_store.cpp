#include <gtest/gtest.h>

#include <QDir>
#include <QSettings>
#include <QTemporaryDir>

#include <filesystem>
#include <string>

#include "models/OutputSettingsModel.h"
#include "models/RecordingProfile.h"
#include "models/RecordingProfileRegistry.h"
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
    settings.active_profile.active_profile_id = std::string(kBuiltInProfileMkvH264AacId);
    return settings;
}

RecordingProfile FirstBuiltInProfile() {
    const auto builtins = MakeBuiltInRecordingProfiles();
    return builtins.front();
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

    // No rows persisted yet — empty by default.
    EXPECT_TRUE(loaded.audio_ui_state.source_rows.empty());
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
    settings.output.container = capability::Container::Mp4;
    settings.output.audio_codec = capability::AudioCodec::AacMf;

    store.Save(settings);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_EQ(loaded.output.audio_codec, capability::AudioCodec::AacMf);
    EXPECT_EQ(loaded.output.container, capability::Container::Mp4);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_SourceRows) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings = MakeSettingsSnapshot();
    settings.audio_ui_state.source_rows = {
        {recorder_core::AudioSourceKind::App, true, false},
        {recorder_core::AudioSourceKind::Mic, false, true},
        {recorder_core::AudioSourceKind::Sys, true, true},
    };

    store.Save(settings);
    const PersistedAppSettings loaded = store.Load();
    ASSERT_EQ(loaded.audio_ui_state.source_rows.size(), 3u);
    EXPECT_EQ(loaded.audio_ui_state.source_rows[0].kind, recorder_core::AudioSourceKind::App);
    EXPECT_TRUE(loaded.audio_ui_state.source_rows[0].enabled);
    EXPECT_FALSE(loaded.audio_ui_state.source_rows[0].merge_with_above);
    EXPECT_EQ(loaded.audio_ui_state.source_rows[1].kind, recorder_core::AudioSourceKind::Mic);
    EXPECT_FALSE(loaded.audio_ui_state.source_rows[1].enabled);
    EXPECT_TRUE(loaded.audio_ui_state.source_rows[1].merge_with_above);
    EXPECT_EQ(loaded.audio_ui_state.source_rows[2].kind, recorder_core::AudioSourceKind::Sys);
    EXPECT_TRUE(loaded.audio_ui_state.source_rows[2].enabled);
    EXPECT_TRUE(loaded.audio_ui_state.source_rows[2].merge_with_above);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_DisplaySourceRows) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings = MakeSettingsSnapshot();
    settings.audio_ui_state.source_rows = {
        {recorder_core::AudioSourceKind::SystemOutput, true, false},
        {recorder_core::AudioSourceKind::Mic, false, true},
    };

    store.Save(settings);
    const PersistedAppSettings loaded = store.Load();
    ASSERT_EQ(loaded.audio_ui_state.source_rows.size(), 2u);
    EXPECT_EQ(loaded.audio_ui_state.source_rows[0].kind, recorder_core::AudioSourceKind::SystemOutput);
    EXPECT_EQ(loaded.audio_ui_state.source_rows[1].kind, recorder_core::AudioSourceKind::Mic);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_MicGainDbValues) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));

    PersistedAppSettings settings = MakeSettingsSnapshot();
    settings.audio_ui_state.mic_gain_linear = 1.0f;
    store.Save(settings);
    EXPECT_FLOAT_EQ(store.Load().audio_ui_state.mic_gain_linear, 1.0f);

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

TEST(AppSettingsStoreTest, AppSettingsStore_OldVersion_MicGainResetsToDefault) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    // Simulate a pre-v2 file that had mic_gain_db set.
    QSettings settings(settings_path, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("audio"));
    settings.setValue(QStringLiteral("mic_gain_db"), 12);
    settings.endGroup();
    settings.sync();

    AppSettingsStore store(settings_path);
    const PersistedAppSettings loaded = store.Load();
    // settings_version absent → 0, which is < 2 → mic_gain reset to 1.0.
    EXPECT_FLOAT_EQ(loaded.audio_ui_state.mic_gain_linear, 1.0f);
    // source_rows empty — no rows persisted.
    EXPECT_TRUE(loaded.audio_ui_state.source_rows.empty());
}

TEST(AppSettingsStoreTest, AppSettingsStore_Save_WritesSettingsVersion3) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    AppSettingsStore store(settings_path);
    PersistedAppSettings settings = MakeSettingsSnapshot();
    settings.audio_ui_state.mic_gain_linear = 4.0f;
    store.Save(settings);

    QSettings raw_settings(settings_path, QSettings::IniFormat);
    EXPECT_EQ(raw_settings.value(QStringLiteral("settings_version")).toInt(), 4);
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

TEST(AppSettingsStoreTest, AppSettingsStore_InvalidSourceRowKind_Skipped) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    QSettings settings(settings_path, QSettings::IniFormat);
    settings.setValue(QStringLiteral("settings_version"), 3);
    settings.beginGroup(QStringLiteral("audio"));
    settings.setValue(QStringLiteral("source_row_count"), 3);
    settings.setValue(QStringLiteral("source_row_0_kind"), QStringLiteral("app"));
    settings.setValue(QStringLiteral("source_row_0_enabled"), true);
    settings.setValue(QStringLiteral("source_row_0_merge"), false);
    settings.setValue(QStringLiteral("source_row_1_kind"), QStringLiteral("invalid_kind"));
    settings.setValue(QStringLiteral("source_row_1_enabled"), true);
    settings.setValue(QStringLiteral("source_row_1_merge"), false);
    settings.setValue(QStringLiteral("source_row_2_kind"), QStringLiteral("sys"));
    settings.setValue(QStringLiteral("source_row_2_enabled"), true);
    settings.setValue(QStringLiteral("source_row_2_merge"), true);
    settings.endGroup();
    settings.sync();

    AppSettingsStore store(settings_path);
    const PersistedAppSettings loaded = store.Load();
    // Invalid kind skipped → only 2 rows.
    ASSERT_EQ(loaded.audio_ui_state.source_rows.size(), 2u);
    EXPECT_EQ(loaded.audio_ui_state.source_rows[0].kind, recorder_core::AudioSourceKind::App);
    EXPECT_EQ(loaded.audio_ui_state.source_rows[1].kind, recorder_core::AudioSourceKind::Sys);
}

TEST(AppSettingsStoreTest, RecordingProfiles_BuiltInsExist) {
    const auto builtins = MakeBuiltInRecordingProfiles();
    ASSERT_GE(builtins.size(), 3u);
    EXPECT_EQ(builtins[0].id, std::string(kBuiltInProfileMkvH264AacId));
    EXPECT_EQ(builtins[1].id, std::string(kBuiltInProfileWebmAv1OpusId));
    EXPECT_EQ(builtins[2].id, std::string(kBuiltInProfileMp4H264AacId));
}

TEST(AppSettingsStoreTest, AppSettingsStore_LoadMissingFile_DefaultActiveProfileIsMkvH264Aac) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    const PersistedAppSettings loaded = store.Load();

    EXPECT_EQ(loaded.active_profile.active_profile_id, std::string(kBuiltInProfileMkvH264AacId));
    EXPECT_EQ(loaded.output.container, capability::Container::Matroska);
    EXPECT_EQ(loaded.output.video_codec, capability::VideoCodec::H264Nvenc);
    EXPECT_EQ(loaded.output.audio_codec, capability::AudioCodec::AacMf);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_HotkeyBindings) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings = MakeSettingsSnapshot();
    settings.hotkey_bindings[0] = QStringLiteral("Ctrl+Alt+F10");
    settings.hotkey_bindings[1] = QStringLiteral("Ctrl+Shift+F11");
    settings.hotkey_bindings[2] = QStringLiteral("Alt+F8");
    settings.hotkey_bindings[3] = QStringLiteral("Ctrl+Alt+M");

    store.Save(settings);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_EQ(loaded.hotkey_bindings[0], QStringLiteral("Ctrl+Alt+F10"));
    EXPECT_EQ(loaded.hotkey_bindings[1], QStringLiteral("Ctrl+Shift+F11"));
    EXPECT_EQ(loaded.hotkey_bindings[2], QStringLiteral("Alt+F8"));
    EXPECT_EQ(loaded.hotkey_bindings[3], QStringLiteral("Ctrl+Alt+M"));
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_UserProfileAndActiveRestore) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings = MakeSettingsSnapshot();

    RecordingProfile user_profile = FirstBuiltInProfile();
    user_profile.source = RecordingProfileSource::User;
    user_profile.id = "user.alpha";
    user_profile.name = "Alpha";
    user_profile.output.container = capability::Container::WebM;
    user_profile.output.video_codec = capability::VideoCodec::Av1Nvenc;
    user_profile.output.audio_codec = capability::AudioCodec::Opus;
    user_profile.output.naming_pattern = L"{profile}/{datetime}";
    settings.user_profiles.push_back(user_profile);
    settings.active_profile.active_profile_id = user_profile.id;
    settings.output = user_profile.output;
    settings.video = user_profile.video;
    settings.audio_ui_state = user_profile.audio_ui_state;

    store.Save(settings);
    const PersistedAppSettings loaded = store.Load();

    EXPECT_EQ(loaded.active_profile.active_profile_id, "user.alpha");
    ASSERT_EQ(loaded.user_profiles.size(), 1u);
    EXPECT_EQ(loaded.user_profiles[0].id, "user.alpha");
    EXPECT_EQ(loaded.output.container, capability::Container::WebM);
    EXPECT_EQ(loaded.output.video_codec, capability::VideoCodec::Av1Nvenc);
    EXPECT_EQ(loaded.output.audio_codec, capability::AudioCodec::Opus);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_ModifiedBuiltInProfile) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings = MakeSettingsSnapshot();

    RecordingProfile modified = FirstBuiltInProfile();
    modified.id = std::string(kBuiltInProfileMkvH264AacId);
    modified.name = "MKV · H.264 · AAC";
    modified.output.naming_pattern = L"{profile}/{container}/{datetime}";
    settings.modified_builtin_profiles.push_back(modified);
    settings.active_profile.active_profile_id = modified.id;
    settings.active_profile.active_profile_modified = true;
    settings.output = modified.output;
    settings.video = modified.video;
    settings.audio_ui_state = modified.audio_ui_state;

    store.Save(settings);
    const PersistedAppSettings loaded = store.Load();

    EXPECT_EQ(loaded.active_profile.active_profile_id, std::string(kBuiltInProfileMkvH264AacId));
    EXPECT_TRUE(loaded.active_profile.active_profile_modified);
    ASSERT_EQ(loaded.modified_builtin_profiles.size(), 1u);
    EXPECT_EQ(loaded.modified_builtin_profiles[0].output.naming_pattern, L"{profile}/{container}/{datetime}");
    EXPECT_EQ(loaded.output.naming_pattern, L"{profile}/{container}/{datetime}");
}

TEST(ProfileRegistryTest, BuiltInEditCreatesModifiedSnapshot) {
    RecordingProfileRegistry registry;
    ActiveRecordingProfileState active;
    active.active_profile_id = std::string(kBuiltInProfileMkvH264AacId);
    registry.LoadState({}, {}, active);

    OutputSettingsModel output = FirstBuiltInProfile().output;
    output.naming_pattern = L"{profile}/{datetime}";
    registry.ApplyOutputToActive(output);

    EXPECT_TRUE(registry.IsActiveBuiltInModified());
    ASSERT_EQ(registry.ModifiedBuiltInProfiles().size(), 1u);
    EXPECT_EQ(registry.ModifiedBuiltInProfiles()[0].output.naming_pattern, L"{profile}/{datetime}");
}

TEST(ProfileRegistryTest, DuplicateCreatesUserProfile) {
    RecordingProfileRegistry registry;
    ActiveRecordingProfileState active;
    active.active_profile_id = std::string(kBuiltInProfileMkvH264AacId);
    registry.LoadState({}, {}, active);

    ASSERT_TRUE(registry.DuplicateActiveProfile());
    EXPECT_EQ(registry.UserProfiles().size(), 1u);
    EXPECT_TRUE(registry.IsActiveProfileUser());
}

TEST(ProfileRegistryTest, DeleteOnlyUserProfileSwitchesBackToDefaultBuiltIn) {
    RecordingProfileRegistry registry;
    ActiveRecordingProfileState active;
    active.active_profile_id = std::string(kBuiltInProfileMkvH264AacId);
    registry.LoadState({}, {}, active);

    registry.DuplicateActiveProfile();
    ASSERT_TRUE(registry.IsActiveProfileUser());
    ASSERT_TRUE(registry.DeleteActiveUserProfile());
    EXPECT_EQ(registry.ActiveState().active_profile_id, std::string(kBuiltInProfileMkvH264AacId));
    EXPECT_TRUE(registry.UserProfiles().empty());
}

TEST(ProfileRegistryTest, ResetActiveProfile_RemovesBuiltInModification) {
    RecordingProfileRegistry registry;
    ActiveRecordingProfileState active;
    active.active_profile_id = std::string(kBuiltInProfileMkvH264AacId);
    registry.LoadState({}, {}, active);

    OutputSettingsModel output = FirstBuiltInProfile().output;
    output.naming_pattern = L"{profile}/{datetime}";
    registry.ApplyOutputToActive(output);
    ASSERT_TRUE(registry.IsActiveBuiltInModified());

    ASSERT_TRUE(registry.ResetActiveProfile());
    EXPECT_FALSE(registry.IsActiveBuiltInModified());
    EXPECT_TRUE(registry.ModifiedBuiltInProfiles().empty());
}

} // namespace
} // namespace exosnap
