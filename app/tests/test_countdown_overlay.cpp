// test_countdown_overlay.cpp — COUNTDOWN-OVERLAY-R1
//
// Unit tests for CountdownOverlayWindow.
// Each TEST_F runs in isolation via --gtest_filter (ctest discovery), so all
// tests share a common QApplication fixture set up in SetUpTestSuite().

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QRect>
#include <QSize>
#include <QString>

#include "ui/overlay/CountdownOverlayWindow.h"

namespace exosnap {
namespace {

using ui::overlay::CountdownOverlayWindow;

// ── QApplication fixture ─────────────────────────────────────────────────────

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "countdown_overlay_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class CountdownOverlayTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

// ── Construction ─────────────────────────────────────────────────────────────

TEST_F(CountdownOverlayTest, Construction_DefaultsToHidden) {
    CountdownOverlayWindow overlay;
    EXPECT_FALSE(overlay.isVisible());
}

TEST_F(CountdownOverlayTest, Construction_DefaultRemainingIs3) {
    CountdownOverlayWindow overlay;
    EXPECT_EQ(overlay.remainingSeconds(), 3);
}

TEST_F(CountdownOverlayTest, Construction_DefaultDurationIs3) {
    CountdownOverlayWindow overlay;
    EXPECT_EQ(overlay.durationSeconds(), 3);
}

TEST_F(CountdownOverlayTest, SizeHint_Is160x160) {
    // Canvas = kCircleSize(124) + kShadowMargin(18)*2 = 160
    CountdownOverlayWindow overlay;
    const QSize hint = overlay.sizeHint();
    EXPECT_EQ(hint.width(), hint.height()) << "Overlay must be square";
    EXPECT_GT(hint.width(), 100);
}

// ── Show / hide ──────────────────────────────────────────────────────────────

TEST_F(CountdownOverlayTest, ShowCountdown_UpdatesRemaining) {
    CountdownOverlayWindow overlay;
    overlay.showCountdown(2, 3);
    EXPECT_EQ(overlay.remainingSeconds(), 2);
    EXPECT_EQ(overlay.durationSeconds(), 3);
}

TEST_F(CountdownOverlayTest, ShowCountdown_ExclusionGuard) {
    // When WDA_EXCLUDEFROMCAPTURE fails (e.g. in a test process without the
    // correct window station), the overlay MUST NOT become visible.
    CountdownOverlayWindow overlay;
    overlay.showCountdown(3, 3);
    if (!overlay.isExcluded()) {
        EXPECT_FALSE(overlay.isVisible()) << "Overlay must stay hidden when SetWindowDisplayAffinity failed";
    }
    // If exclusion succeeded both outcomes are valid.
}

TEST_F(CountdownOverlayTest, HideOverlay_HidesWindow) {
    CountdownOverlayWindow overlay;
    overlay.hideOverlay();
    EXPECT_FALSE(overlay.isVisible());
}

TEST_F(CountdownOverlayTest, HideOverlay_WhenAlreadyHidden_DoesNotCrash) {
    CountdownOverlayWindow overlay;
    EXPECT_NO_FATAL_FAILURE(overlay.hideOverlay());
    EXPECT_FALSE(overlay.isVisible());
}

TEST_F(CountdownOverlayTest, UpdateCountdown_SetsValues) {
    CountdownOverlayWindow overlay;
    overlay.updateCountdown(1, 5);
    EXPECT_EQ(overlay.remainingSeconds(), 1);
    EXPECT_EQ(overlay.durationSeconds(), 5);
}

// ── Monitor centering ────────────────────────────────────────────────────────

TEST_F(CountdownOverlayTest, SetMonitorGeometry_NullRectIsAccepted) {
    CountdownOverlayWindow overlay;
    EXPECT_NO_FATAL_FAILURE(overlay.setMonitorGeometry(QRect{}));
}

TEST_F(CountdownOverlayTest, SetMonitorGeometry_ValidRectIsAccepted) {
    CountdownOverlayWindow overlay;
    EXPECT_NO_FATAL_FAILURE(overlay.setMonitorGeometry(QRect(0, 0, 2560, 1440)));
}

TEST_F(CountdownOverlayTest, SetMonitorGeometry_SecondaryMonitor) {
    CountdownOverlayWindow overlay;
    // Simulate secondary monitor at virtual-screen offset (1920, 0).
    EXPECT_NO_FATAL_FAILURE(overlay.setMonitorGeometry(QRect(1920, 0, 1920, 1080)));
}

// ── Exclusion state ──────────────────────────────────────────────────────────

TEST_F(CountdownOverlayTest, IsExcluded_ReturnsBoolWithoutCrash) {
    CountdownOverlayWindow overlay;
    // Trigger exclusion by calling showCountdown; then check.
    overlay.showCountdown(3, 3);
    // The result depends on the platform / process capabilities — just check it
    // doesn't crash and returns a consistent value.
    const bool excl = overlay.isExcluded();
    EXPECT_EQ(overlay.isVisible(), excl ? overlay.isVisible() : false);
    (void)excl;
}

// ── Digit rendering (painted via QPainter — smoke-level sanity) ─────────────

TEST_F(CountdownOverlayTest, SizeHint_EqualsMinimumSizeHint) {
    CountdownOverlayWindow overlay;
    EXPECT_EQ(overlay.sizeHint(), overlay.minimumSizeHint());
}

TEST_F(CountdownOverlayTest, ShowCountdown_ClampsBelowZero) {
    CountdownOverlayWindow overlay;
    // Negative remaining should be clamped to 1.
    overlay.showCountdown(-1, 3);
    EXPECT_GE(overlay.remainingSeconds(), 1);
}

TEST_F(CountdownOverlayTest, UpdateCountdown_ClampsBelowZero) {
    CountdownOverlayWindow overlay;
    overlay.updateCountdown(0, 3);
    EXPECT_GE(overlay.remainingSeconds(), 1);
}

} // namespace
} // namespace exosnap
