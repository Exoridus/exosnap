#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QPushButton>
#include <QSlider>
#include <QWidget>

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

// ---------------------------------------------------------------------------
// Slice J: Opacity + Chroma-key controls on the Settings webcam card.
// ---------------------------------------------------------------------------

// Opacity round-trips through the Settings path: applySettings seeds the slider,
// and moving the slider emits the new value back out.
TEST_F(WebcamSetupPanelTest, OpacitySlider_RoundTripsValue) {
    ui::widgets::WebcamSetupPanel panel;

    WebcamSettings seed;
    seed.opacity = 0.5f;
    panel.applySettings(seed);

    auto* slider = panel.findChild<QSlider*>(QStringLiteral("webcamPanelOpacitySlider"));
    ASSERT_NE(slider, nullptr);
    EXPECT_EQ(slider->value(), 50) << "applySettings must seed the opacity slider from settings.opacity";

    float last_opacity = -1.0f;
    QObject::connect(&panel, &ui::widgets::WebcamSetupPanel::settingsChanged,
                     [&](const WebcamSettings& s) { last_opacity = s.opacity; });

    slider->setValue(80);
    EXPECT_FLOAT_EQ(last_opacity, 0.8f) << "Moving the opacity slider must emit the new opacity";
}

// The chroma-key group is collapsed while disabled and expands when the header
// toggle is enabled; the toggle emits chroma_key.enabled through the settings path.
TEST_F(WebcamSetupPanelTest, ChromaToggle_ExpandsAndCollapsesGroup) {
    ui::widgets::WebcamSetupPanel panel;

    auto* body = panel.findChild<QWidget*>(QStringLiteral("webcamPanelChromaBody"));
    ASSERT_NE(body, nullptr);
    auto* toggle = panel.findChild<ui::widgets::ExoToggle*>(QStringLiteral("webcamPanelChromaToggle"));
    ASSERT_NE(toggle, nullptr);

    EXPECT_TRUE(body->isHidden()) << "Chroma group must start collapsed while disabled";

    bool last_enabled = false;
    QObject::connect(&panel, &ui::widgets::WebcamSetupPanel::settingsChanged,
                     [&](const WebcamSettings& s) { last_enabled = s.chroma_key.enabled; });

    toggle->setChecked(true);
    EXPECT_FALSE(body->isHidden()) << "Enabling chroma must expand the group";
    EXPECT_TRUE(last_enabled) << "Enabling chroma must emit chroma_key.enabled = true";

    toggle->setChecked(false);
    EXPECT_TRUE(body->isHidden()) << "Disabling chroma must collapse the group";
    EXPECT_FALSE(last_enabled);
}

// A programmatic (preset) change updates the chroma controls: toggle, group
// visibility, key-colour selection and the parameter sliders.
TEST_F(WebcamSetupPanelTest, ApplySettings_SyncsChromaControlsFromPreset) {
    ui::widgets::WebcamSetupPanel panel;

    WebcamSettings seed;
    seed.chroma_key.enabled = true;
    seed.chroma_key.color_mode = WebcamChromaKeyColorMode::Blue;
    seed.chroma_key.tolerance = 0.70f;
    seed.chroma_key.softness = 0.20f;
    seed.chroma_key.spill_reduction = 0.10f;
    panel.applySettings(seed);

    auto* toggle = panel.findChild<ui::widgets::ExoToggle*>(QStringLiteral("webcamPanelChromaToggle"));
    ASSERT_NE(toggle, nullptr);
    EXPECT_TRUE(toggle->isChecked());

    auto* body = panel.findChild<QWidget*>(QStringLiteral("webcamPanelChromaBody"));
    ASSERT_NE(body, nullptr);
    EXPECT_FALSE(body->isHidden()) << "Preset with chroma enabled must expand the group";

    auto* blue = panel.findChild<QPushButton*>(QStringLiteral("webcamPanelChromaBlueBtn"));
    ASSERT_NE(blue, nullptr);
    EXPECT_TRUE(blue->isChecked()) << "Blue key-colour must be selected from the preset";

    auto* tol = panel.findChild<QSlider*>(QStringLiteral("webcamPanelChromaToleranceSlider"));
    ASSERT_NE(tol, nullptr);
    EXPECT_EQ(tol->value(), 70);

    auto* soft = panel.findChild<QSlider*>(QStringLiteral("webcamPanelChromaSoftnessSlider"));
    ASSERT_NE(soft, nullptr);
    EXPECT_EQ(soft->value(), 20);

    auto* spill = panel.findChild<QSlider*>(QStringLiteral("webcamPanelChromaSpillSlider"));
    ASSERT_NE(spill, nullptr);
    EXPECT_EQ(spill->value(), 10);
}

