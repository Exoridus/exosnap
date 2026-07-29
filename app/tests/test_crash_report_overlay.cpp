#include <gtest/gtest.h>

#include <QAccessible>
#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QWidget>

#include "ui/dialogs/CrashReportOverlay.h"
#include "ui/dialogs/CrashReportPanel.h"
#include "ui/theme/ExoSnapTheme.h"
#include "ui/widgets/ExoCheckBox.h"

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "crash_report_overlay_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

ui::dialogs::CrashReportModel SampleModel(bool recording = false) {
    ui::dialogs::CrashReportModel model;
    model.recording_was_active = recording;
    model.exception = QStringLiteral("0xC0000005 \xc2\xb7 ACCESS_VIOLATION");
    model.module = QStringLiteral("exosnap.dll +0x3f1a2");
    model.thread = QStringLiteral("\"encoder\" (#7)");
    model.stack = {QStringLiteral("exo::EncoderNVENC::submitFrame()"), QStringLiteral("exo::Pipeline::onFrameReady()"),
                   QStringLiteral("exo::CaptureLoop::tick()")};
    model.version = QStringLiteral("1.0.4 \xc2\xb7 build a5d55f1");
    model.encoder = QStringLiteral("NVENC AV1 \xe2\x86\x92 MKV");
    model.crash_dir = QStringLiteral("crash-dir");
    model.dmp_path = QStringLiteral("crash-dir/report.dmp");
    return model;
}

// Returns the union of every label text inside a widget tree (for copy assertions).
bool ContainsLabel(const QWidget& root, const QString& needle) {
    for (auto* label : root.findChildren<QLabel*>())
        if (label->text().contains(needle))
            return true;
    return false;
}

class CrashReportTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(CrashReportTest, PanelRendersInWindowNotAsNativeDialog) {
    ui::dialogs::CrashReportPanel panel(SampleModel());
    // The crash surface must be a plain QWidget, never a separate native QDialog.
    EXPECT_EQ(qobject_cast<QDialog*>(&panel), nullptr);
    EXPECT_EQ(panel.objectName(), QStringLiteral("crashReportCard"));
}

TEST_F(CrashReportTest, OverlayRendersInWindowNotAsNativeDialog) {
    ui::dialogs::CrashReportOverlay overlay(SampleModel());
    EXPECT_EQ(qobject_cast<QDialog*>(&overlay), nullptr);
    // The card lives inside the overlay.
    EXPECT_NE(overlay.findChild<QWidget*>(QStringLiteral("crashReportCard")), nullptr);
}

TEST_F(CrashReportTest, RememberChoiceDefaultsUnchecked) {
    ui::dialogs::CrashReportPanel panel(SampleModel());
    auto* check = panel.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("crashRememberChoiceCheck"));
    ASSERT_NE(check, nullptr);
    EXPECT_FALSE(check->isChecked());
    EXPECT_FALSE(panel.rememberChoiceChecked());
}

TEST_F(CrashReportTest, PrivacyDisclosureIsCollapsedAndKeyboardAccessible) {
    ui::dialogs::CrashReportPanel panel(SampleModel());
    auto* toggle = panel.findChild<QPushButton*>(QStringLiteral("crashPrivacyDisclosure"));
    auto* details = panel.findChild<QWidget*>(QStringLiteral("crashPrivacyDetails"));
    ASSERT_NE(toggle, nullptr);
    ASSERT_NE(details, nullptr);

    EXPECT_TRUE(toggle->isCheckable());
    EXPECT_FALSE(toggle->isChecked());
    EXPECT_TRUE(details->isHidden());
    EXPECT_EQ(toggle->accessibleName(), QStringLiteral("What is included in this report?"));
    EXPECT_TRUE(toggle->accessibleDescription().contains(QStringLiteral("collapsed")));
    auto* accessible = QAccessible::queryAccessibleInterface(toggle);
    ASSERT_NE(accessible, nullptr);
    EXPECT_TRUE(accessible->state().checkable);
    EXPECT_FALSE(accessible->state().checked);

    toggle->click();
    EXPECT_TRUE(toggle->isChecked());
    EXPECT_FALSE(details->isHidden());
    EXPECT_TRUE(toggle->accessibleDescription().contains(QStringLiteral("expanded")));

    toggle->click();
    EXPECT_FALSE(toggle->isChecked());
    EXPECT_TRUE(details->isHidden());
}

