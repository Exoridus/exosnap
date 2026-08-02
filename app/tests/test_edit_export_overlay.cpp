#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QSize>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include "models/RecordingMarker.h"
#include "pages/EditExportPage.h"
#include "ui/dialogs/EditExportOverlay.h"
#include "ui/widgets/ExportPanel.h"

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

// Answers the modal confirmation from outside; QDialog::exec() spins its own
// event loop, so the click has to come from a timer that fires inside it.
void AnswerModalDialog(const QString& button_text, bool* out_appeared) {
    auto* timer = new QTimer;
    auto* attempts = new int(0);
    timer->setInterval(10);
    QObject::connect(timer, &QTimer::timeout, timer, [timer, button_text, out_appeared, attempts]() {
        auto finish = [timer, attempts]() {
            timer->stop();
            delete attempts;
            timer->deleteLater();
        };
        if (++*attempts > 200) { // ~2 s
            if (auto* stuck = QApplication::activeModalWidget())
                stuck->close();
            finish();
            return;
        }
        auto* modal = QApplication::activeModalWidget();
        if (modal == nullptr)
            return;
        for (auto* btn : modal->findChildren<QAbstractButton*>()) {
            if (QString(btn->text()).remove(QLatin1Char('&')) != button_text)
                continue;
            if (out_appeared != nullptr)
                *out_appeared = true;
            btn->click();
            finish();
            return;
        }
    });
    timer->start();
}

// Gives the hosted page a trim range, so every dismiss path has to go through
// the discard guard.
void GivePageUnsavedEdits(EditExportPage* page) {
    EditContext ctx;
    ctx.output_path = QStringLiteral("C:\\test\\recording.mkv");
    ctx.duration_seconds = 100.0;
    page->setEditContext(ctx);
    page->setTrimRangeMs(10000, 90000);
}

// Starts a real export against a master that cannot be opened. The running flag
// is set synchronously; it is only cleared from a queued completion callback, so
// as long as the test does not pump the loop the overlay stays dismiss-blocked.
void StartDoomedExport(EditExportPage* page) {
    EditContext ctx;
    ctx.output_path = QDir::temp().filePath(QStringLiteral("exosnap-edit-overlay-test.mkv"));
    ctx.mkv_master_path = QDir::temp().filePath(QStringLiteral("exosnap-edit-overlay-missing.mkv"));
    page->setEditContext(ctx);
    auto* button = page->findChild<QPushButton*>(QStringLiteral("editExportPrimaryBtn"));
    ASSERT_NE(button, nullptr);
    button->click();
}

// Lets a started export's queued completion callback land before the fixture
// tears the page down. The export runs on a real worker thread, so how long it
// needs is the machine's business — wait for the page to report it finished
// instead of guessing a sleep (a fixed 50 ms lost that race on a loaded runner).
void DrainExport(EditExportPage* page) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (page->isExportRunning() && elapsed.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    for (int i = 0; i < 8; ++i)
        QCoreApplication::processEvents();
}

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

// The overlay leaves the real title bar uncovered so the window stays movable
// and minimizable for the whole edit session (ADR 0022).
TEST_F(EditExportOverlayTest, TopInsetKeepsTheTitleBarBandFree) {
    QWidget host;
    host.resize(1280, 820);
    ui::dialogs::EditExportOverlay overlay(&host);

    overlay.setTopInset(48);
    overlay.openOverlay();

    EXPECT_EQ(overlay.geometry().top(), 48);
    EXPECT_EQ(overlay.geometry().height(), 820 - 48);
    EXPECT_EQ(overlay.geometry().width(), 1280);
}

TEST_F(EditExportOverlayTest, TopInsetSurvivesAParentResize) {
    QWidget host;
    host.resize(1280, 820);
    ui::dialogs::EditExportOverlay overlay(&host);
    overlay.setTopInset(48);
    overlay.openOverlay();

    // A hidden host defers its own resize event until it is shown, so deliver it
    // directly — the overlay's parent-watching event filter is what is under test.
    host.resize(1000, 700);
    QResizeEvent resized(QSize(1000, 700), QSize(1280, 820));
    QCoreApplication::sendEvent(&host, &resized);

    EXPECT_EQ(overlay.geometry().top(), 48);
    EXPECT_EQ(overlay.geometry().height(), 700 - 48);
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
    ASSERT_FALSE(overlay.page()->isExportRunning());

    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&overlay, &esc);

    EXPECT_TRUE(overlay.isHidden());
}

TEST_F(EditExportOverlayTest, Escape_DoesNotClose_WhileAnExportRuns) {
    QWidget host;
    host.resize(1280, 820);
    ui::dialogs::EditExportOverlay overlay(&host);
    overlay.openOverlay();
    StartDoomedExport(overlay.page());
    ASSERT_TRUE(overlay.isDismissBlocked());

    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&overlay, &esc);

    EXPECT_TRUE(overlay.isOpen()) << "Escape must not dismiss the overlay mid-export";
    DrainExport(overlay.page());
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