// Opacity + chroma are pushed live during recording (UpdateWebcamOverlay), so the
// recording lock must leave them editable — the same class as Mirror. Only the
// restart-class controls (device/resolution/rescan) lock.
TEST_F(WebcamSetupPanelTest, RecordingLock_KeepsOpacityAndChromaEditable) {
    ui::widgets::WebcamSetupPanel panel;
    panel.setControlsLocked(true);

    auto* combo = panel.findChild<QComboBox*>(QStringLiteral("webcamPanelDeviceCombo"));
    ASSERT_NE(combo, nullptr);
    EXPECT_FALSE(combo->isEnabled()) << "Device combo is restart-class and must lock";

    auto* opacity = panel.findChild<QSlider*>(QStringLiteral("webcamPanelOpacitySlider"));
    ASSERT_NE(opacity, nullptr);
    EXPECT_TRUE(opacity->isEnabled()) << "Opacity is live-editable and must stay editable while locked";

    auto* chroma = panel.findChild<ui::widgets::ExoToggle*>(QStringLiteral("webcamPanelChromaToggle"));
    ASSERT_NE(chroma, nullptr);
    EXPECT_TRUE(chroma->isEnabled()) << "Chroma is live-editable and must stay editable while locked";
}

// Pins the widget→emit direction for the three chroma parameter sliders: moving
// each one emits the matching chroma_key.* value through the settings path.
TEST_F(WebcamSetupPanelTest, ChromaSliders_EmitParameterValues) {
    ui::widgets::WebcamSetupPanel panel;

    WebcamSettings last;
    QObject::connect(&panel, &ui::widgets::WebcamSetupPanel::settingsChanged,
                     [&](const WebcamSettings& s) { last = s; });

    auto* tol = panel.findChild<QSlider*>(QStringLiteral("webcamPanelChromaToleranceSlider"));
    ASSERT_NE(tol, nullptr);
    tol->setValue(65);
    EXPECT_FLOAT_EQ(last.chroma_key.tolerance, 0.65f);

    auto* soft = panel.findChild<QSlider*>(QStringLiteral("webcamPanelChromaSoftnessSlider"));
    ASSERT_NE(soft, nullptr);
    soft->setValue(25);
    EXPECT_FLOAT_EQ(last.chroma_key.softness, 0.25f);

    auto* spill = panel.findChild<QSlider*>(QStringLiteral("webcamPanelChromaSpillSlider"));
    ASSERT_NE(spill, nullptr);
    spill->setValue(45);
    EXPECT_FLOAT_EQ(last.chroma_key.spill_reduction, 0.45f);
}

// The MF-absent gate covers the new controls too.
TEST_F(WebcamSetupPanelTest, MfUnavailable_DisablesOpacityAndChroma) {
    ui::widgets::WebcamSetupPanel panel;
    panel.setMfUnavailable(true);

    auto* opacity = panel.findChild<QSlider*>(QStringLiteral("webcamPanelOpacitySlider"));
    ASSERT_NE(opacity, nullptr);
    EXPECT_FALSE(opacity->isEnabled()) << "Opacity slider must be disabled when MF is unavailable";

    auto* chroma = panel.findChild<ui::widgets::ExoToggle*>(QStringLiteral("webcamPanelChromaToggle"));
    ASSERT_NE(chroma, nullptr);
    EXPECT_FALSE(chroma->isEnabled()) << "Chroma toggle must be disabled when MF is unavailable";
}

} // namespace
} // namespace exosnap
