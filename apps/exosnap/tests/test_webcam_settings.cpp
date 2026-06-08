#include <gtest/gtest.h>

#include <QDir>
#include <QSettings>
#include <QTemporaryDir>

#include <limits>

#include "models/WebcamSettings.h"
#include "settings/AppSettingsStore.h"

namespace exosnap {
namespace {

// ---------------------------------------------------------------------------
// Helpers — capture-restart decision logic (mirrors RecordingCoordinator)
// ---------------------------------------------------------------------------

static bool CaptureNeedsRestart(const WebcamSettings& prev, const WebcamSettings& next) {
    return next.device_id != prev.device_id || next.width != prev.width || next.height != prev.height ||
           next.fps != prev.fps;
}

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

void ExpectOverlayInBounds(const WebcamOverlayRect& overlay) {
    EXPECT_GE(overlay.x_norm, 0.0f);
    EXPECT_GE(overlay.y_norm, 0.0f);
    EXPECT_GE(overlay.w_norm, WebcamOverlayRect::kMinSizeNorm);
    EXPECT_GE(overlay.h_norm, WebcamOverlayRect::kMinSizeNorm);
    EXPECT_LE(overlay.x_norm + overlay.w_norm, 1.0f);
    EXPECT_LE(overlay.y_norm + overlay.h_norm, 1.0f);
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
    EXPECT_FALSE(loaded.webcam.overlay_user_placed);
    EXPECT_TRUE(loaded.webcam.aspect_ratio_locked);
    EXPECT_FALSE(loaded.webcam.mirror); // mirror defaults off when the key is absent
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
    settings.webcam.overlay_user_placed = true;
    settings.webcam.aspect_ratio_locked = false;
    settings.webcam.mirror = true;
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
    EXPECT_TRUE(loaded.webcam.overlay_user_placed);
    EXPECT_FALSE(loaded.webcam.aspect_ratio_locked);
    EXPECT_TRUE(loaded.webcam.mirror); // mirror round-trips
    EXPECT_TRUE(loaded.webcam.chroma_key.enabled);
    EXPECT_EQ(loaded.webcam.chroma_key.r, 12);
    EXPECT_EQ(loaded.webcam.chroma_key.g, 34);
    EXPECT_EQ(loaded.webcam.chroma_key.b, 56);
    EXPECT_FLOAT_EQ(loaded.webcam.chroma_key.tolerance, 0.45f);
    EXPECT_FLOAT_EQ(loaded.webcam.chroma_key.softness, 0.15f);
}

// ---------------------------------------------------------------------------
// Capture-restart decision tests (model-level, no hardware required)
// ---------------------------------------------------------------------------

TEST(WebcamSettingsTest, OverlayOnlyChange_DoesNotRequireCaptureRestart) {
    WebcamSettings base;
    base.enabled = true;
    base.device_id = "cam1";
    base.width = 1280;
    base.height = 720;
    base.fps = 30;

    WebcamSettings overlay_moved = base;
    overlay_moved.overlay.x_norm = 0.5f;
    overlay_moved.overlay.y_norm = 0.3f;

    EXPECT_FALSE(CaptureNeedsRestart(base, overlay_moved));
}

TEST(WebcamSettingsTest, ChromaKeyChange_DoesNotRequireCaptureRestart) {
    WebcamSettings base;
    base.enabled = true;
    base.device_id = "cam1";
    base.width = 1920;
    base.height = 1080;
    base.fps = 30;
    base.chroma_key.enabled = false;

    WebcamSettings chroma_on = base;
    chroma_on.chroma_key.enabled = true;
    chroma_on.chroma_key.r = 0;
    chroma_on.chroma_key.g = 177;
    chroma_on.chroma_key.b = 64;

    EXPECT_FALSE(CaptureNeedsRestart(base, chroma_on));
}

TEST(WebcamSettingsTest, DeviceChange_RequiresCaptureRestart) {
    WebcamSettings base;
    base.device_id = "cam1";
    base.width = 1280;
    base.height = 720;
    base.fps = 30;

    WebcamSettings different_device = base;
    different_device.device_id = "cam2";

    EXPECT_TRUE(CaptureNeedsRestart(base, different_device));
}

TEST(WebcamSettingsTest, ResolutionChange_RequiresCaptureRestart) {
    WebcamSettings base;
    base.device_id = "cam1";
    base.width = 1280;
    base.height = 720;
    base.fps = 30;

    WebcamSettings hd = base;
    hd.width = 1920;
    hd.height = 1080;

    EXPECT_TRUE(CaptureNeedsRestart(base, hd));
}

TEST(WebcamSettingsTest, FpsChange_RequiresCaptureRestart) {
    WebcamSettings base;
    base.device_id = "cam1";
    base.width = 1280;
    base.height = 720;
    base.fps = 30;

    WebcamSettings sixty = base;
    sixty.fps = 60;

    EXPECT_TRUE(CaptureNeedsRestart(base, sixty));
}

TEST(WebcamSettingsTest, DisabledWebcam_IsNoOp_NoCaptureParams) {
    WebcamSettings disabled;
    disabled.enabled = false;
    disabled.device_id = "cam1";
    disabled.width = 1280;
    disabled.height = 720;
    disabled.fps = 30;

    // Changing only overlay on a disabled webcam still doesn't flag capture restart
    WebcamSettings also_disabled = disabled;
    also_disabled.overlay.w_norm = 0.5f;
    EXPECT_FALSE(CaptureNeedsRestart(disabled, also_disabled));
}

TEST(WebcamSettingsTest, OverlayRect_WidthFromHeight_MaintainsAspectRatio) {
    // Simulate computing overlay height from width and a 16:9 AR
    constexpr double kAr = 16.0 / 9.0;
    const double overlay_w = 0.25;
    const double overlay_h = overlay_w / kAr;

    // Round-trip: compute width back from height
    const double recovered_w = overlay_h * kAr;
    EXPECT_NEAR(recovered_w, overlay_w, 1e-9);
}

TEST(WebcamSettingsTest, OverlayRect_HeightFromWidth_MaintainsAspectRatio) {
    constexpr double kAr = 4.0 / 3.0;
    const double overlay_h = 0.20;
    const double overlay_w = overlay_h * kAr;

    const double recovered_h = overlay_w / kAr;
    EXPECT_NEAR(recovered_h, overlay_h, 1e-9);
}

TEST(WebcamSettingsTest, AspectRatioLocked_DefaultIsTrue) {
    const WebcamSettings s;
    EXPECT_TRUE(s.aspect_ratio_locked);
}

TEST(WebcamSettingsTest, SanitizeOverlayRect_ClampsInsideUnitRect) {
    WebcamOverlayRect overlay;
    overlay.x_norm = 0.92f;
    overlay.y_norm = 0.95f;
    overlay.w_norm = 0.80f;
    overlay.h_norm = 0.70f;

    const WebcamOverlayRect sanitized = SanitizeWebcamOverlayRect(overlay);
    ExpectOverlayInBounds(sanitized);
}

TEST(WebcamSettingsTest, SanitizeOverlayRect_RepairsNonFiniteValues) {
    WebcamOverlayRect overlay;
    overlay.x_norm = std::numeric_limits<float>::infinity();
    overlay.y_norm = -std::numeric_limits<float>::infinity();
    overlay.w_norm = std::numeric_limits<float>::quiet_NaN();
    overlay.h_norm = std::numeric_limits<float>::quiet_NaN();

    const WebcamOverlayRect sanitized = SanitizeWebcamOverlayRect(overlay);
    ExpectOverlayInBounds(sanitized);
    EXPECT_FLOAT_EQ(sanitized.w_norm, 0.25f);
    EXPECT_FLOAT_EQ(sanitized.h_norm, 0.25f);
}

TEST(WebcamSettingsTest, LoadInvalidPersistedOverlay_ClampsToSafeBounds) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString ini_path = TempSettingsPath(temp_dir);
    {
        QSettings raw(ini_path, QSettings::IniFormat);
        raw.beginGroup(QStringLiteral("webcam"));
        raw.setValue(QStringLiteral("overlay_x"), 1.75);
        raw.setValue(QStringLiteral("overlay_y"), -2.5);
        raw.setValue(QStringLiteral("overlay_w"), 4.0);
        raw.setValue(QStringLiteral("overlay_h"), 0.001);
        raw.endGroup();
        raw.sync();
    }

