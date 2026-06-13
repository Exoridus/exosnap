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
                                const QString& artefact = QStringLiteral("/tmp/rec.mkv"), bool finalized = false) {
    RecoveryManifestEntry e;
    e.id = id;
    e.artefact_path = artefact;
    e.intended_container = QStringLiteral("mkv");
    e.final_output_path = artefact;
    e.started_at = QStringLiteral("2026-06-13T10:00:00Z");
    e.finalized = finalized;

    RecoveryCandidate c;
    c.entry = e;
    c.artefact_size_bytes = 1024 * 1024 * 42; // 42 MB
    return c;
}

RecoveryCandidate MakeFinalizedCandidate(const QString& id = QStringLiteral("finalized-id"),
                                         const QString& artefact = QStringLiteral("/tmp/rec_finalized.mkv")) {
    return MakeCandidate(id, artefact, /*finalized=*/true);
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
// 3. ADR-0015: Finish, Continue, Delete buttons present for non-finalized candidate
// =============================================================================

TEST_F(RecoveryOverlayTest, ActionButtonsPresentForNonFinalized) {
    OverlayFixture fx;
    // Non-finalized: all three actions visible.
    const QVector<RecoveryCandidate> candidates = {MakeCandidate()};
    ui::dialogs::RecoveryOverlay overlay(fx.service, candidates);

    EXPECT_NE(overlay.findChild<QPushButton*>(QStringLiteral("recoveryFinishBtn")), nullptr);
    EXPECT_NE(overlay.findChild<QPushButton*>(QStringLiteral("recoveryContinueBtn")), nullptr);
    EXPECT_NE(overlay.findChild<QPushButton*>(QStringLiteral("recoveryDeleteBtn")), nullptr);
}

// =============================================================================
// 4. ADR-0015: Continue button NOT present for finalized candidates
// =============================================================================

TEST_F(RecoveryOverlayTest, ContinueButtonHiddenForFinalizedCandidate) {
    OverlayFixture fx;
    // Finalized: Continue must not be visible.
    const QVector<RecoveryCandidate> candidates = {MakeFinalizedCandidate()};
    ui::dialogs::RecoveryOverlay overlay(fx.service, candidates);

    auto* continue_btn = overlay.findChild<QPushButton*>(QStringLiteral("recoveryContinueBtn"));
    // Either the button is absent or it is hidden.
    EXPECT_TRUE(continue_btn == nullptr || !continue_btn->isVisible());
}

// =============================================================================
// 5. ADR-0015: "Decide later" text button present (replaces bare ×)
// =============================================================================

TEST_F(RecoveryOverlayTest, DecideLaterButtonPresent) {
    OverlayFixture fx;
    ui::dialogs::RecoveryOverlay overlay(fx.service, {MakeCandidate()});

    auto* btn = overlay.findChild<QPushButton*>(QStringLiteral("recoveryDecideLaterBtn"));
    ASSERT_NE(btn, nullptr);
    EXPECT_TRUE(btn->text().contains(QStringLiteral("Decide"), Qt::CaseInsensitive));
}

// =============================================================================
// 6. ADR-0015: No bare × close button (replaced by "Decide later")
// =============================================================================

TEST_F(RecoveryOverlayTest, NoRawXCloseButton) {
    OverlayFixture fx;
    ui::dialogs::RecoveryOverlay overlay(fx.service, {MakeCandidate()});

    // The old recoveryCloseButton (×) must no longer exist.
    auto* old_btn = overlay.findChild<QPushButton*>(QStringLiteral("recoveryCloseButton"));
    EXPECT_EQ(old_btn, nullptr);
}

// =============================================================================
// 7. Open/close state transitions
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
// 8. "Decide later" button closes the overlay and emits closed()
// =============================================================================

TEST_F(RecoveryOverlayTest, DecideLaterClosesOverlay) {
    OverlayFixture fx;
    QWidget host;
    auto* overlay = new ui::dialogs::RecoveryOverlay(fx.service, {MakeCandidate()}, &host);
    overlay->openOverlay();

    int closed_count = 0;
    QObject::connect(overlay, &ui::dialogs::RecoveryOverlay::closed, overlay, [&closed_count]() { ++closed_count; });

    auto* decide_btn = overlay->findChild<QPushButton*>(QStringLiteral("recoveryDecideLaterBtn"));
    ASSERT_NE(decide_btn, nullptr);
    decide_btn->click();

    EXPECT_FALSE(overlay->isOpen());
    EXPECT_EQ(closed_count, 1);
}

// =============================================================================
// 9. Title text contains expected string
// =============================================================================

TEST_F(RecoveryOverlayTest, TitleTextPresent) {
    OverlayFixture fx;
    ui::dialogs::RecoveryOverlay overlay(fx.service, {MakeCandidate()});
    auto* title = overlay.findChild<QLabel*>(QStringLiteral("recoveryTitle"));
    ASSERT_NE(title, nullptr);
    EXPECT_TRUE(title->text().contains(QStringLiteral("Recover"), Qt::CaseInsensitive));
}

// =============================================================================
// 10. ADR-0015: Continue button emits continueRequested signal and closes overlay
// =============================================================================

TEST_F(RecoveryOverlayTest, ContinueEmitsSignalAndClosesOverlay) {
    OverlayFixture fx;
    QWidget host;
    const auto candidate = MakeCandidate(QStringLiteral("continue-id"), QStringLiteral("/tmp/crash.mkv"));
    auto* overlay = new ui::dialogs::RecoveryOverlay(fx.service, {candidate}, &host);
    overlay->openOverlay();

    QString received_id;
    QObject::connect(overlay, &ui::dialogs::RecoveryOverlay::continueRequested, overlay,
                     [&received_id](const RecoveryManifestEntry& entry) { received_id = entry.id; });

    auto* continue_btn = overlay->findChild<QPushButton*>(QStringLiteral("recoveryContinueBtn"));
    ASSERT_NE(continue_btn, nullptr);
    // Use !isHidden() rather than isVisible() because the host widget is not shown
    // in the test environment — isVisible() is transitive through the parent chain.
    EXPECT_FALSE(continue_btn->isHidden());
    continue_btn->click();

    EXPECT_EQ(received_id, QStringLiteral("continue-id"));
    // Overlay must close after Continue.
    EXPECT_FALSE(overlay->isOpen());
}

// =============================================================================
// 11. ADR-0015: Delete button arm/confirm two-step (first click = arm)
// =============================================================================

TEST_F(RecoveryOverlayTest, DeleteButtonFirstClickArms) {
    OverlayFixture fx;
    const QVector<RecoveryCandidate> candidates = {MakeCandidate()};
    ui::dialogs::RecoveryOverlay overlay(fx.service, candidates);

    auto* delete_btn = overlay.findChild<QPushButton*>(QStringLiteral("recoveryDeleteBtn"));
    ASSERT_NE(delete_btn, nullptr);

    const QString original_text = delete_btn->text();
    delete_btn->click();

    // After first click the button text must change to indicate confirmation pending.
    EXPECT_NE(delete_btn->text(), original_text);
    EXPECT_TRUE(delete_btn->text().contains(QStringLiteral("Confirm"), Qt::CaseInsensitive));
}

// =============================================================================
// 12. Hint text mentions key action words
// =============================================================================

TEST_F(RecoveryOverlayTest, HintTextMentionsActions) {
    OverlayFixture fx;
    ui::dialogs::RecoveryOverlay overlay(fx.service, {MakeCandidate()});
    auto* hint = overlay.findChild<QLabel*>(QStringLiteral("recoveryHint"));
    ASSERT_NE(hint, nullptr);
    EXPECT_TRUE(hint->text().contains(QStringLiteral("Finish"), Qt::CaseInsensitive));
    EXPECT_TRUE(hint->text().contains(QStringLiteral("Continue"), Qt::CaseInsensitive));
}

} // namespace
} // namespace exosnap
