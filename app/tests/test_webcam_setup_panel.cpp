#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>

#include "models/WebcamSettings.h"
#include "ui/widgets/CameraPreview.h"
#include "ui/widgets/ExoToggle.h"
#include "ui/widgets/WebcamSetupPanel.h"

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "webcam_setup_panel_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

// Constructing WebcamSetupPanel enumerates capture devices (a robust static MF
// call that returns an empty list when no camera/MF is present) but never opens
// a camera -- preview capture only starts on showEvent(). These tests never
// show() the panel and never touch real webcam hardware.
class WebcamSetupPanelTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

// Regression test: WebcamSetupPanel::collectSettings() used to rebuild a fresh
// WebcamSettings and preserve overlay/chroma fields from current_settings_ but
// not opacity, so any control-less emission (e.g. the Mirror toggle) silently
// reset opacity to the struct default (1.0) even though applySettings() had
// just seeded a different value. This must fail before the collectSettings()
// fix and pass after it.
TEST_F(WebcamSetupPanelTest, MirrorToggle_PreservesSeededOpacity) {
    ui::widgets::WebcamSetupPanel panel;

    WebcamSettings seed;
    seed.opacity = 0.4f;
    panel.applySettings(seed);

    int count = 0;
    float last_opacity = -1.0f;
    QObject::connect(&panel, &ui::widgets::WebcamSetupPanel::settingsChanged, [&](const WebcamSettings& s) {
        ++count;
        last_opacity = s.opacity;
    });

    auto* mirror = panel.findChild<ui::widgets::ExoToggle*>(QStringLiteral("webcamPanelMirrorToggle"));
    ASSERT_NE(mirror, nullptr);
    mirror->setChecked(!mirror->isChecked());

    ASSERT_GE(count, 1);
    EXPECT_FLOAT_EQ(last_opacity, 0.4f);
}

// Same regression, driven via the Enable toggle instead of Mirror.
TEST_F(WebcamSetupPanelTest, EnableToggle_PreservesSeededOpacity) {
    ui::widgets::WebcamSetupPanel panel;

    WebcamSettings seed;
    seed.opacity = 0.4f;
    panel.applySettings(seed);

    int count = 0;
    float last_opacity = -1.0f;
    QObject::connect(&panel, &ui::widgets::WebcamSetupPanel::settingsChanged, [&](const WebcamSettings& s) {
        ++count;
        last_opacity = s.opacity;
    });

    auto* enable = panel.findChild<ui::widgets::ExoToggle*>(QStringLiteral("webcamPanelEnableToggle"));
    ASSERT_NE(enable, nullptr);
    enable->setChecked(!enable->isChecked());

    ASSERT_GE(count, 1);
    EXPECT_FLOAT_EQ(last_opacity, 0.4f);
}

// ---------------------------------------------------------------------------
// S4: MF-absent gate. Migrated from the removed test_webcam_page.cpp — the
// embedded Settings webcam card is the shipped surface that carries this gate.
// ---------------------------------------------------------------------------

TEST_F(WebcamSetupPanelTest, SetMfUnavailable_DisablesEnableToggle) {
    ui::widgets::WebcamSetupPanel panel;
    panel.setMfUnavailable(true);

    auto* enable = panel.findChild<ui::widgets::ExoToggle*>(QStringLiteral("webcamPanelEnableToggle"));
    ASSERT_NE(enable, nullptr);
    EXPECT_FALSE(enable->isEnabled()) << "Enable toggle must be disabled when MF is unavailable";
}

TEST_F(WebcamSetupPanelTest, SetMfUnavailable_DisablesDeviceCombo) {
    ui::widgets::WebcamSetupPanel panel;
    panel.setMfUnavailable(true);

    auto* combo = panel.findChild<QComboBox*>(QStringLiteral("webcamPanelDeviceCombo"));
    ASSERT_NE(combo, nullptr);
    EXPECT_FALSE(combo->isEnabled()) << "Device combo must be disabled when MF is unavailable";
}

TEST_F(WebcamSetupPanelTest, SetMfUnavailable_ShowsMediaFeaturePackNotice) {
    ui::widgets::WebcamSetupPanel panel;
    panel.setMfUnavailable(true);

    // The gate surfaces its notice in the preview placeholder (the card has no
    // standalone notice label).
    auto* preview = panel.findChild<ui::widgets::CameraPreview*>();
    ASSERT_NE(preview, nullptr);
    EXPECT_TRUE(preview->placeholderText().contains(QStringLiteral("Media Feature Pack"), Qt::CaseInsensitive))
        << "Notice must mention Media Feature Pack. text=" << preview->placeholderText().toStdString();
}

TEST_F(WebcamSetupPanelTest, SetMfUnavailable_SetControlsLockedIsNoOp) {
    // After setMfUnavailable the controls must stay disabled even if
    // setControlsLocked(false) is called — the MF gate wins.
    ui::widgets::WebcamSetupPanel panel;
    panel.setMfUnavailable(true);
    panel.setControlsLocked(false); // must be a no-op

    auto* combo = panel.findChild<QComboBox*>(QStringLiteral("webcamPanelDeviceCombo"));
    ASSERT_NE(combo, nullptr);
    EXPECT_FALSE(combo->isEnabled()) << "Combo must stay disabled after setMfUnavailable + unlock";
}

TEST_F(WebcamSetupPanelTest, DeviceCombo_DisabledWhenRecordingLocked) {
    ui::widgets::WebcamSetupPanel panel;
    panel.setControlsLocked(true);

    auto* combo = panel.findChild<QComboBox*>(QStringLiteral("webcamPanelDeviceCombo"));
    ASSERT_NE(combo, nullptr);
    EXPECT_FALSE(combo->isEnabled()) << "Device combo must be disabled while recording is locked";
}

TEST_F(WebcamSetupPanelTest, DeviceCombo_ReenabledWhenLockReleased) {
    ui::widgets::WebcamSetupPanel panel;
    panel.setControlsLocked(true);
    panel.setControlsLocked(false);

    auto* combo = panel.findChild<QComboBox*>(QStringLiteral("webcamPanelDeviceCombo"));
    ASSERT_NE(combo, nullptr);
    EXPECT_TRUE(combo->isEnabled()) << "Device combo must re-enable after the recording lock is released";
}

} // namespace
} // namespace exosnap