    AppSettingsStore store(ini_path);
    const PersistedAppSettings loaded = store.Load();
    ExpectOverlayInBounds(loaded.webcam.overlay);
}

TEST(WebcamSettingsTest, SaveInvalidWebcamSettings_WritesSanitizedValues) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings = MakeSettingsSnapshot();
    settings.webcam.width = 0;
    settings.webcam.height = -4;
    settings.webcam.fps = 0;
    settings.webcam.overlay.x_norm = 0.8f;
    settings.webcam.overlay.y_norm = 0.9f;
    settings.webcam.overlay.w_norm = 0.6f;
    settings.webcam.overlay.h_norm = 0.7f;
    settings.webcam.chroma_key.tolerance = 9.0f;
    settings.webcam.chroma_key.softness = -5.0f;

    store.Save(settings);
    const PersistedAppSettings loaded = store.Load();

    EXPECT_GE(loaded.webcam.width, 1);
    EXPECT_GE(loaded.webcam.height, 1);
    EXPECT_GE(loaded.webcam.fps, 1);
    EXPECT_LE(loaded.webcam.chroma_key.tolerance, 1.0f);
    EXPECT_GE(loaded.webcam.chroma_key.tolerance, 0.0f);
    EXPECT_LE(loaded.webcam.chroma_key.softness, 1.0f);
    EXPECT_GE(loaded.webcam.chroma_key.softness, 0.0f);
    ExpectOverlayInBounds(loaded.webcam.overlay);
}

} // namespace exosnap
