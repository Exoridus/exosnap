#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QWidget>

#include "ui/dialogs/ExportOverlay.h"

namespace exosnap::ui::dialogs {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "export_overlay_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class ExportOverlayTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

// A resize on an unshown top-level widget still needs a pump of the event
// loop before its QEvent::Resize reaches the installed event filter.
void SettleLayout() {
    for (int i = 0; i < 8; ++i)
        QCoreApplication::processEvents();
}

// ---- Construction ----

TEST_F(ExportOverlayTest, StartsClosedInOptionsState) {
    QWidget host;
    ExportOverlay overlay(&host);
    EXPECT_FALSE(overlay.isCardOpen());
    EXPECT_EQ(overlay.state(), ExportOverlay::State::Options);
}

TEST_F(ExportOverlayTest, ContainerCombo_CarriesTheFormerOutputPanelChoices) {
    QWidget host;
    ExportOverlay overlay(&host);
    auto* combo = overlay.findChild<QComboBox*>(QStringLiteral("outputContainerCombo"));
    ASSERT_NE(combo, nullptr);
    ASSERT_EQ(combo->count(), 2);
    EXPECT_EQ(combo->itemData(0).toString(), QStringLiteral("mkv"));
    EXPECT_EQ(combo->itemData(1).toString(), QStringLiteral("mp4"));
}

TEST_F(ExportOverlayTest, SaveModeCombo_CarriesTheFormerOutputPanelChoices) {
    QWidget host;
    ExportOverlay overlay(&host);
    auto* combo = overlay.findChild<QComboBox*>(QStringLiteral("outputSaveModeCombo"));
    ASSERT_NE(combo, nullptr);
    ASSERT_EQ(combo->count(), 2);
    EXPECT_EQ(combo->itemData(0).toString(), QStringLiteral("new"));
    EXPECT_EQ(combo->itemData(1).toString(), QStringLiteral("overwrite"));
}

TEST_F(ExportOverlayTest, DestinationLabel_IsStaticInformationalText) {
    QWidget host;
    ExportOverlay overlay(&host);
    auto* dest = overlay.findChild<QLabel*>(QStringLiteral("editExportDestFolder"));
    ASSERT_NE(dest, nullptr);
    EXPECT_FALSE(dest->text().isEmpty());
}

// ---- containerKey() / saveModeKey() ----

TEST_F(ExportOverlayTest, ContainerKey_ReflectsSelectedComboItem) {
    QWidget host;
    ExportOverlay overlay(&host);
    EXPECT_EQ(overlay.containerKey(), QStringLiteral("mkv"));

    auto* combo = overlay.findChild<QComboBox*>(QStringLiteral("outputContainerCombo"));
    ASSERT_NE(combo, nullptr);
    combo->setCurrentIndex(1);
    EXPECT_EQ(overlay.containerKey(), QStringLiteral("mp4"));
}

TEST_F(ExportOverlayTest, SaveModeKey_ReflectsSelectedComboItem) {
    QWidget host;
    ExportOverlay overlay(&host);
    EXPECT_EQ(overlay.saveModeKey(), QStringLiteral("new"));

    auto* combo = overlay.findChild<QComboBox*>(QStringLiteral("outputSaveModeCombo"));
    ASSERT_NE(combo, nullptr);
    combo->setCurrentIndex(1);
    EXPECT_EQ(overlay.saveModeKey(), QStringLiteral("overwrite"));
}

// ---- openCard() / closeCard() ----

TEST_F(ExportOverlayTest, OpenCard_ShowsCardInOptionsStateWithZeroProgress) {
    QWidget host;
    host.resize(800, 600);
    ExportOverlay overlay(&host);

    auto* bar = overlay.findChild<QProgressBar*>(QStringLiteral("exportProgressBar"));
    ASSERT_NE(bar, nullptr);
    bar->setValue(77); // dirty it first

    overlay.openCard();

    EXPECT_TRUE(overlay.isCardOpen());
    EXPECT_EQ(overlay.state(), ExportOverlay::State::Options);
    EXPECT_EQ(bar->value(), 0);
}

TEST_F(ExportOverlayTest, CloseCard_HidesCardInOptionsState) {
    QWidget host;
    host.resize(800, 600);
    ExportOverlay overlay(&host);
    overlay.openCard();

    overlay.closeCard();
    EXPECT_FALSE(overlay.isCardOpen());
}

TEST_F(ExportOverlayTest, CloseCard_IsIgnoredWhileRunning) {
    QWidget host;
    host.resize(800, 600);
    ExportOverlay overlay(&host);
    overlay.openCard();
    overlay.showRunning();

    overlay.closeCard();
    EXPECT_TRUE(overlay.isCardOpen()) << "Cancel is the only way out of a running export";
}

// ---- State machine ----

TEST_F(ExportOverlayTest, ShowRunning_TransitionsStateAndResetsProgress) {
    QWidget host;
    host.resize(800, 600);
    ExportOverlay overlay(&host);
    overlay.openCard();

    auto* bar = overlay.findChild<QProgressBar*>(QStringLiteral("exportProgressBar"));
    ASSERT_NE(bar, nullptr);
    overlay.setProgress(40);

    overlay.showRunning();
    EXPECT_EQ(overlay.state(), ExportOverlay::State::Running);
    EXPECT_EQ(bar->value(), 0);
}

TEST_F(ExportOverlayTest, SetProgress_UpdatesProgressBarValue) {
    QWidget host;
    host.resize(800, 600);
    ExportOverlay overlay(&host);
    overlay.openCard();
    overlay.showRunning();

    auto* bar = overlay.findChild<QProgressBar*>(QStringLiteral("exportProgressBar"));
    ASSERT_NE(bar, nullptr);

    overlay.setProgress(62);
    EXPECT_EQ(bar->value(), 62);
}

TEST_F(ExportOverlayTest, SetProgress_ClampsToValidRange) {
    QWidget host;
    host.resize(800, 600);
    ExportOverlay overlay(&host);
    overlay.openCard();
    overlay.showRunning();

    auto* bar = overlay.findChild<QProgressBar*>(QStringLiteral("exportProgressBar"));
    ASSERT_NE(bar, nullptr);

    overlay.setProgress(-5);
    EXPECT_EQ(bar->value(), 0);
    overlay.setProgress(140);
    EXPECT_EQ(bar->value(), 100);
}

TEST_F(ExportOverlayTest, ShowDone_TransitionsToDoneAndShowsOutputFilename) {
    QWidget host;
    host.resize(800, 600);
    ExportOverlay overlay(&host);
    overlay.openCard();
    overlay.showRunning();

    overlay.showDone(QStringLiteral("C:\\clips\\2026-08-02_edit.mkv"));
    EXPECT_EQ(overlay.state(), ExportOverlay::State::Done);

    auto* detail = overlay.findChild<QLabel*>(QStringLiteral("exportResultDetail"));
    ASSERT_NE(detail, nullptr);
    EXPECT_EQ(detail->text(), QStringLiteral("2026-08-02_edit.mkv"));
}

TEST_F(ExportOverlayTest, ShowFailed_TransitionsToFailedAndShowsErrorMessage) {
    QWidget host;
    host.resize(800, 600);
    ExportOverlay overlay(&host);
    overlay.openCard();
    overlay.showRunning();

    overlay.showFailed(QStringLiteral("Disk full"));
    EXPECT_EQ(overlay.state(), ExportOverlay::State::Failed);

    auto* detail = overlay.findChild<QLabel*>(QStringLiteral("exportResultDetail"));
    ASSERT_NE(detail, nullptr);
    EXPECT_EQ(detail->text(), QStringLiteral("Disk full"));
}

TEST_F(ExportOverlayTest, OpenCard_AfterFailed_ResetsBackToOptions) {
    QWidget host;
    host.resize(800, 600);
    ExportOverlay overlay(&host);
    overlay.openCard();
    overlay.showRunning();
    overlay.showFailed(QStringLiteral("network error"));
    ASSERT_EQ(overlay.state(), ExportOverlay::State::Failed);

    overlay.openCard();
    EXPECT_EQ(overlay.state(), ExportOverlay::State::Options);
}

// ---- Button-driven signals ----

TEST_F(ExportOverlayTest, ExportButton_EmitsExportRequested_InOptionsState) {
    QWidget host;
    host.resize(800, 600);
    ExportOverlay overlay(&host);
    overlay.openCard();

    int count = 0;
    QObject::connect(&overlay, &ExportOverlay::exportRequested, [&]() { ++count; });
    auto* primary = overlay.findChild<QPushButton*>(QStringLiteral("exportPrimaryBtn"));
    ASSERT_NE(primary, nullptr);
    primary->click();

    EXPECT_EQ(count, 1);
}

TEST_F(ExportOverlayTest, RetryButton_EmitsRetryRequested_InFailedState) {
    QWidget host;
    host.resize(800, 600);
    ExportOverlay overlay(&host);
    overlay.openCard();
    overlay.showRunning();
    overlay.showFailed(QStringLiteral("timeout"));

    int count = 0;
    QObject::connect(&overlay, &ExportOverlay::retryRequested, [&]() { ++count; });
    auto* primary = overlay.findChild<QPushButton*>(QStringLiteral("exportPrimaryBtn"));
    ASSERT_NE(primary, nullptr);
    primary->click();

    EXPECT_EQ(count, 1);
}

// Nothing is running in Options, so Cancel dismisses the card. Routing it to
// cancelRequested() instead would reach the page's abort handler, which keeps
// the card open — leaving the button with no visible effect.
TEST_F(ExportOverlayTest, CancelButton_EmitsCloseRequested_InOptionsState) {
    QWidget host;
    host.resize(800, 600);
    ExportOverlay overlay(&host);
    overlay.openCard();

    int close_count = 0;
    int cancel_count = 0;
    QObject::connect(&overlay, &ExportOverlay::closeRequested, [&]() { ++close_count; });
    QObject::connect(&overlay, &ExportOverlay::cancelRequested, [&]() { ++cancel_count; });
    auto* cancel = overlay.findChild<QPushButton*>(QStringLiteral("exportCancelBtn"));
    ASSERT_NE(cancel, nullptr);
    EXPECT_EQ(cancel->text(), QStringLiteral("Cancel"));
    cancel->click();

    EXPECT_EQ(close_count, 1);
    EXPECT_EQ(cancel_count, 0);
}

TEST_F(ExportOverlayTest, CancelButton_EmitsCancelRequested_InRunningState) {
    QWidget host;
    host.resize(800, 600);
    ExportOverlay overlay(&host);
    overlay.openCard();
    overlay.showRunning();

    int count = 0;
    QObject::connect(&overlay, &ExportOverlay::cancelRequested, [&]() { ++count; });
    auto* cancel = overlay.findChild<QPushButton*>(QStringLiteral("exportCancelBtn"));
    ASSERT_NE(cancel, nullptr);
    EXPECT_EQ(cancel->text(), QStringLiteral("Cancel"));
    cancel->click();

    EXPECT_EQ(count, 1);
}

TEST_F(ExportOverlayTest, CancelButton_BecomesCloseAndEmitsCloseRequested_InDoneState) {
    QWidget host;
    host.resize(800, 600);
    ExportOverlay overlay(&host);
    overlay.openCard();
    overlay.showRunning();
    overlay.showDone(QStringLiteral("out.mkv"));

    int count = 0;
    QObject::connect(&overlay, &ExportOverlay::closeRequested, [&]() { ++count; });
    auto* cancel = overlay.findChild<QPushButton*>(QStringLiteral("exportCancelBtn"));
    ASSERT_NE(cancel, nullptr);
    EXPECT_EQ(cancel->text(), QStringLiteral("Close"));
    cancel->click();

    EXPECT_EQ(count, 1);
}

TEST_F(ExportOverlayTest, CancelButton_BecomesCloseAndEmitsCloseRequested_InFailedState) {
    QWidget host;
    host.resize(800, 600);
    ExportOverlay overlay(&host);
    overlay.openCard();
    overlay.showRunning();
    overlay.showFailed(QStringLiteral("bad remux"));

    int count = 0;
    QObject::connect(&overlay, &ExportOverlay::closeRequested, [&]() { ++count; });
    auto* cancel = overlay.findChild<QPushButton*>(QStringLiteral("exportCancelBtn"));
    ASSERT_NE(cancel, nullptr);
    EXPECT_EQ(cancel->text(), QStringLiteral("Close"));
    cancel->click();

    EXPECT_EQ(count, 1);
}

TEST_F(ExportOverlayTest, OpenFolderButton_EmitsOpenFolderRequested) {
    QWidget host;
    host.resize(800, 600);
    ExportOverlay overlay(&host);
    overlay.openCard();
    overlay.showRunning();
    overlay.showDone(QStringLiteral("out.mkv"));

    int count = 0;
    QObject::connect(&overlay, &ExportOverlay::openFolderRequested, [&]() { ++count; });
    auto* btn = overlay.findChild<QPushButton*>(QStringLiteral("exportOpenFolderBtn"));
    ASSERT_NE(btn, nullptr);
    EXPECT_FALSE(btn->isHidden());
    btn->click();

    EXPECT_EQ(count, 1);
}

TEST_F(ExportOverlayTest, RevealButton_EmitsRevealFileRequested) {
    QWidget host;
    host.resize(800, 600);
    ExportOverlay overlay(&host);
    overlay.openCard();
    overlay.showRunning();
    overlay.showDone(QStringLiteral("out.mkv"));

    int count = 0;
    QObject::connect(&overlay, &ExportOverlay::revealFileRequested, [&]() { ++count; });
    auto* btn = overlay.findChild<QPushButton*>(QStringLiteral("exportRevealBtn"));
    ASSERT_NE(btn, nullptr);
    EXPECT_FALSE(btn->isHidden());
    btn->click();

    EXPECT_EQ(count, 1);
}

TEST_F(ExportOverlayTest, OpenFolderAndRevealButtons_HiddenOutsideDoneState) {
    QWidget host;
    host.resize(800, 600);
    ExportOverlay overlay(&host);
    overlay.openCard();

    auto* open_btn = overlay.findChild<QPushButton*>(QStringLiteral("exportOpenFolderBtn"));
    auto* reveal_btn = overlay.findChild<QPushButton*>(QStringLiteral("exportRevealBtn"));
    ASSERT_NE(open_btn, nullptr);
    ASSERT_NE(reveal_btn, nullptr);
    EXPECT_TRUE(open_btn->isHidden());
    EXPECT_TRUE(reveal_btn->isHidden());
}

// ---- Card does not drive its own open/close on signals ----

TEST_F(ExportOverlayTest, ButtonClicks_DoNotChangeCardOpenStateThemselves) {
    // Presentation only: the card leads no transitions itself. The page decides
    // what openCard()/closeCard() to call in response to every signal.
    QWidget host;
    host.resize(800, 600);
    ExportOverlay overlay(&host);
    overlay.openCard();

    auto* cancel = overlay.findChild<QPushButton*>(QStringLiteral("exportCancelBtn"));
    ASSERT_NE(cancel, nullptr);
    cancel->click();

    EXPECT_TRUE(overlay.isCardOpen());
    EXPECT_EQ(overlay.state(), ExportOverlay::State::Options);
}

// ---- Escape / backdrop dismissal ----

TEST_F(ExportOverlayTest, Escape_EmitsCloseRequested_WhenNotRunning) {
    QWidget host;
    host.resize(800, 600);
    ExportOverlay overlay(&host);
    overlay.openCard();

    int count = 0;
    QObject::connect(&overlay, &ExportOverlay::closeRequested, [&]() { ++count; });
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&overlay, &esc);