TEST_F(CrashReportTest, SendButtonEmitsSendReportRequested) {
    ui::dialogs::CrashReportPanel panel(SampleModel());
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("crashSendButton"));
    ASSERT_NE(send, nullptr);

    int count = 0;
    QObject::connect(&panel, &ui::dialogs::CrashReportPanel::sendReportRequested, &panel, [&count]() { ++count; });
    send->click();
    EXPECT_EQ(count, 1);
}

// The panel appears on the launch *after* a crash, so the app is already running. The
// secondary action must dismiss the report, not restart what was just started.
TEST_F(CrashReportTest, SecondaryButtonDeclinesInsteadOfRestarting) {
    ui::dialogs::CrashReportPanel panel(SampleModel());
    EXPECT_EQ(panel.findChild<QPushButton*>(QStringLiteral("crashRestartButton")), nullptr)
        << "restarting an app that just started offers the user nothing";

    auto* decline = panel.findChild<QPushButton*>(QStringLiteral("crashDeclineButton"));
    ASSERT_NE(decline, nullptr);

    int count = 0;
    QObject::connect(&panel, &ui::dialogs::CrashReportPanel::dontSendRequested, &panel, [&count]() { ++count; });
    decline->click();
    EXPECT_EQ(count, 1);
}

TEST_F(CrashReportTest, RememberToggleOnlyChangesLocalDraftAndButtonCopy) {
    ui::dialogs::CrashReportPanel panel(SampleModel());
    auto* check = panel.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("crashRememberChoiceCheck"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("crashSendButton"));
    auto* decline = panel.findChild<QPushButton*>(QStringLiteral("crashDeclineButton"));
    auto* hint = panel.findChild<QLabel*>(QStringLiteral("crashRememberChoiceHint"));
    ASSERT_NE(check, nullptr);
    ASSERT_NE(send, nullptr);
    ASSERT_NE(decline, nullptr);
    ASSERT_NE(hint, nullptr);

    int action_count = 0;
    QObject::connect(&panel, &ui::dialogs::CrashReportPanel::sendReportRequested, [&]() { ++action_count; });
    QObject::connect(&panel, &ui::dialogs::CrashReportPanel::dontSendRequested, [&]() { ++action_count; });
    check->setChecked(true);
    EXPECT_EQ(action_count, 0);
    EXPECT_TRUE(panel.rememberChoiceChecked());
    EXPECT_EQ(send->text(), QStringLiteral("Send report"));
    EXPECT_EQ(decline->text(), QStringLiteral("Don't send"));
    EXPECT_TRUE(send->accessibleDescription().contains(QStringLiteral("automatic")));
    EXPECT_TRUE(decline->accessibleDescription().contains(QStringLiteral("stops future")));
    EXPECT_FALSE(hint->isHidden());
}

TEST_F(CrashReportTest, ChromeCloseIsNeutralDismissal) {
    ui::dialogs::CrashReportPanel panel(SampleModel());
    auto* close = panel.findChild<QPushButton*>(QStringLiteral("crashChromeCloseButton"));
    ASSERT_NE(close, nullptr);

    int dismiss_count = 0;
    int decline_count = 0;
    QObject::connect(&panel, &ui::dialogs::CrashReportPanel::dismissRequested, [&dismiss_count]() { ++dismiss_count; });
    QObject::connect(&panel, &ui::dialogs::CrashReportPanel::dontSendRequested,
                     [&decline_count]() { ++decline_count; });
    close->click();
    EXPECT_EQ(dismiss_count, 1);
    EXPECT_EQ(decline_count, 0);
}

TEST_F(CrashReportTest, RecordingBannerPresentOnlyWhenRecordingWasActive) {
    ui::dialogs::CrashReportPanel without(SampleModel(false));
    EXPECT_EQ(without.findChild<QFrame*>(QStringLiteral("crashRecordingBanner")), nullptr);

    ui::dialogs::CrashReportPanel with(SampleModel(true));
    auto* banner = with.findChild<QFrame*>(QStringLiteral("crashRecordingBanner"));
    ASSERT_NE(banner, nullptr);
    EXPECT_TRUE(ContainsLabel(with, QStringLiteral("available for recovery")));
}

