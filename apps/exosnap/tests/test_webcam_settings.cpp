#include <gtest/gtest.h>

#include <QDir>
#include <QTemporaryDir>

#include "settings/AppSettingsStore.h"

namespace exosnap {
namespace {

QString TempSettingsPath(const QTemporaryDir& temp_dir) {
    return QDir(temp_dir.path()).filePath(QStringLiteral("settings.ini"));
}

PersistedAppSettings MakeSettingsSnapshot() {
    PersistedAppSettings settings;
    settings.output = OutputSettingsModel::Defaults();
    settings.video = VideoSettingsModel::Defaults();
    settings.audio_ui_state = capability::AudioUiState{};
    settings.active_profile.active_profile_id = std::string(kBuiltInProfileMkvH264AacId);
    return settings;
}

} // namespace

TEST(WebcamSettingsTest, LoadMissingFile_UsesDefaults) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    const PersistedAppSettings loaded = store.Load();

    EXPECT_FALSE(loaded.webcam.enabled);
    EXPECT_TRUE(loaded.webcam.device_id.empty());
    EXPECT_EQ(loaded.webcam.width, 1280);
    EXPECT_EQ(loaded.webcam.height, 720);
    EXPECT_EQ(loaded.webcam.fps, 30);
    EXPECT_FLOAT_EQ(loaded.webcam.overlay.x_norm, 0.0f);
    EXPECT_FLOAT_EQ(loaded.webcam.overlay.y_norm, 0.0f);
    EXPECT_FLOAT_EQ(loaded.webcam.overlay.w_norm, 0.25f);
    EXPECT_FLOAT_EQ(loaded.webcam.overlay.h_norm, 0.25f);
    EXPECT_TRUE(loaded.webcam.aspect_ratio_locked);
    EXPECT_FALSE(loaded.webcam.chroma_key.enabled);
    EXPECT_EQ(loaded.webcam.chroma_key.r, 0);
    EXPECT_EQ(loaded.webcam.chroma_key.g, 177);
    EXPECT_EQ(loaded.webcam.chroma_key.b, 64);
    EXPECT_FLOAT_EQ(loaded.webcam.chroma_key.tolerance, 0.30f);
    EXPECT_FLOAT_EQ(loaded.webcam.chroma_key.softness, 0.05f);
}

TEST(WebcamSettingsTest, SaveAndLoad_RoundTripsWebcamSettings) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings = MakeSettingsSnapshot();
    settings.webcam.enabled = true;
    settings.webcam.device_id = "camera-01";
    settings.webcam.width = 1920;
    settings.webcam.height = 1080;
    settings.webcam.fps = 60;
    settings.webcam.overlay.x_norm = 0.2f;
    settings.webcam.overlay.y_norm = 0.1f;
    settings.webcam.overlay.w_norm = 0.35f;
    settings.webcam.overlay.h_norm = 0.4f;
    settings.webcam.aspect_ratio_locked = false;
    settings.webcam.chroma_key.enabled = true;
    settings.webcam.chroma_key.r = 12;
    settings.webcam.chroma_key.g = 34;
    settings.webcam.chroma_key.b = 56;
    settings.webcam.chroma_key.tolerance = 0.45f;
    settings.webcam.chroma_key.softness = 0.15f;

    store.Save(settings);
    const PersistedAppSettings loaded = store.Load();

    EXPECT_TRUE(loaded.webcam.enabled);
    EXPECT_EQ(loaded.webcam.device_id, "camera-01");
    EXPECT_EQ(loaded.webcam.width, 1920);
    EXPECT_EQ(loaded.webcam.height, 1080);
    EXPECT_EQ(loaded.webcam.fps, 60);
    EXPECT_FLOAT_EQ(loaded.webcam.overlay.x_norm, 0.2f);
    EXPECT_FLOAT_EQ(loaded.webcam.overlay.y_norm, 0.1f);
    EXPECT_FLOAT_EQ(loaded.webcam.overlay.w_norm, 0.35f);
    EXPECT_FLOAT_EQ(loaded.webcam.overlay.h_norm, 0.4f);
    EXPECT_FALSE(loaded.webcam.aspect_ratio_locked);
    EXPECT_TRUE(loaded.webcam.chroma_key.enabled);
    EXPECT_EQ(loaded.webcam.chroma_key.r, 12);
    EXPECT_EQ(loaded.webcam.chroma_key.g, 34);
    EXPECT_EQ(loaded.webcam.chroma_key.b, 56);
    EXPECT_FLOAT_EQ(loaded.webcam.chroma_key.tolerance, 0.45f);
    EXPECT_FLOAT_EQ(loaded.webcam.chroma_key.softness, 0.15f);
}

} // namespace exosnap
