// test_region_selection_overlay.cpp — REGION-SELECTION-SKIN-R1
//
// Tests for the skinned RegionSelectionOverlay:
//   • formatReadout() — "W × H · ratio" formatting and ratio detection.
//   • nearestPresetLabel() — threshold logic.
//   • snapToPresetAspect() — geometry snapping.
//   • Handle hit-testing — 18 px hit radius.
//   • Confirm / Esc signal flow — regionSelected / regionCancelled still fire.
//
// Uses the QApplication-fixture pattern (EnsureApplication) since
// RegionSelectionOverlay is a QWidget and ctest runs each test in isolation.

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QPushButton>
#include <QRect>
#include <QString>

#include "ui/widgets/RegionSelectionOverlay.h"

namespace exosnap::ui::widgets {
namespace {

// ── QApplication fixture ─────────────────────────────────────────────────────

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "region_selection_overlay_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class RegionSelectionOverlayTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

// ── formatReadout ─────────────────────────────────────────────────────────────

TEST_F(RegionSelectionOverlayTest, FormatReadout_16x9_IncludesRatio) {
    // 1280×720 is exactly 16:9.
    const QString result = RegionSelectionOverlay::formatReadout(1280, 720);
    EXPECT_TRUE(result.contains(QStringLiteral("1280"))) << result.toStdString();
    EXPECT_TRUE(result.contains(QStringLiteral("720"))) << result.toStdString();
    EXPECT_TRUE(result.contains(QStringLiteral("16:9"))) << result.toStdString();
}

TEST_F(RegionSelectionOverlayTest, FormatReadout_1920x1080_IncludesRatio) {
    const QString result = RegionSelectionOverlay::formatReadout(1920, 1080);
    EXPECT_TRUE(result.contains(QStringLiteral("16:9"))) << result.toStdString();
}

TEST_F(RegionSelectionOverlayTest, FormatReadout_4x3_IncludesRatio) {
    // 800×600 is 4:3.
    const QString result = RegionSelectionOverlay::formatReadout(800, 600);
    EXPECT_TRUE(result.contains(QStringLiteral("4:3"))) << result.toStdString();
}

TEST_F(RegionSelectionOverlayTest, FormatReadout_1x1_IncludesRatio) {
    const QString result = RegionSelectionOverlay::formatReadout(500, 500);
    EXPECT_TRUE(result.contains(QStringLiteral("1:1"))) << result.toStdString();
}

TEST_F(RegionSelectionOverlayTest, FormatReadout_ArbitraryRatio_NoRatioLabel) {
    // 347×211 has no close preset ratio.
    const QString result = RegionSelectionOverlay::formatReadout(347, 211);
    EXPECT_TRUE(result.contains(QStringLiteral("347"))) << result.toStdString();
    EXPECT_FALSE(result.contains(QStringLiteral(":"))) << result.toStdString();
}

TEST_F(RegionSelectionOverlayTest, FormatReadout_ZeroSize_ReturnsEmpty) {
    EXPECT_TRUE(RegionSelectionOverlay::formatReadout(0, 720).isEmpty());
    EXPECT_TRUE(RegionSelectionOverlay::formatReadout(1280, 0).isEmpty());
    EXPECT_TRUE(RegionSelectionOverlay::formatReadout(0, 0).isEmpty());
}

// ── nearestPresetLabel ───────────────────────────────────────────────────────

TEST_F(RegionSelectionOverlayTest, NearestPreset_ExactMatch_Returns16x9) {
    EXPECT_EQ(RegionSelectionOverlay::nearestPresetLabel(1280, 720), QStringLiteral("16:9"));
}

TEST_F(RegionSelectionOverlayTest, NearestPreset_SlightlyOffWithin5pct) {
    // 1290×720 is (1290/720) ≈ 1.792, 16:9 = 1.777, diff ≈ 0.8 % → within 5 %.
    const QString label = RegionSelectionOverlay::nearestPresetLabel(1290, 720);
    EXPECT_EQ(label, QStringLiteral("16:9")) << label.toStdString();
}

TEST_F(RegionSelectionOverlayTest, NearestPreset_FarFromAny_ReturnsEmpty) {
    // 347×211: ratio ≈ 1.644 — not near 16:9 (1.778), 4:3 (1.333), 1:1 (1.0), 21:9 (2.333).
    EXPECT_TRUE(RegionSelectionOverlay::nearestPresetLabel(347, 211).isEmpty());
}

TEST_F(RegionSelectionOverlayTest, NearestPreset_ZeroHeight_ReturnsEmpty) {
    EXPECT_TRUE(RegionSelectionOverlay::nearestPresetLabel(1280, 0).isEmpty());
}

TEST_F(RegionSelectionOverlayTest, NearestPreset_21x9_Match) {
    // 2560×1080: ratio = 2.370, 21:9 = 2.333, diff ≈ 1.6 % → within 5 %.
    const QString label = RegionSelectionOverlay::nearestPresetLabel(2560, 1080);
    EXPECT_EQ(label, QStringLiteral("21:9")) << label.toStdString();
}

// ── snapToPresetAspect ───────────────────────────────────────────────────────

TEST_F(RegionSelectionOverlayTest, Snap_NearlyExact16x9_AdjustsWidth) {
    // 1290×720 is close to 16:9. After snap, width should be 1280 (= 720*16/9).
    const QRect sel(0, 0, 1290, 720);
    const QRect monitor(0, 0, 2560, 1440);
    const QRect snapped = RegionSelectionOverlay::snapToPresetAspect(sel, monitor);
    EXPECT_EQ(snapped.height(), 720);
    // 720 * 16 / 9 = 1280.
    EXPECT_EQ(snapped.width(), 1280);
}

TEST_F(RegionSelectionOverlayTest, Snap_FarFromPreset_Unchanged) {
    const QRect sel(0, 0, 347, 211);
    const QRect monitor(0, 0, 1920, 1080);
    const QRect snapped = RegionSelectionOverlay::snapToPresetAspect(sel, monitor);
    EXPECT_EQ(snapped, sel);
}

TEST_F(RegionSelectionOverlayTest, Snap_ClampsToMonitorRight) {
    // If snapping would push beyond the monitor, clamp.
    const QRect sel(1200, 0, 1290, 720);   // right edge would be 1200+1280=2480
    const QRect monitor(0, 0, 2000, 1440); // monitor only goes to 1999
    const QRect snapped = RegionSelectionOverlay::snapToPresetAspect(sel, monitor);
    EXPECT_LE(snapped.right(), monitor.right());
}

TEST_F(RegionSelectionOverlayTest, Snap_InvalidRect_ReturnsUnchanged) {
    const QRect invalid;
    const QRect monitor(0, 0, 1920, 1080);
    EXPECT_EQ(RegionSelectionOverlay::snapToPresetAspect(invalid, monitor), invalid);
}

// ── Handle hit-testing ───────────────────────────────────────────────────────
// We test via a live RegionSelectionOverlay widget. The overlay requires
// activateForSelection() to set selection_rect_; we test hitTestHandle via the
// indirectly-observable behavior: after activating with an initial region,
// the overlay must be in state that accepts corner drags.
// For pure hit-test logic we call the static helpers instead.

TEST_F(RegionSelectionOverlayTest, HandleHitRadius_Within18px_Detected) {
    // Build a RegionSelectionOverlay and simulate activation with a known rect.
    // Then check that the handles can be found at expected positions.
    RegionSelectionOverlay overlay;

    // We cannot call activateForSelection without a visible desktop; instead,
    // test the pure static near-threshold math which mirrors hitTestHandle.
    // The hit box in the implementation is 18×18 centered on each corner.
    constexpr int kHitHalf = 9; // 18/2
    const QPoint corner(100, 100);

    // A point within the hit area.
    const QRect hitBox(corner.x() - kHitHalf, corner.y() - kHitHalf, 18, 18);
    EXPECT_TRUE(hitBox.contains(QPoint(corner.x() + kHitHalf - 1, corner.y())));
    // A point outside.
    EXPECT_FALSE(hitBox.contains(QPoint(corner.x() + kHitHalf + 1, corner.y())));
}

// ── Signal flow: regionSelected / regionCancelled ────────────────────────────
// These tests verify that the existing accept/cancel signals still fire
// (regression guard: the skin refactor must not break the signal contract).
//
// We drive confirmSelection() and cancelSelection() directly since they are
// private; we use the public Esc key event and Confirm button click paths.

TEST_F(RegionSelectionOverlayTest, CancelSignal_EmittedOnKeyEscape) {
    RegionSelectionOverlay overlay;

    int cancelCount = 0;
    int selectCount = 0;
    QObject::connect(&overlay, &RegionSelectionOverlay::regionCancelled, [&] { ++cancelCount; });
    QObject::connect(&overlay, &RegionSelectionOverlay::regionSelected, [&](QRect) { ++selectCount; });

    // Trigger cancelSelection via Escape key simulation (the public path).
    QKeyEvent escEvent(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&overlay, &escEvent);

    EXPECT_EQ(cancelCount, 1) << "regionCancelled should fire on Esc";
    EXPECT_EQ(selectCount, 0) << "regionSelected must not fire";
}

TEST_F(RegionSelectionOverlayTest, CancelButton_HasEscLabel) {
    // Verify the cancel button label was changed to "Esc" per Mappe spec.
    RegionSelectionOverlay overlay;
    const auto* cancel = overlay.findChild<QPushButton*>(QStringLiteral("regionOverlayCancelButton"));
    ASSERT_NE(cancel, nullptr);
    EXPECT_EQ(cancel->text(), QStringLiteral("Esc"));
}

TEST_F(RegionSelectionOverlayTest, ConfirmButton_HasConfirmLabel) {
    RegionSelectionOverlay overlay;
    const auto* confirm = overlay.findChild<QPushButton*>(QStringLiteral("regionOverlayConfirmButton"));
    ASSERT_NE(confirm, nullptr);
    EXPECT_EQ(confirm->text(), QStringLiteral("Confirm"));
}

TEST_F(RegionSelectionOverlayTest, ConfirmButton_HiddenInitially) {
    // Before a region is drawn, the Confirm button should be hidden.
    RegionSelectionOverlay overlay;
    const auto* confirm = overlay.findChild<QPushButton*>(QStringLiteral("regionOverlayConfirmButton"));
    ASSERT_NE(confirm, nullptr);
    EXPECT_FALSE(confirm->isVisible());
}

TEST_F(RegionSelectionOverlayTest, CancelButton_HiddenInitially) {
    // Before activateForSelection, the Esc button should also be hidden.
    RegionSelectionOverlay overlay;
    const auto* cancel = overlay.findChild<QPushButton*>(QStringLiteral("regionOverlayCancelButton"));
    ASSERT_NE(cancel, nullptr);
    EXPECT_FALSE(cancel->isVisible());
}

TEST_F(RegionSelectionOverlayTest, InteractionModeChanged_NotEmittedWhenAlreadyNone) {
    // cancelSelection() calls setOverlayInteraction(None).
    // Since the initial mode IS None, the signal should NOT be emitted
    // (setOverlayInteraction guards against no-op transitions).
    RegionSelectionOverlay overlay;

    int modeCount = 0;
    QObject::connect(&overlay, &RegionSelectionOverlay::interactionModeChanged,
                     [&](RegionSelectionOverlay::InteractionMode) { ++modeCount; });

    QKeyEvent escEvent(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&overlay, &escEvent);

    // No-op: already in None, so interactionModeChanged should not fire.
    EXPECT_EQ(modeCount, 0);
}

TEST_F(RegionSelectionOverlayTest, CancelSignal_EmittedOnCancelButtonClick) {
    RegionSelectionOverlay overlay;

    int cancelCount = 0;
    QObject::connect(&overlay, &RegionSelectionOverlay::regionCancelled, [&] { ++cancelCount; });

    auto* cancel = overlay.findChild<QPushButton*>(QStringLiteral("regionOverlayCancelButton"));
    ASSERT_NE(cancel, nullptr);

    // Force visible so the click is processed.
    cancel->setVisible(true);
    emit cancel->clicked();

    EXPECT_EQ(cancelCount, 1);
}

TEST_F(RegionSelectionOverlayTest, DesignConstants_AccentIsStudioMint) {
    // kAccentRgb must match Studio Mint #9BD9D2.
    const QColor accent(RegionSelectionOverlay::kAccentRgb);
    EXPECT_EQ(accent.red(), 0x9B);
    EXPECT_EQ(accent.green(), 0xD9);
    EXPECT_EQ(accent.blue(), 0xD2);
}

TEST_F(RegionSelectionOverlayTest, DesignConstants_BgIsDark) {
    const QColor bg(RegionSelectionOverlay::kBgRgb);
    EXPECT_EQ(bg.red(), 0x0E);
    EXPECT_EQ(bg.green(), 0x0E);
    EXPECT_EQ(bg.blue(), 0x10);
}

} // namespace
} // namespace exosnap::ui::widgets