TEST_F(CrashReportTest, PrivacyCopyDistinguishesDumpAndStructuredEvent) {
    ui::dialogs::CrashReportPanel panel(SampleModel());
    auto* toggle = panel.findChild<QPushButton*>(QStringLiteral("crashPrivacyDisclosure"));
    ASSERT_NE(toggle, nullptr);
    toggle->click();
    EXPECT_TRUE(ContainsLabel(panel, QStringLiteral("native dump is separate")));
    EXPECT_TRUE(ContainsLabel(panel, QStringLiteral("loaded-module paths")));
    EXPECT_TRUE(ContainsLabel(panel, QStringLiteral("Recordings or recording content")));
    const QString forbidden_preview_claim = QStringLiteral("exactly what ") + QStringLiteral("uploads");
    EXPECT_FALSE(ContainsLabel(panel, forbidden_preview_claim));
    EXPECT_FALSE(ContainsLabel(panel, QStringLiteral("File paths & filenames")));
    EXPECT_FALSE(ContainsLabel(panel, QStringLiteral("Usernames")));
}

TEST_F(CrashReportTest, SummaryRendersOnlyNonEmptyTechnicalFields) {
    ui::dialogs::CrashReportPanel panel(SampleModel());
    EXPECT_TRUE(ContainsLabel(panel, QStringLiteral("0xC0000005")));
    EXPECT_TRUE(ContainsLabel(panel, QStringLiteral("exosnap.dll +0x3f1a2")));
    EXPECT_TRUE(ContainsLabel(panel, QStringLiteral("submitFrame")));

    auto real_next_launch = SampleModel();
    real_next_launch.exception.clear();
    real_next_launch.module.clear();
    real_next_launch.thread.clear();
    real_next_launch.stack.clear();
    ui::dialogs::CrashReportPanel honest(real_next_launch);
    EXPECT_FALSE(ContainsLabel(honest, QStringLiteral("EXCEPTION")));
    EXPECT_FALSE(ContainsLabel(honest, QStringLiteral("MODULE")));
    EXPECT_FALSE(ContainsLabel(honest, QStringLiteral("THREAD")));
    EXPECT_FALSE(ContainsLabel(honest, QStringLiteral("STACK")));
    EXPECT_TRUE(ContainsLabel(honest, QStringLiteral("Not available locally")));
}

TEST_F(CrashReportTest, ThemeRebuildPreservesDraftWithoutCommittingAction) {
    ui::dialogs::CrashReportPanel panel(SampleModel());
    auto* remember = panel.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("crashRememberChoiceCheck"));
    auto* disclosure = panel.findChild<QPushButton*>(QStringLiteral("crashPrivacyDisclosure"));
    ASSERT_NE(remember, nullptr);
    ASSERT_NE(disclosure, nullptr);
    remember->setChecked(true);
    disclosure->click();

    int action_count = 0;
    QObject::connect(&panel, &ui::dialogs::CrashReportPanel::sendReportRequested, [&]() { ++action_count; });
    QObject::connect(&panel, &ui::dialogs::CrashReportPanel::dontSendRequested, [&]() { ++action_count; });
    ui::theme::ReapplyTheme(*EnsureApplication(), QStringLiteral("light-paper"));

    EXPECT_EQ(action_count, 0);
    EXPECT_TRUE(panel.rememberChoiceChecked());
    auto* rebuilt_disclosure = panel.findChild<QPushButton*>(QStringLiteral("crashPrivacyDisclosure"));
    ASSERT_NE(rebuilt_disclosure, nullptr);
    EXPECT_TRUE(rebuilt_disclosure->isChecked());
    ui::theme::ReapplyTheme(*EnsureApplication(), QStringLiteral("dark-default"));
}

TEST_F(CrashReportTest, ActionRowHasVisibleFolderButtonAndNoOverflowMenu) {
    ui::dialogs::CrashReportPanel panel(SampleModel());
    EXPECT_EQ(panel.findChild<QPushButton*>(QStringLiteral("crashOverflowButton")), nullptr);
    EXPECT_FALSE(ContainsLabel(panel, QStringLiteral("Report on GitHub")));

    auto* folder = panel.findChild<QPushButton*>(QStringLiteral("crashOpenFolderButton"));
    ASSERT_NE(folder, nullptr);
    EXPECT_EQ(folder->text(), QStringLiteral("Open crash folder"));
    EXPECT_EQ(folder->accessibleName(), QStringLiteral("Open crash folder"));
    EXPECT_TRUE(folder->isVisibleTo(&panel));

    int count = 0;
    QObject::connect(&panel, &ui::dialogs::CrashReportPanel::openCrashFolderRequested, &panel, [&count]() { ++count; });
    folder->click();
    EXPECT_EQ(count, 1);
}

