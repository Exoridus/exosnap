#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QLabel>
#include <QPushButton>
#include <QRect>
#include <QSlider>
#include <QWidget>

#include <vector>

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

    // No chip buttons remain — the Key color button's hex text reflects the
    // preset's active color instead (blue = #0000FF).
    auto* key_btn = panel.findChild<QPushButton*>(QStringLiteral("webcamPanelKeyColorBtn"));
    ASSERT_NE(key_btn, nullptr);
    EXPECT_EQ(key_btn->text(), QStringLiteral("#0000FF"))
        << "Key color button must show the Blue preset's hex value from the applied settings";

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

// ---------------------------------------------------------------------------
// Layout regression: the camera preview must not reserve a stale fixed height,
// and (Task 9) must span the full panel width, not a compact side column.
// ---------------------------------------------------------------------------

// Regression test: the embedded preview used to be pinned to a hardcoded
// setFixedHeight(175) + Qt::AlignTop, sized for the control column back when it
// only held Enable/Camera/Resolution/Mirror. The preview must have a vertical
// policy that lets it grow, not Fixed.
TEST_F(WebcamSetupPanelTest, CameraPreview_VerticalPolicyIsNotFixed) {
    ui::widgets::WebcamSetupPanel panel;

    auto* preview = panel.findChild<ui::widgets::CameraPreview*>();
    ASSERT_NE(preview, nullptr);
    EXPECT_NE(preview->sizePolicy().verticalPolicy(), QSizePolicy::Fixed)
        << "Camera preview must not be vertically Fixed.";
}