    EXPECT_EQ(count, 1);
}

TEST_F(ExportOverlayTest, Escape_BlockedWhileRunning) {
    QWidget host;
    host.resize(800, 600);
    ExportOverlay overlay(&host);
    overlay.openCard();
    overlay.showRunning();

    int count = 0;
    QObject::connect(&overlay, &ExportOverlay::closeRequested, [&]() { ++count; });
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&overlay, &esc);

    EXPECT_EQ(count, 0);
}

TEST_F(ExportOverlayTest, BackdropClick_EmitsCloseRequested_WhenNotRunning) {
    QWidget host;
    host.resize(800, 600);
    ExportOverlay overlay(&host);
    overlay.openCard();

    int count = 0;
    QObject::connect(&overlay, &ExportOverlay::closeRequested, [&]() { ++count; });
    // A point in the overlay's outer margin band, well outside the centred card.
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(2, 2), QPointF(2, 2), QPointF(2, 2), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&overlay, &press);

    EXPECT_EQ(count, 1);
}

TEST_F(ExportOverlayTest, BackdropClick_BlockedWhileRunning) {
    QWidget host;
    host.resize(800, 600);
    ExportOverlay overlay(&host);
    overlay.openCard();
    overlay.showRunning();

    int count = 0;
    QObject::connect(&overlay, &ExportOverlay::closeRequested, [&]() { ++count; });
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(2, 2), QPointF(2, 2), QPointF(2, 2), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&overlay, &press);

    EXPECT_EQ(count, 0);
}