TEST_F(CrashReportTest, ActionRowTextsAndFocusOrderAreStable) {
    ui::dialogs::CrashReportPanel panel(SampleModel());
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("crashSendButton"));
    auto* decline = panel.findChild<QPushButton*>(QStringLiteral("crashDeclineButton"));
    auto* folder = panel.findChild<QPushButton*>(QStringLiteral("crashOpenFolderButton"));
    ASSERT_NE(send, nullptr);
    ASSERT_NE(decline, nullptr);
    ASSERT_NE(folder, nullptr);

    EXPECT_EQ(send->text(), QStringLiteral("Send report"));
    EXPECT_EQ(decline->text(), QStringLiteral("Don't send"));
    EXPECT_EQ(folder->text(), QStringLiteral("Open crash folder"));
    EXPECT_EQ(send->nextInFocusChain(), decline);
    EXPECT_EQ(decline->nextInFocusChain(), folder);
}

TEST_F(CrashReportTest, OverlayForwardsSendReportSignal) {
    ui::dialogs::CrashReportOverlay overlay(SampleModel());
    auto* send = overlay.findChild<QPushButton*>(QStringLiteral("crashSendButton"));
    ASSERT_NE(send, nullptr);

    int count = 0;
    QObject::connect(&overlay, &ui::dialogs::CrashReportOverlay::sendReportRequested, &overlay,
                     [&count]() { ++count; });
    send->click();
    EXPECT_EQ(count, 1);
}

TEST_F(CrashReportTest, OverlayOpenThenCloseTogglesOpenState) {
    QWidget host;
    auto* overlay = new ui::dialogs::CrashReportOverlay(SampleModel(), &host);

    EXPECT_FALSE(overlay->isOpen());
    overlay->openOverlay();
    EXPECT_TRUE(overlay->isOpen());
    overlay->closeOverlay();
    EXPECT_FALSE(overlay->isOpen());
}

TEST_F(CrashReportTest, OverlayExplicitDeclineDismissesAndEmitsDontSend) {
    QWidget host;
    auto* overlay = new ui::dialogs::CrashReportOverlay(SampleModel(), &host);
    overlay->openOverlay();
    ASSERT_TRUE(overlay->isOpen());

    int closed_count = 0;
    int decline_count = 0;
    QObject::connect(overlay, &ui::dialogs::CrashReportOverlay::closed, overlay, [&closed_count]() { ++closed_count; });
    QObject::connect(overlay, &ui::dialogs::CrashReportOverlay::dontSendRequested, overlay,
                     [&decline_count]() { ++decline_count; });

    auto* decline = overlay->findChild<QPushButton*>(QStringLiteral("crashDeclineButton"));
    ASSERT_NE(decline, nullptr);
    decline->click();

    EXPECT_FALSE(overlay->isOpen());
    EXPECT_EQ(closed_count, 1);
    EXPECT_EQ(decline_count, 1);
}

TEST_F(CrashReportTest, OverlayEscapeWithRememberDraftIsNeutralDismissal) {
    QWidget host;
    auto* overlay = new ui::dialogs::CrashReportOverlay(SampleModel(), &host);
    overlay->openOverlay();
    auto* remember = overlay->findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("crashRememberChoiceCheck"));
    ASSERT_NE(remember, nullptr);
    remember->setChecked(true);

    int closed_count = 0;
    int send_count = 0;
    int decline_count = 0;
    QObject::connect(overlay, &ui::dialogs::CrashReportOverlay::closed, [&]() { ++closed_count; });
    QObject::connect(overlay, &ui::dialogs::CrashReportOverlay::sendReportRequested, [&]() { ++send_count; });
    QObject::connect(overlay, &ui::dialogs::CrashReportOverlay::dontSendRequested, [&]() { ++decline_count; });

    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(overlay, &escape);
    EXPECT_FALSE(overlay->isOpen());
    EXPECT_EQ(closed_count, 1);
    EXPECT_EQ(send_count, 0);
    EXPECT_EQ(decline_count, 0);
}

} // namespace
} // namespace exosnap
