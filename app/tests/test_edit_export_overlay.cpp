#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QWidget>

#include "pages/EditExportPage.h"
#include "ui/dialogs/EditExportOverlay.h"

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "edit_export_overlay_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class EditExportOverlayTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(EditExportOverlayTest, RendersInWindowNotAsNativeDialog) {
    ui::dialogs::EditExportOverlay overlay;
    EXPECT_EQ(qobject_cast<QDialog*>(&overlay), nullptr);
}

TEST_F(EditExportOverlayTest, ContainsEmbeddedEditExportPage) {
    ui::dialogs::EditExportOverlay overlay;
    ASSERT_NE(overlay.page(), nullptr);
    EXPECT_EQ(overlay.page()->parent(), &overlay);
    EXPECT_EQ(overlay.page()->objectName(), QStringLiteral("editExportOverlayPanel"));
}

TEST_F(EditExportOverlayTest, IsInitiallyHidden) {
    ui::dialogs::EditExportOverlay overlay;
    EXPECT_TRUE(overlay.isHidden());
    EXPECT_FALSE(overlay.isOpen());
}

TEST_F(EditExportOverlayTest, OpenOverlay_MakesOverlayVisible) {
    QWidget host;
    host.resize(1280, 820);
    ui::dialogs::EditExportOverlay overlay(&host);

    overlay.openOverlay();
    EXPECT_TRUE(overlay.isOpen());
}

TEST_F(EditExportOverlayTest, CloseOverlay_HidesOverlayAndEmitsClosedSignal) {
    QWidget host;
    host.resize(1280, 820);
    ui::dialogs::EditExportOverlay overlay(&host);
    overlay.openOverlay();

    bool closed_fired = false;
    QObject::connect(&overlay, &ui::dialogs::EditExportOverlay::closed, [&]() { closed_fired = true; });

    overlay.closeOverlay();
    EXPECT_TRUE(overlay.isHidden());
    EXPECT_TRUE(closed_fired);
}

TEST_F(EditExportOverlayTest, CloseOverlay_IdempotentWhenAlreadyClosed) {
    ui::dialogs::EditExportOverlay overlay;

    int closed_count = 0;
    QObject::connect(&overlay, &ui::dialogs::EditExportOverlay::closed, [&]() { ++closed_count; });

    overlay.closeOverlay(); // already hidden
    overlay.closeOverlay(); // still hidden — must not fire again
    EXPECT_EQ(closed_count, 0);
}

// The opened()/closed() pair drives MainWindow's mic-privacy gate on RecordPage
// (setEditOverlayActive): opened() must fire exactly once per hidden -> open
// transition and closed() exactly once per open -> hidden transition, so the
// visibility-gated mic monitoring is suspended for exactly the overlay session.
TEST_F(EditExportOverlayTest, OpenedSignal_FiresOncePerOpenTransition) {
    QWidget host;
    host.resize(1280, 820);
    ui::dialogs::EditExportOverlay overlay(&host);

    int opened_count = 0;
    QObject::connect(&overlay, &ui::dialogs::EditExportOverlay::opened, [&]() { ++opened_count; });

    overlay.openOverlay();
    EXPECT_EQ(opened_count, 1);
    overlay.openOverlay(); // already open — no second transition
    EXPECT_EQ(opened_count, 1);

    overlay.closeOverlay();
    overlay.openOverlay(); // re-open after close — a new transition
    EXPECT_EQ(opened_count, 2);
}

TEST_F(EditExportOverlayTest, OpenedAndClosedSignals_PairUp) {
    QWidget host;
    host.resize(1280, 820);
    ui::dialogs::EditExportOverlay overlay(&host);

    int opened_count = 0;
    int closed_count = 0;
    QObject::connect(&overlay, &ui::dialogs::EditExportOverlay::opened, [&]() { ++opened_count; });
    QObject::connect(&overlay, &ui::dialogs::EditExportOverlay::closed, [&]() { ++closed_count; });

    overlay.openOverlay();
    overlay.closeOverlay();
    overlay.openOverlay();
    overlay.closeOverlay();

    EXPECT_EQ(opened_count, 2);
    EXPECT_EQ(closed_count, 2);
}

TEST_F(EditExportOverlayTest, BackRequested_ClosesOverlay) {
    QWidget host;
    host.resize(1280, 820);
    ui::dialogs::EditExportOverlay overlay(&host);
    overlay.openOverlay();

    bool closed_fired = false;
    QObject::connect(&overlay, &ui::dialogs::EditExportOverlay::closed, [&]() { closed_fired = true; });

    auto* back_btn = overlay.page()->findChild<QPushButton*>(QStringLiteral("editExportBackBtn"));
    ASSERT_NE(back_btn, nullptr);
    back_btn->click();

    EXPECT_TRUE(overlay.isHidden());
    EXPECT_TRUE(closed_fired);
}