TEST_F(ExportOverlayTest, ClickInsideCard_DoesNotEmitCloseRequested) {
    QWidget host;
    host.resize(800, 600);
    ExportOverlay overlay(&host);
    overlay.openCard();

    auto* card = overlay.findChild<QFrame*>(QStringLiteral("exportOverlayCard"));
    ASSERT_NE(card, nullptr);
    const QPointF inside(card->geometry().center());

    int count = 0;
    QObject::connect(&overlay, &ExportOverlay::closeRequested, [&]() { ++count; });
    QMouseEvent press(QEvent::MouseButtonPress, inside, inside, inside, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&overlay, &press);

    EXPECT_EQ(count, 0);
}

// ---- Geometry sync ----

TEST_F(ExportOverlayTest, OpenCard_SyncsGeometryToParentRect) {
    QWidget host;
    host.resize(900, 700);
    ExportOverlay overlay(&host);

    overlay.openCard();
    EXPECT_EQ(overlay.geometry(), host.rect());
}

TEST_F(ExportOverlayTest, ParentResize_ResyncsOverlayGeometry) {
    QWidget host;
    host.resize(900, 700);
    ExportOverlay overlay(&host);
    overlay.openCard();
    // A hidden top-level widget only queues its resize event for delivery once
    // shown — show() is what makes the resync path (parent event filter, not
    // openCard()'s own direct sync call) actually observable here.
    host.show();
    SettleLayout();

    host.resize(1000, 500);
    SettleLayout();
    EXPECT_EQ(overlay.geometry(), host.rect());
}

} // namespace
} // namespace exosnap::ui::dialogs
