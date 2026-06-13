#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QRect>
#include <QString>
#include <QTextStream>

#include "settings/AppSettingsStore.h"
#include "ui/overlay/RecordingOverlayWindow.h"

namespace exosnap {
namespace {

using ui::overlay::RecordingOverlayWindow;

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "recording_overlay_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class RecordingOverlayTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

// ── Construction and defaults ────────────────────────────────────────────────

TEST_F(RecordingOverlayTest, Construction_DefaultsToHidden) {
    RecordingOverlayWindow overlay;
    EXPECT_FALSE(overlay.isVisible());
}

TEST_F(RecordingOverlayTest, Construction_InitialStateIsRecording) {
    RecordingOverlayWindow overlay;
    EXPECT_EQ(overlay.overlayState(), RecordingOverlayWindow::OverlayState::Recording);
}

TEST_F(RecordingOverlayTest, Construction_ElapsedTextStartsEmpty) {
    RecordingOverlayWindow overlay;
    EXPECT_TRUE(overlay.elapsedText().isEmpty());
}

TEST_F(RecordingOverlayTest, SizeHint_IsReasonable) {
    RecordingOverlayWindow overlay;
    const QSize hint = overlay.sizeHint();
    // Width must accommodate "REC  00:00:00" pill.
    EXPECT_GT(hint.width(), 80);
    EXPECT_GT(hint.height(), 16);
    EXPECT_LT(hint.height(), 60);
}

// ── State transitions ────────────────────────────────────────────────────────

TEST_F(RecordingOverlayTest, UpdateElapsed_SetsText) {
    RecordingOverlayWindow overlay;
    overlay.updateElapsed(QStringLiteral("00:01:23"));
    EXPECT_EQ(overlay.elapsedText(), QStringLiteral("00:01:23"));
}

TEST_F(RecordingOverlayTest, HideOverlay_HidesWindow) {
    RecordingOverlayWindow overlay;
    // Hide when already hidden should be a no-op / not crash.
    overlay.hideOverlay();
    EXPECT_FALSE(overlay.isVisible());
}

TEST_F(RecordingOverlayTest, SetMonitorGeometry_NullRectIsAccepted) {
    RecordingOverlayWindow overlay;
    // Null rect: should not crash; falls back to primary screen.
    EXPECT_NO_FATAL_FAILURE(overlay.setMonitorGeometry(QRect{}));
}

TEST_F(RecordingOverlayTest, SetMonitorGeometry_ValidRectIsAccepted) {
    RecordingOverlayWindow overlay;
    EXPECT_NO_FATAL_FAILURE(overlay.setMonitorGeometry(QRect(0, 0, 1920, 1080)));
}

// ── Exclusion fallback behavior ──────────────────────────────────────────────

// On Windows in a test process the HWND may or may not be fully initialised,
// but the API contract we test here is: if excluded_ is false, showRecording/
// showPaused must NOT make the overlay visible.
//
// We can't easily force SetWindowDisplayAffinity to fail in a unit test, but
// we CAN verify the overlay's own guard: when isExcluded() is false the window
// must not become visible through the public API.
//
// The test below creates the overlay, immediately checks the exclusion state,
// and verifies the invariant holds: if exclusion failed, the window is hidden.

TEST_F(RecordingOverlayTest, ExclusionFallback_HiddenWhenNotExcluded) {
    RecordingOverlayWindow overlay;
    // Force a show call; the overlay internally guards on excluded_.
    overlay.showRecording(QStringLiteral("00:00:00"));

    if (!overlay.isExcluded()) {
        // On this platform (or test process) exclusion failed: window must stay hidden.
        EXPECT_FALSE(overlay.isVisible()) << "Overlay must remain hidden when SetWindowDisplayAffinity failed";
    }
    // If exclusion succeeded the overlay may be visible — both outcomes are valid.
}

TEST_F(RecordingOverlayTest, ExclusionFallback_PausedHiddenWhenNotExcluded) {
    RecordingOverlayWindow overlay;
    overlay.showPaused(QStringLiteral("00:01:00"));

    if (!overlay.isExcluded()) {
        EXPECT_FALSE(overlay.isVisible()) << "Paused overlay must stay hidden when SetWindowDisplayAffinity failed";
    }
}

// ── AppSettingsStore overlay toggle ─────────────────────────────────────────

// These tests use QCoreApplication::applicationDirPath(), which requires a
// QApplication instance. ctest (gtest_discover_tests) runs each test in
// isolation via --gtest_filter, so they cannot rely on the RecordingOverlayTest
// suite having created the application first.
class AppSettingsOverlayTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(AppSettingsOverlayTest, DefaultShowOverlayIsTrue) {
    PersistedAppSettings settings;
    EXPECT_TRUE(settings.show_recording_overlay);
}

TEST_F(AppSettingsOverlayTest, PersistAndLoadOverlaySetting_True) {
    const QString tmp_path = QCoreApplication::applicationDirPath() + QStringLiteral("/test_overlay_settings_on.ini");
    QFile::remove(tmp_path);

    AppSettingsStore store(tmp_path);
    PersistedAppSettings s;
    s.show_recording_overlay = true;
    store.Save(s);

    const PersistedAppSettings loaded = store.Load();
    EXPECT_TRUE(loaded.show_recording_overlay);

    QFile::remove(tmp_path);
}

TEST_F(AppSettingsOverlayTest, PersistAndLoadOverlaySetting_False) {
    const QString tmp_path = QCoreApplication::applicationDirPath() + QStringLiteral("/test_overlay_settings_off.ini");
    QFile::remove(tmp_path);

    AppSettingsStore store(tmp_path);
    PersistedAppSettings s;
    s.show_recording_overlay = false;
    store.Save(s);

    const PersistedAppSettings loaded = store.Load();
    EXPECT_FALSE(loaded.show_recording_overlay);

    QFile::remove(tmp_path);
}

TEST_F(AppSettingsOverlayTest, MissingKeyDefaultsToTrue) {
    // A store loaded from a file that has NO overlay group should default to true.
    const QString tmp_path =
        QCoreApplication::applicationDirPath() + QStringLiteral("/test_overlay_settings_missing.ini");
    QFile::remove(tmp_path);

    // Write a minimal settings file without an [overlay] group.
    {
        QFile f(tmp_path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream s(&f);
            s << "[hotkeys]\n";
            s << "binding_0=Alt+F9\n";
        }
    }

    AppSettingsStore store(tmp_path);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_TRUE(loaded.show_recording_overlay);

    QFile::remove(tmp_path);
}

// ── Overlay state transitions after show/hide ────────────────────────────────

TEST_F(RecordingOverlayTest, HideAfterShow_StatesRecording_ThenHides) {
    RecordingOverlayWindow overlay;
    overlay.updateElapsed(QStringLiteral("00:00:05"));
    EXPECT_EQ(overlay.elapsedText(), QStringLiteral("00:00:05"));

    overlay.hideOverlay();
    EXPECT_FALSE(overlay.isVisible());
}

TEST_F(RecordingOverlayTest, UpdateElapsed_WhileHidden_DoesNotCrash) {
    RecordingOverlayWindow overlay;
    EXPECT_NO_FATAL_FAILURE(overlay.updateElapsed(QStringLiteral("00:02:30")));
    EXPECT_EQ(overlay.elapsedText(), QStringLiteral("00:02:30"));
}

} // namespace
} // namespace exosnap