// Task 9: the preview moved from a compact 180-300px side column to a
// full-width row on top of the control stack. Laid out at a representative
// Settings width, it must span (approximately) the whole panel width -- the
// old setMaximumWidth(300) cap must be gone.
TEST_F(WebcamSetupPanelTest, CameraPreview_SpansFullWidth) {
    ui::widgets::WebcamSetupPanel panel;
    panel.resize(640, panel.sizeHint().height());
    panel.show();
    QCoreApplication::processEvents();

    auto* preview = panel.findChild<ui::widgets::CameraPreview*>();
    ASSERT_NE(preview, nullptr);
    EXPECT_GE(preview->width(), 600) << "Preview must span the full panel width, not a compact side column "
                                        "(the old 180-300px width cap must be removed).";
    panel.hide();
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

// ---------------------------------------------------------------------------
// Task 9: Key color button replaces the four preset chip buttons; rescan
// re-parents onto the preview.
// ---------------------------------------------------------------------------

TEST_F(WebcamSetupPanelTest, KeyColorButton_OpensPickerAndShowsHex) {
    ui::widgets::WebcamSetupPanel panel;

    auto* key_btn = panel.findChild<QPushButton*>(QStringLiteral("webcamPanelKeyColorBtn"));
    ASSERT_NE(key_btn, nullptr) << "Key color button must exist";

    EXPECT_FALSE(key_btn->isVisibleTo(&panel))
        << "Key color row lives inside the chroma body, which is collapsed while chroma is off";

    auto* toggle = panel.findChild<ui::widgets::ExoToggle*>(QStringLiteral("webcamPanelChromaToggle"));
    ASSERT_NE(toggle, nullptr);
    toggle->setChecked(true);

    EXPECT_TRUE(key_btn->isVisibleTo(&panel)) << "Key color button becomes visible once chroma key is enabled";
    EXPECT_TRUE(key_btn->text().contains(QStringLiteral("#")))
        << "Key color button must show the active key color as hex text. text=" << key_btn->text().toStdString();
}

TEST_F(WebcamSetupPanelTest, RescanButton_LivesOnThePreview) {
    ui::widgets::WebcamSetupPanel panel;

    auto* rescan = panel.findChild<QPushButton*>(QStringLiteral("webcamPanelRescanBtn"));
    ASSERT_NE(rescan, nullptr);
    auto* preview = panel.findChild<ui::widgets::CameraPreview*>();
    ASSERT_NE(preview, nullptr);
    EXPECT_EQ(rescan->parentWidget(), preview)
        << "Rescan button must be re-parented onto camera_preview_, not the device row";
}

// Real positioning math (not just parentage): after layout at a representative
// width, the button's geometry must sit fully inside the preview, pinned to
// its bottom-right corner with the documented 6px margin.
TEST_F(WebcamSetupPanelTest, RescanButton_PinnedBottomRightOfPreview) {
    ui::widgets::WebcamSetupPanel panel;
    panel.resize(640, panel.sizeHint().height());
    panel.show();
    QCoreApplication::processEvents();

    auto* rescan = panel.findChild<QPushButton*>(QStringLiteral("webcamPanelRescanBtn"));
    ASSERT_NE(rescan, nullptr);
    auto* preview = panel.findChild<ui::widgets::CameraPreview*>();
    ASSERT_NE(preview, nullptr);

    constexpr int kMargin = 6;
    const QRect preview_rect = preview->rect();
    const QRect btn_geom = rescan->geometry(); // in camera_preview_'s local coords

    EXPECT_TRUE(preview_rect.contains(btn_geom))
        << "Rescan button must lie entirely within the preview. preview=" << preview_rect.width() << "x"
        << preview_rect.height() << " btn=" << btn_geom.x() << "," << btn_geom.y() << " " << btn_geom.width() << "x"
        << btn_geom.height();
    EXPECT_EQ(preview_rect.right() - btn_geom.right(), kMargin)
        << "Rescan button's right edge must sit kMargin px from the preview's right edge";
    EXPECT_EQ(preview_rect.bottom() - btn_geom.bottom(), kMargin)
        << "Rescan button's bottom edge must sit kMargin px from the preview's bottom edge";

    // Resize again after the initial show: this exercises the preview's own
    // Resize event (caught via WebcamSetupPanel's event filter on
    // camera_preview_), not just the panel's resizeEvent from the first show.
    panel.resize(500, panel.sizeHint().height() + 40);
    QCoreApplication::processEvents();
    const QRect preview_rect2 = preview->rect();
    const QRect btn_geom2 = rescan->geometry();
    EXPECT_TRUE(preview_rect2.contains(btn_geom2))
        << "Rescan button must stay pinned inside the preview after a second resize";
    EXPECT_EQ(preview_rect2.right() - btn_geom2.right(), kMargin);
    EXPECT_EQ(preview_rect2.bottom() - btn_geom2.bottom(), kMargin);

    panel.hide();
}

// ---------------------------------------------------------------------------
// Webcam fps was requested but never applied: collectSettings() used to
// hardcode WebcamSettings::fps = 30 regardless of which "@ N fps" resolution
// row was actually selected, and the row's item data only carried
// width/height. Both are fixed: the row now carries its real fps, and
// selecting it must be reflected in the emitted settings.
// ---------------------------------------------------------------------------

TEST_F(WebcamSetupPanelTest, ResolutionSelection_CarriesRealFpsIntoSettings) {
    ui::widgets::WebcamSetupPanel panel;

    auto* combo = panel.findChild<QComboBox*>(QStringLiteral("webcamPanelResolutionCombo"));
    ASSERT_NE(combo, nullptr);
    combo->clear();
    combo->addItem(QStringLiteral("1920\xC3\x97"
                                  "1080 @ 30 fps"),
                   QVariantList{1920, 1080, 30});
    combo->addItem(QStringLiteral("1920\xC3\x97"
                                  "1080 @ 60 fps"),
                   QVariantList{1920, 1080, 60});
    combo->setCurrentIndex(0);

    WebcamSettings last;
    QObject::connect(&panel, &ui::widgets::WebcamSetupPanel::settingsChanged,
                     [&](const WebcamSettings& s) { last = s; });

    combo->setCurrentIndex(1); // pick the "@ 60 fps" row
    EXPECT_EQ(last.fps, 60) << "Selecting a different fps row at the same resolution must carry that fps "
                               "through -- it must not stay hardcoded at 30";
}

// FindResolutionRowIndex is the pure policy behind restoring a previously
// selected format (applySettings() / onWebcamDevicesChanged()): prefer an
// exact (width, height, fps) match; fall back to the first width/height match
// when the exact fps is no longer offered (a different camera, or a preset
// saved against a wider-format device).
TEST_F(WebcamSetupPanelTest, FindResolutionRowIndex_PrefersExactFpsMatch) {
    using Row = ui::widgets::WebcamSetupPanel::ResolutionRow;
    const std::vector<Row> rows = {
        {1920, 1080, 30},
        {1920, 1080, 60},
        {1280, 720, 30},
    };
    EXPECT_EQ(ui::widgets::WebcamSetupPanel::FindResolutionRowIndex(rows, 1920, 1080, 60), 1);
}

TEST_F(WebcamSetupPanelTest, FindResolutionRowIndex_FallsBackToWidthHeightOnly) {
    using Row = ui::widgets::WebcamSetupPanel::ResolutionRow;
    const std::vector<Row> rows = {
        {1920, 1080, 30},
        {1920, 1080, 60},
    };
    // No row offers 1920x1080 @ 24 fps (e.g. a different camera): fall back to
    // the first row at that resolution rather than matching nothing.
    EXPECT_EQ(ui::widgets::WebcamSetupPanel::FindResolutionRowIndex(rows, 1920, 1080, 24), 0);
}

TEST_F(WebcamSetupPanelTest, FindResolutionRowIndex_NoWidthHeightMatchReturnsNegativeOne) {
    using Row = ui::widgets::WebcamSetupPanel::ResolutionRow;
    const std::vector<Row> rows = {
        {1280, 720, 30},
    };
    EXPECT_EQ(ui::widgets::WebcamSetupPanel::FindResolutionRowIndex(rows, 1920, 1080, 30), -1);
}

TEST_F(WebcamSetupPanelTest, KeyColorLabel_UsesAmericanSpelling) {
    ui::widgets::WebcamSetupPanel panel;

    bool found_us_spelling = false;
    bool found_uk_spelling = false;
    for (auto* label : panel.findChildren<QLabel*>()) {
        if (label->text() == QStringLiteral("Key color"))
            found_us_spelling = true;
        if (label->text() == QStringLiteral("Key colour"))
            found_uk_spelling = true;
    }
    EXPECT_TRUE(found_us_spelling) << "Label must read 'Key color' (en-US)";
    EXPECT_FALSE(found_uk_spelling) << "Label must not read 'Key colour' (en-GB)";
}

} // namespace
} // namespace exosnap
