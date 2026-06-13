#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QRect>
#include <QString>
#include <QTextStream>

#include "settings/AppSettingsStore.h"
#include "ui/overlay/DiagnosticsOverlayWindow.h"

namespace exosnap {
namespace {

using ui::overlay::DiagnosticsOverlayWindow;

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "diagnostics_overlay_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class DiagnosticsOverlayTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

// ── Construction and defaults ────────────────────────────────────────────────

TEST_F(DiagnosticsOverlayTest, Construction_DefaultsToHidden) {
    DiagnosticsOverlayWindow overlay;
    EXPECT_FALSE(overlay.isVisible());
}

TEST_F(DiagnosticsOverlayTest, Construction_MetricFieldsStartEmpty) {
    DiagnosticsOverlayWindow overlay;
    EXPECT_TRUE(overlay.fpsBitrateText().isEmpty());
    EXPECT_TRUE(overlay.avDriftText().isEmpty());
    EXPECT_TRUE(overlay.droppedFramesText().isEmpty());
    EXPECT_TRUE(overlay.outputSizeText().isEmpty());
}

TEST_F(DiagnosticsOverlayTest, Construction_MuteGlyphsDefaultFalse) {
    DiagnosticsOverlayWindow overlay;
    EXPECT_FALSE(overlay.isMicMuted());
    EXPECT_FALSE(overlay.isSysMuted());
}

TEST_F(DiagnosticsOverlayTest, SizeHint_IsReasonable) {
    DiagnosticsOverlayWindow overlay;
    const QSize hint = overlay.sizeHint();
    // OVERLAY-SKIN-AND-PROOF-R1: single-row OverlayPill; width covers the full
    // horizontal segment sequence, height is one text-row + vertical padding.
    EXPECT_GT(hint.width(), 80);
    EXPECT_GT(hint.height(), 20);
    EXPECT_LT(hint.height(), 60); // single row: ~32px, not multi-row anymore
}

// ── State / metrics updates ──────────────────────────────────────────────────

TEST_F(DiagnosticsOverlayTest, UpdateMetrics_SetsAllFields) {
    DiagnosticsOverlayWindow overlay;
    overlay.updateMetrics(QStringLiteral("60 fps / 12 Mbps"), QStringLiteral("+3 ms"), QStringLiteral("2"),
                          QStringLiteral("1.2 GB"), true, false);

    EXPECT_EQ(overlay.fpsBitrateText(), QStringLiteral("60 fps / 12 Mbps"));
    EXPECT_EQ(overlay.avDriftText(), QStringLiteral("+3 ms"));
    EXPECT_EQ(overlay.droppedFramesText(), QStringLiteral("2"));
    EXPECT_EQ(overlay.outputSizeText(), QStringLiteral("1.2 GB"));
    EXPECT_TRUE(overlay.isMicMuted());
    EXPECT_FALSE(overlay.isSysMuted());
}

TEST_F(DiagnosticsOverlayTest, UpdateMetrics_GlyphState_BothMuted) {
    DiagnosticsOverlayWindow overlay;
    overlay.updateMetrics(QString(), QString(), QString(), QString(), true, true);
    EXPECT_TRUE(overlay.isMicMuted());
    EXPECT_TRUE(overlay.isSysMuted());
}

TEST_F(DiagnosticsOverlayTest, UpdateMetrics_GlyphState_NeitherMuted) {
    DiagnosticsOverlayWindow overlay;
    overlay.updateMetrics(QString(), QString(), QString(), QString(), false, false);
    EXPECT_FALSE(overlay.isMicMuted());
    EXPECT_FALSE(overlay.isSysMuted());
}

TEST_F(DiagnosticsOverlayTest, UpdateMetrics_WhileHidden_DoesNotCrash) {
    DiagnosticsOverlayWindow overlay;
    EXPECT_NO_FATAL_FAILURE(overlay.updateMetrics(QStringLiteral("59 fps / 10 Mbps"), QStringLiteral("-1 ms"),
                                                  QStringLiteral("0"), QStringLiteral("500 MB"), false, false));
}

TEST_F(DiagnosticsOverlayTest, UpdateMetrics_CanUpdateFieldsMultipleTimes) {
    DiagnosticsOverlayWindow overlay;
    overlay.updateMetrics(QStringLiteral("30 fps"), QString(), QString(), QString(), false, false);
    EXPECT_EQ(overlay.fpsBitrateText(), QStringLiteral("30 fps"));

    overlay.updateMetrics(QStringLiteral("60 fps"), QString(), QString(), QString(), false, false);
    EXPECT_EQ(overlay.fpsBitrateText(), QStringLiteral("60 fps"));
}

// ── Glyph row — sizeHint height ─────────────────────────────────────────────

TEST_F(DiagnosticsOverlayTest, SizeHint_GrowsWhenGlyphsPresent) {
    // OVERLAY-SKIN-AND-PROOF-R1: single-row OverlayPill redesign.
    // Muted-source glyphs are now part of the same horizontal row, so height
    // stays constant while WIDTH grows when glyphs are shown.
    DiagnosticsOverlayWindow overlay_no_glyph;
    const QSize hint_no_glyph = overlay_no_glyph.sizeHint();

    DiagnosticsOverlayWindow overlay_mic;
    overlay_mic.updateMetrics(QString(), QString(), QString(), QString(), /*mic_muted=*/true, /*sys_muted=*/false);
    const QSize hint_mic = overlay_mic.sizeHint();

    DiagnosticsOverlayWindow overlay_both;
    overlay_both.updateMetrics(QString(), QString(), QString(), QString(), /*mic_muted=*/true, /*sys_muted=*/true);
    const QSize hint_both = overlay_both.sizeHint();

    // Width grows when glyphs are added (inline in the single row).
    EXPECT_GE(hint_mic.width(), hint_no_glyph.width()) << "sizeHint width must not shrink when mic glyph is shown";
    EXPECT_GE(hint_both.width(), hint_mic.width())
        << "sizeHint width must be at least as wide with both glyphs as with one";
    // Height stays constant (single row design).
    EXPECT_EQ(hint_mic.height(), hint_no_glyph.height()) << "sizeHint height is constant (single row)";
    EXPECT_EQ(hint_both.height(), hint_mic.height()) << "sizeHint height is constant (single row)";
    // Still within a sane upper bound.
    EXPECT_LT(hint_both.height(), 80);
    EXPECT_LT(hint_both.width(), 800);
}

// ── A/V drift rendering ──────────────────────────────────────────────────────

TEST_F(DiagnosticsOverlayTest, UpdateMetrics_DriftPositive_StoresFormattedText) {
    DiagnosticsOverlayWindow overlay;
    overlay.updateMetrics(QString(), QStringLiteral("+12 ms"), QString(), QString(), false, false);
    EXPECT_EQ(overlay.avDriftText(), QStringLiteral("+12 ms"));
}

TEST_F(DiagnosticsOverlayTest, UpdateMetrics_DriftNegative_StoresFormattedText) {
    DiagnosticsOverlayWindow overlay;
    overlay.updateMetrics(QString(), QStringLiteral("-8 ms"), QString(), QString(), false, false);
    EXPECT_EQ(overlay.avDriftText(), QStringLiteral("-8 ms"));
}

TEST_F(DiagnosticsOverlayTest, UpdateMetrics_DriftDash_StoresEmDash) {
    // When no drift is available, MainWindow passes "—"; verify the overlay stores it.
    DiagnosticsOverlayWindow overlay;
    overlay.updateMetrics(QString(), QStringLiteral("—"), QString(), QString(), false, false);
    EXPECT_EQ(overlay.avDriftText(), QStringLiteral("—"));
}

TEST_F(DiagnosticsOverlayTest, UpdateMetrics_DriftNotEmpty_NotDash) {
    // After a real drift value is set, avDriftText must not be empty.
    DiagnosticsOverlayWindow overlay;
    overlay.updateMetrics(QString(), QStringLiteral("+3 ms"), QString(), QString(), false, false);
    EXPECT_FALSE(overlay.avDriftText().isEmpty());
    EXPECT_NE(overlay.avDriftText(), QStringLiteral("—"));
}

// ── Hide overlay ─────────────────────────────────────────────────────────────

TEST_F(DiagnosticsOverlayTest, HideOverlay_WhenAlreadyHidden_DoesNotCrash) {
    DiagnosticsOverlayWindow overlay;
    EXPECT_NO_FATAL_FAILURE(overlay.hideOverlay());
    EXPECT_FALSE(overlay.isVisible());
}

// ── Monitor geometry ─────────────────────────────────────────────────────────

TEST_F(DiagnosticsOverlayTest, SetMonitorGeometry_NullRectIsAccepted) {
    DiagnosticsOverlayWindow overlay;
    EXPECT_NO_FATAL_FAILURE(overlay.setMonitorGeometry(QRect{}));
}

TEST_F(DiagnosticsOverlayTest, SetMonitorGeometry_ValidRectIsAccepted) {
    DiagnosticsOverlayWindow overlay;
    EXPECT_NO_FATAL_FAILURE(overlay.setMonitorGeometry(QRect(0, 0, 1920, 1080)));
}

TEST_F(DiagnosticsOverlayTest, SetMonitorGeometry_SecondaryMonitorRectIsAccepted) {
    DiagnosticsOverlayWindow overlay;
    EXPECT_NO_FATAL_FAILURE(overlay.setMonitorGeometry(QRect(1920, 0, 2560, 1440)));
}

// ── Exclusion fallback behavior ──────────────────────────────────────────────
//
// We can't force SetWindowDisplayAffinity to fail in a unit test, but we can
// verify the overlay's guard: when isExcluded() is false, showOverlay() must
// NOT make the overlay visible.

TEST_F(DiagnosticsOverlayTest, ExclusionFallback_HiddenWhenNotExcluded) {
    DiagnosticsOverlayWindow overlay;
    overlay.showOverlay();

    if (!overlay.isExcluded()) {
        // Exclusion failed: window must stay hidden.
        EXPECT_FALSE(overlay.isVisible())
            << "DiagnosticsOverlay must remain hidden when SetWindowDisplayAffinity failed";
    }
    // If exclusion succeeded the overlay may be visible — both outcomes are valid.
}

TEST_F(DiagnosticsOverlayTest, ExclusionFallback_StaysHiddenAfterMultipleShowCalls) {
    DiagnosticsOverlayWindow overlay;
    overlay.showOverlay();
    overlay.showOverlay(); // second call — must not bypass the guard

    if (!overlay.isExcluded()) {
        EXPECT_FALSE(overlay.isVisible())
            << "DiagnosticsOverlay must remain hidden after repeated show calls when exclusion failed";
    }
}

// ── AppSettingsStore diagnostics overlay field ───────────────────────────────
//
// ctest runs each test in isolation via --gtest_filter so we own the QApplication
// here too (via EnsureApplication).

class AppSettingsDiagnosticsOverlayTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(AppSettingsDiagnosticsOverlayTest, DefaultShowDiagnosticsOverlayIsFalse) {
    PersistedAppSettings settings;
    EXPECT_FALSE(settings.show_diagnostics_overlay);
}

TEST_F(AppSettingsDiagnosticsOverlayTest, PersistAndLoad_DiagnosticsOverlay_True) {
    const QString tmp_path = QCoreApplication::applicationDirPath() + QStringLiteral("/test_diag_overlay_on.ini");
    QFile::remove(tmp_path);

    AppSettingsStore store(tmp_path);
    PersistedAppSettings s;
    s.show_diagnostics_overlay = true;
    store.Save(s);

    const PersistedAppSettings loaded = store.Load();
    EXPECT_TRUE(loaded.show_diagnostics_overlay);

    QFile::remove(tmp_path);
}

TEST_F(AppSettingsDiagnosticsOverlayTest, PersistAndLoad_DiagnosticsOverlay_False) {
    const QString tmp_path = QCoreApplication::applicationDirPath() + QStringLiteral("/test_diag_overlay_off.ini");
    QFile::remove(tmp_path);

    AppSettingsStore store(tmp_path);
    PersistedAppSettings s;
    s.show_diagnostics_overlay = false;
    store.Save(s);

    const PersistedAppSettings loaded = store.Load();
    EXPECT_FALSE(loaded.show_diagnostics_overlay);

    QFile::remove(tmp_path);
}

TEST_F(AppSettingsDiagnosticsOverlayTest, MissingKey_DefaultsToFalse) {
    // A store loaded from a file with NO overlay group should default to false.
    const QString tmp_path = QCoreApplication::applicationDirPath() + QStringLiteral("/test_diag_overlay_missing.ini");
    QFile::remove(tmp_path);

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
    EXPECT_FALSE(loaded.show_diagnostics_overlay);

    QFile::remove(tmp_path);
}

TEST_F(AppSettingsDiagnosticsOverlayTest, RecordingOverlayDefaultUnchanged) {
    // Ensure the recording overlay still defaults to true after our changes.
    PersistedAppSettings settings;
    EXPECT_TRUE(settings.show_recording_overlay);
}

} // namespace
} // namespace exosnap