TEST_F(EditExportOverlayTest, BackdropClick_DoesNotClose_WhileAnExportRuns) {
    QWidget host;
    host.resize(1280, 820);
    ui::dialogs::EditExportOverlay overlay(&host);
    overlay.openOverlay();
    StartDoomedExport(overlay.page());
    ASSERT_TRUE(overlay.isDismissBlocked());

    QMouseEvent press(QEvent::MouseButtonPress, QPointF(2, 2), QPointF(2, 2), QPointF(2, 2), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&overlay, &press);

    EXPECT_TRUE(overlay.isOpen()) << "Backdrop click must not dismiss the overlay mid-export";
    DrainExport(overlay.page());
}

TEST_F(EditExportOverlayTest, IsDismissBlocked_TracksTheRunningExport) {
    QWidget host;
    host.resize(1280, 820);
    ui::dialogs::EditExportOverlay overlay(&host);
    EXPECT_FALSE(overlay.isDismissBlocked());

    StartDoomedExport(overlay.page());
    EXPECT_TRUE(overlay.isDismissBlocked());

    DrainExport(overlay.page());
    EXPECT_FALSE(overlay.isDismissBlocked());
}

// ---- Discard guard on the dismiss paths ----

TEST_F(EditExportOverlayTest, Escape_WithEdits_AsksAndKeepEditingStaysOpen) {
    QWidget host;
    host.resize(1280, 820);
    ui::dialogs::EditExportOverlay overlay(&host);
    overlay.openOverlay();
    GivePageUnsavedEdits(overlay.page());
    ASSERT_TRUE(overlay.page()->hasUnsavedEdits());

    bool asked = false;
    AnswerModalDialog(QStringLiteral("Keep editing"), &asked);
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&overlay, &esc);

    EXPECT_TRUE(asked);
    EXPECT_TRUE(overlay.isOpen());
}

TEST_F(EditExportOverlayTest, BackdropClick_WithEdits_AsksAndDiscardCloses) {
    QWidget host;
    host.resize(1280, 820);
    ui::dialogs::EditExportOverlay overlay(&host);
    overlay.openOverlay();
    GivePageUnsavedEdits(overlay.page());
    ASSERT_TRUE(overlay.page()->hasUnsavedEdits());

    bool asked = false;
    AnswerModalDialog(QStringLiteral("Discard"), &asked);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(2, 2), QPointF(2, 2), QPointF(2, 2), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&overlay, &press);

    EXPECT_TRUE(asked);
    EXPECT_TRUE(overlay.isHidden());
}

// requestCloseOverlay() is what a nav-away goes through: "Keep editing" must
// report the refusal so MainWindow can cancel the navigation outright.
TEST_F(EditExportOverlayTest, RequestClose_WithEdits_ReportsARefusedClose) {
    QWidget host;
    host.resize(1280, 820);
    ui::dialogs::EditExportOverlay overlay(&host);
    overlay.openOverlay();
    GivePageUnsavedEdits(overlay.page());

    bool asked = false;
    AnswerModalDialog(QStringLiteral("Keep editing"), &asked);
    EXPECT_FALSE(overlay.requestCloseOverlay());
    EXPECT_TRUE(asked);
    EXPECT_TRUE(overlay.isOpen());
}

// A recording start closes the overlay without a word: a modal that blocks a
// hotkey-triggered recording is worse than the lost trim (ADR 0022).
TEST_F(EditExportOverlayTest, CloseOverlay_WithEdits_NeverAsks) {
    QWidget host;
    host.resize(1280, 820);
    ui::dialogs::EditExportOverlay overlay(&host);
    overlay.openOverlay();
    GivePageUnsavedEdits(overlay.page());
    ASSERT_TRUE(overlay.page()->hasUnsavedEdits());

    overlay.closeOverlay();
    EXPECT_TRUE(overlay.isHidden());
    EXPECT_EQ(QApplication::activeModalWidget(), nullptr);
}

// Object-name stability: test_edit_export_page.cpp's findChild-based assertions
// (editExportPrimaryBtn, editReportIcon, exportPanel, ...) must keep working
// when the page is hosted inside the overlay — re-hosting must not rename or
// rebuild any of EditExportPage's internal widgets.
TEST_F(EditExportOverlayTest, HostedPage_PreservesInternalObjectNames) {
    ui::dialogs::EditExportOverlay overlay;

    EXPECT_NE(overlay.page()->findChild<ui::widgets::ExportPanel*>(QStringLiteral("exportPanel")), nullptr);
    EXPECT_NE(overlay.page()->findChild<QLabel*>(QStringLiteral("editReportIcon")), nullptr);
    EXPECT_NE(overlay.page()->findChild<QWidget*>(QStringLiteral("editDetailsRail")), nullptr);

    auto* primary = overlay.page()->findChild<QPushButton*>(QStringLiteral("editExportPrimaryBtn"));
    ASSERT_NE(primary, nullptr);
    EXPECT_EQ(primary->text(), QStringLiteral("Export"));

    auto* back_btn = overlay.page()->findChild<QPushButton*>(QStringLiteral("editExportBackBtn"));
    EXPECT_NE(back_btn, nullptr);
}

} // namespace
} // namespace exosnap
