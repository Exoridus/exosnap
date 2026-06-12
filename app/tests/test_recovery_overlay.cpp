#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QWidget>

#include "services/RecoveryService.h"
#include "settings/RecoveryManifestStore.h"
#include "ui/dialogs/RecoveryOverlay.h"

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "recovery_overlay_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

// Minimal store + service fixture with an empty temp dir.
struct OverlayFixture {
    QTemporaryDir tmp;
    RecoveryManifestStore store;
    RecoveryService service;

    OverlayFixture() : store(tmp.path() + QStringLiteral("/manifest.json")), service(store) {
    }
};

RecoveryCandidate MakeCandidate(const QString& id = QStringLiteral("test-id"),
                                const QString& artefact = QStringLiteral("/tmp/rec.mkv")) {
    RecoveryManifestEntry e;
    e.id = id;
    e.artefact_path = artefact;
    e.intended_container = QStringLiteral("mkv");
    e.final_output_path = artefact;
    e.started_at = QStringLiteral("2026-06-13T10:00:00Z");
    e.finalized = false;

    RecoveryCandidate c;
    c.entry = e;
    c.artefact_size_bytes = 1024 * 1024 * 42; // 42 MB
    return c;
}

class RecoveryOverlayTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

// =============================================================================
// 1. Overlay is not a native QDialog
// =============================================================================

TEST_F(RecoveryOverlayTest, IsNotNativeDialog) {
    OverlayFixture fx;
    const QVector<RecoveryCandidate> candidates = {MakeCandidate()};
    ui::dialogs::RecoveryOverlay overlay(fx.service, candidates);

    EXPECT_EQ(qobject_cast<QDialog*>(&overlay), nullptr);
}

// =============================================================================
// 2. Card is present
// =============================================================================

TEST_F(RecoveryOverlayTest, CardIsPresent) {
    OverlayFixture fx;
    const QVector<RecoveryCandidate> candidates = {MakeCandidate()};
    ui::dialogs::RecoveryOverlay overlay(fx.service, candidates);

    EXPECT_NE(overlay.findChild<QFrame*>(QStringLiteral("recoveryCard")), nullptr);
}

// =============================================================================
// 3. Row buttons are present for each candidate
// =============================================================================

TEST_F(RecoveryOverlayTest, ActionButtonsPresent) {
    OverlayFixture fx;
    const QVector<RecoveryCandidate> candidates = {MakeCandidate()};
    ui::dialogs::RecoveryOverlay overlay(fx.service, candidates);

    EXPECT_NE(overlay.findChild<QPushButton*>(QStringLiteral("recoveryKeepBtn")), nullptr);
    EXPECT_NE(overlay.findChild<QPushButton*>(QStringLiteral("recoveryExportBtn")), nullptr);
    EXPECT_NE(overlay.findChild<QPushButton*>(QStringLiteral("recoveryDiscardBtn")), nullptr);
}

// =============================================================================
// 4. Close button is × symbol
// =============================================================================

TEST_F(RecoveryOverlayTest, CloseButtonIsXSymbol) {
    OverlayFixture fx;
    ui::dialogs::RecoveryOverlay overlay(fx.service, {MakeCandidate()});
    auto* btn = overlay.findChild<QPushButton*>(QStringLiteral("recoveryCloseButton"));
    ASSERT_NE(btn, nullptr);
    EXPECT_EQ(btn->text(), QString::fromLatin1("\xd7"));
}

// =============================================================================
// 5. Open/close state transitions
// =============================================================================

TEST_F(RecoveryOverlayTest, OpenCloseState) {
    OverlayFixture fx;
    QWidget host;
    auto* overlay = new ui::dialogs::RecoveryOverlay(fx.service, {MakeCandidate()}, &host);

    EXPECT_FALSE(overlay->isOpen());
    overlay->openOverlay();
    EXPECT_TRUE(overlay->isOpen());
    overlay->closeOverlay();
    EXPECT_FALSE(overlay->isOpen());
}

// =============================================================================
// 6. Close button emits closed() signal
// =============================================================================

TEST_F(RecoveryOverlayTest, CloseButtonEmitsClosed) {
    OverlayFixture fx;
    QWidget host;
    auto* overlay = new ui::dialogs::RecoveryOverlay(fx.service, {MakeCandidate()}, &host);
    overlay->openOverlay();

    int closed_count = 0;
    QObject::connect(overlay, &ui::dialogs::RecoveryOverlay::closed, overlay, [&closed_count]() { ++closed_count; });

    auto* close_btn = overlay->findChild<QPushButton*>(QStringLiteral("recoveryCloseButton"));
    ASSERT_NE(close_btn, nullptr);
    close_btn->click();

    EXPECT_FALSE(overlay->isOpen());
    EXPECT_EQ(closed_count, 1);
}

// =============================================================================
// 7. Title text contains expected string
// =============================================================================

TEST_F(RecoveryOverlayTest, TitleTextPresent) {
    OverlayFixture fx;
    ui::dialogs::RecoveryOverlay overlay(fx.service, {MakeCandidate()});
    auto* title = overlay.findChild<QLabel*>(QStringLiteral("recoveryTitle"));
    ASSERT_NE(title, nullptr);
    EXPECT_TRUE(title->text().contains(QStringLiteral("Recover"), Qt::CaseInsensitive));
}

} // namespace
} // namespace exosnap