TEST_F(EditExportOverlayTest, Escape_ClosesOverlay_WhenNotExporting) {
    QWidget host;
    host.resize(1280, 820);
    ui::dialogs::EditExportOverlay overlay(&host);
    overlay.openOverlay();
    ASSERT_EQ(overlay.page()->phase(), EditExportPage::Phase::Review);

    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&overlay, &esc);

    EXPECT_TRUE(overlay.isHidden());
}

TEST_F(EditExportOverlayTest, Escape_DoesNotClose_WhenExporting) {
    QWidget host;
    host.resize(1280, 820);
    ui::dialogs::EditExportOverlay overlay(&host);
    overlay.page()->setPhase(EditExportPage::Phase::Exporting);
    overlay.openOverlay();
    ASSERT_TRUE(overlay.isDismissBlocked());

    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&overlay, &esc);

    EXPECT_TRUE(overlay.isOpen()) << "Escape must not dismiss the overlay mid-export";
}

TEST_F(EditExportOverlayTest, BackdropClick_ClosesOverlay_WhenNotExporting) {
    QWidget host;
    host.resize(1280, 820);
    ui::dialogs::EditExportOverlay overlay(&host);
    overlay.openOverlay();

    // A point outside the hosted page's geometry (top-left corner, inside the
    // overlay's margin band) is the "backdrop".
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(2, 2), QPointF(2, 2), QPointF(2, 2), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&overlay, &press);

    EXPECT_TRUE(overlay.isHidden());
}

TEST_F(EditExportOverlayTest, BackdropClick_DoesNotClose_WhenExporting) {
    QWidget host;
    host.resize(1280, 820);
    ui::dialogs::EditExportOverlay overlay(&host);
    overlay.page()->setPhase(EditExportPage::Phase::Exporting);
    overlay.openOverlay();
    ASSERT_TRUE(overlay.isDismissBlocked());

    QMouseEvent press(QEvent::MouseButtonPress, QPointF(2, 2), QPointF(2, 2), QPointF(2, 2), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&overlay, &press);

    EXPECT_TRUE(overlay.isOpen()) << "Backdrop click must not dismiss the overlay mid-export";
}

TEST_F(EditExportOverlayTest, IsDismissBlocked_TrueOnlyDuringExportingPhase) {
    ui::dialogs::EditExportOverlay overlay;

    overlay.page()->setPhase(EditExportPage::Phase::Review);
    EXPECT_FALSE(overlay.isDismissBlocked());
    overlay.page()->setPhase(EditExportPage::Phase::Edit);
    EXPECT_FALSE(overlay.isDismissBlocked());
    overlay.page()->setPhase(EditExportPage::Phase::Output);
    EXPECT_FALSE(overlay.isDismissBlocked());
    overlay.page()->setPhase(EditExportPage::Phase::Exporting);
    EXPECT_TRUE(overlay.isDismissBlocked());
    overlay.page()->setPhase(EditExportPage::Phase::Done);
    EXPECT_FALSE(overlay.isDismissBlocked());
    overlay.page()->setPhase(EditExportPage::Phase::Failed);
    EXPECT_FALSE(overlay.isDismissBlocked());
}

// Object-name stability: test_edit_export_page.cpp's findChild-based assertions
// (editExportProgressBar, editFactDuration, editExportPrimaryBtn, ...) must keep
// working when the page is hosted inside the overlay — re-hosting must not
// rename/rebuild any of EditExportPage's internal widgets.
TEST_F(EditExportOverlayTest, HostedPage_PreservesInternalObjectNames) {
    ui::dialogs::EditExportOverlay overlay;
    overlay.page()->setPhase(EditExportPage::Phase::Exporting);

    auto* bar = overlay.page()->findChild<QProgressBar*>(QStringLiteral("editExportProgressBar"));
    EXPECT_NE(bar, nullptr);

    overlay.page()->setRecordingInfo(QStringLiteral("C:\\test\\recording.mkv"), QStringLiteral("00:04:18"),
                                     QStringLiteral("612 MB"), QStringLiteral("2560 x 1440"),
                                     QStringLiteral("60 fps CFR"), QStringLiteral("AV1"), QStringLiteral("Opus"),
                                     QStringLiteral("MKV"));
    auto* dur = overlay.page()->findChild<QLabel*>(QStringLiteral("editFactDuration"));
    ASSERT_NE(dur, nullptr);
    EXPECT_EQ(dur->text(), QStringLiteral("00:04:18"));

    overlay.page()->setPhase(EditExportPage::Phase::Output);
    auto* primary = overlay.page()->findChild<QPushButton*>(QStringLiteral("editExportPrimaryBtn"));
    ASSERT_NE(primary, nullptr);
    EXPECT_EQ(primary->text(), QStringLiteral("Export"));

    auto* back_btn = overlay.page()->findChild<QPushButton*>(QStringLiteral("editExportBackBtn"));
    EXPECT_NE(back_btn, nullptr);
}

} // namespace
} // namespace exosnap
