#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>

#include "models/WebcamSettings.h"
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

} // namespace
} // namespace exosnap
