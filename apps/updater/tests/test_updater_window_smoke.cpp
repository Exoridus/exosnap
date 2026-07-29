// test_updater_window_smoke.cpp -- widget smoke tests for the updater window.
//
// UpdaterWindow / ProgressRing / StepListWidget are QtWidgets, so this binary
// owns a single QApplication (created once per test binary; CTest isolates each
// binary in its own process). The tests drive the window purely through
// render(const UpdaterUiState&) and assert the visible contract: the five fixed
// step labels, the footer button captions per terminal variant, and the
// close-X enabled/disabled flips while a swap is mid-flight.

#include <gtest/gtest.h>

#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QStringList>

#include <array>

#include "StepListWidget.h"
#include "UpdaterController.h"
#include "UpdaterWindow.h"

namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "updater_window_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

// Progress state with Install mid-flight: close must be blocked.
UpdaterUiState InstallInFlight() {
    UpdaterController c(QStringLiteral("0.8.1"), QStringLiteral("0.9.0"));
    c.onStepDone(UpStep::Download);
    c.onStepDone(UpStep::CloseApp);
    c.onStepStarted(UpStep::Install);
    return c.state();
}

UpdaterUiState VerifyInFlight() {
    UpdaterController c(QStringLiteral("0.8.1"), QStringLiteral("0.9.0"));
    c.onStepDone(UpStep::Download);
    c.onStepDone(UpStep::CloseApp);
    c.onStepDone(UpStep::Install);
    c.onStepStarted(UpStep::Verify);
    return c.state();
}

UpdaterUiState LaunchInFlight() {
    UpdaterController c(QStringLiteral("0.8.1"), QStringLiteral("0.9.0"));
    c.onStepDone(UpStep::Download);
    c.onStepDone(UpStep::CloseApp);
    c.onStepDone(UpStep::Install);
    c.onStepDone(UpStep::Verify);
    c.onStepStarted(UpStep::Launch);
    return c.state();
}

UpdaterUiState Terminal(FailureCase which) {
    UpdaterController c(QStringLiteral("0.8.1"), QStringLiteral("0.9.0"));
    c.onFailure(which, QStringLiteral("1603"));
    return c.state();
}

class UpdaterWindowTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(UpdaterWindowTest, StepLabelsAreTheFiveFixedCanonStrings) {
    const std::array<QString, 5> labels = UpdaterWindow::stepLabels();
    EXPECT_EQ(labels[0], QStringLiteral("Downloading update"));
    EXPECT_EQ(labels[1], QStringLiteral("Closing previous version"));
    EXPECT_EQ(labels[2], QStringLiteral("Installing new files"));
    EXPECT_EQ(labels[3], QStringLiteral("Verifying installation"));
    EXPECT_EQ(labels[4], QStringLiteral("Launching ExoSnap"));
}

TEST_F(UpdaterWindowTest, RenderShowsEveryStepLabelInTheWidgetTree) {
    UpdaterWindow window;
    window.render(InstallInFlight());

    QStringList seen;
    for (auto* label : window.findChildren<QLabel*>())
        seen << label->text();
    for (const QString& expected : UpdaterWindow::stepLabels())
        EXPECT_TRUE(seen.contains(expected)) << expected.toStdString();
}

TEST_F(UpdaterWindowTest, CloseIsBlockedWhileInstallIsWorking) {
    UpdaterWindow window;
    window.render(InstallInFlight());
    EXPECT_FALSE(window.closeEnabled());
}

TEST_F(UpdaterWindowTest, CloseIsBlockedWhileVerifyIsWorking) {
    UpdaterWindow window;
    window.render(VerifyInFlight());
    EXPECT_FALSE(window.closeEnabled());
}

TEST_F(UpdaterWindowTest, CloseIsBlockedWhileLaunchIsWorking) {
    UpdaterWindow window;
    window.render(LaunchInFlight());
    EXPECT_FALSE(window.closeEnabled());
}

TEST_F(UpdaterWindowTest, CloseIsAllowedOnTerminalStates) {
    UpdaterWindow window;
    window.render(Terminal(FailureCase::VerifyInstallFailed));
    EXPECT_TRUE(window.closeEnabled());
}

// The in-window close button is only half the guard -- Alt+F4, the taskbar
// close, and a Windows logoff all raise WM_CLOSE / QCloseEvent directly,
// bypassing the disabled button entirely. closeEvent() must refuse those too
// while a swap is mid-flight, or a force-kill between StageRename's two
// renames can strand the install directory.
TEST_F(UpdaterWindowTest, CloseEventIsIgnoredWhileInstallIsWorking) {
    UpdaterWindow window;
    window.render(InstallInFlight());
    QCloseEvent event;
    QCoreApplication::sendEvent(&window, &event);
    EXPECT_FALSE(event.isAccepted());
}

TEST_F(UpdaterWindowTest, CloseEventIsIgnoredWhileVerifyIsWorking) {
    UpdaterWindow window;
    window.render(VerifyInFlight());
    QCloseEvent event;
    QCoreApplication::sendEvent(&window, &event);
    EXPECT_FALSE(event.isAccepted());
}

TEST_F(UpdaterWindowTest, CloseEventIsIgnoredWhileLaunchIsWorking) {
    UpdaterWindow window;
    window.render(LaunchInFlight());
    QCloseEvent event;
    QCoreApplication::sendEvent(&window, &event);
    EXPECT_FALSE(event.isAccepted());
}

TEST_F(UpdaterWindowTest, CloseEventIsAcceptedOnTerminalStates) {
    UpdaterWindow window;
    window.render(Terminal(FailureCase::VerifyInstallFailed));
    QCloseEvent event;
    QCoreApplication::sendEvent(&window, &event);
    EXPECT_TRUE(event.isAccepted());
}

TEST_F(UpdaterWindowTest, RedVariantFooterButtons) {
    UpdaterWindow window;
    window.render(Terminal(FailureCase::VerifyInstallFailed));
    const QStringList buttons = window.footerButtonLabels();
    ASSERT_EQ(buttons.size(), 2);
    EXPECT_EQ(buttons[0], QStringLiteral("Retry"));
    EXPECT_EQ(buttons[1], QStringLiteral("Open current version"));
}

TEST_F(UpdaterWindowTest, TerminalHeadlineSafetyAndActionsShareOneResultCard) {
    UpdaterWindow window;
    window.render(Terminal(FailureCase::DownloadFailed));

    auto* card = window.findChild<QWidget*>(QStringLiteral("updaterResultCard"));
    auto* headline = window.findChild<QLabel*>(QStringLiteral("updaterResultHeadline"));
    auto* detail = window.findChild<QLabel*>(QStringLiteral("updaterResultDetail"));
    auto* safety = window.findChild<QLabel*>(QStringLiteral("updaterSafetyText"));
    auto* retry = window.findChild<QPushButton*>(QStringLiteral("updaterRetryButton"));
    ASSERT_NE(card, nullptr);
    ASSERT_NE(headline, nullptr);
    ASSERT_NE(detail, nullptr);
    ASSERT_NE(safety, nullptr);
    ASSERT_NE(retry, nullptr);
    EXPECT_TRUE(card->isAncestorOf(headline));
    EXPECT_TRUE(card->isAncestorOf(detail));
    EXPECT_TRUE(card->isAncestorOf(safety));
    EXPECT_TRUE(card->isAncestorOf(retry));
    EXPECT_FALSE(retry->icon().isNull());
    EXPECT_EQ(retry->accessibleName(), QStringLiteral("Retry"));
}

TEST_F(UpdaterWindowTest, ModeLabelIsQuietAndVerifyModeRemainsExplicit) {
    UpdaterWindow window;
    UpdaterUiState state = InstallInFlight();
    state.verification_reinstall = true;
    window.render(state);

    auto* mode = window.findChild<QLabel*>(QStringLiteral("updaterModeTag"));
    ASSERT_NE(mode, nullptr);
    EXPECT_EQ(mode->text(), QStringLiteral("UPDATER \xc2\xb7 VERIFY"));
    EXPECT_TRUE(mode->styleSheet().contains(QStringLiteral("border:none")));
    EXPECT_FALSE(mode->styleSheet().contains(QStringLiteral("border-radius")));
    EXPECT_EQ(mode->accessibleName(), QStringLiteral("Updater mode"));
}

TEST_F(UpdaterWindowTest, GreenVariantFooterButtons) {
    UpdaterWindow window;
    window.render(Terminal(FailureCase::LaunchFailed));
    const QStringList buttons = window.footerButtonLabels();
    ASSERT_EQ(buttons.size(), 2);
    EXPECT_EQ(buttons[0], QStringLiteral("Open ExoSnap"));
    EXPECT_EQ(buttons[1], QStringLiteral("Close"));
}

TEST_F(UpdaterWindowTest, MsiRedVariantHasSinglePrimaryButton) {
    UpdaterWindow window;
    window.render(Terminal(FailureCase::MsiFailed));
    const QStringList buttons = window.footerButtonLabels();
    ASSERT_EQ(buttons.size(), 1);
    EXPECT_EQ(buttons[0], QStringLiteral("Close"));
}

TEST_F(UpdaterWindowTest, MsiRebootRequiredHasSingleCloseButton) {
    UpdaterWindow window;
    window.render(Terminal(FailureCase::MsiRebootRequired));
    const QStringList buttons = window.footerButtonLabels();
    ASSERT_EQ(buttons.size(), 1);
    EXPECT_EQ(buttons[0], QStringLiteral("Close"));
    // Close must be allowed: the install already applied, nothing is mid-flight.
    EXPECT_TRUE(window.closeEnabled());
}

TEST_F(UpdaterWindowTest, MsiRebootRequiredHeadlineMentionsRestart) {
    UpdaterWindow window;
    window.render(Terminal(FailureCase::MsiRebootRequired));

    QStringList seen;
    for (auto* label : window.findChildren<QLabel*>())
        seen << label->text();
    bool mentions_restart = false;
    for (const QString& text : seen)
        mentions_restart = mentions_restart || text.contains(QStringLiteral("restart Windows"));
    EXPECT_TRUE(mentions_restart);
}

TEST_F(UpdaterWindowTest, MsiVerifyFailureDoesNotClaimAConfirmedRollback) {
    UpdaterWindow window;
    window.render(Terminal(FailureCase::VerifyInstallFailedMsi));

    QStringList seen;
    for (auto* label : window.findChildren<QLabel*>())
        seen << label->text();
    bool could_not_confirm = false;
    bool restored = false;
    for (const QString& text : seen) {
        could_not_confirm = could_not_confirm || text.contains(QStringLiteral("Couldn't confirm"));
        restored = restored || text.contains(QStringLiteral("was restored"));
    }
    EXPECT_TRUE(could_not_confirm);
    EXPECT_FALSE(restored);
    // Same actions as the portable B3 red card.
    const QStringList buttons = window.footerButtonLabels();
    ASSERT_EQ(buttons.size(), 2);
    EXPECT_EQ(buttons[0], QStringLiteral("Retry"));
    EXPECT_EQ(buttons[1], QStringLiteral("Open current version"));
}

TEST_F(UpdaterWindowTest, InProgressStateHasNoFooterButtons) {
    UpdaterWindow window;
    window.render(InstallInFlight());
    EXPECT_TRUE(window.footerButtonLabels().isEmpty());
}

// Green is a soft success (the update installed fine; only the auto-relaunch
// didn't happen), so the Launch row must read as an action to take ("manual"),
// never as an error ("failed"). Red/Amber variants are real failures and must
// keep the "failed" tag.
TEST_F(UpdaterWindowTest, GreenVariantRendersLaunchRowTagAsManual) {
    UpdaterWindow window;
    window.render(Terminal(FailureCase::LaunchFailed));

    QStringList seen;
    for (auto* label : window.findChildren<QLabel*>())
        seen << label->text();
    EXPECT_TRUE(seen.contains(QStringLiteral("manual")));
    EXPECT_FALSE(seen.contains(QStringLiteral("failed")));
}

TEST_F(UpdaterWindowTest, RedVariantRendersFailedRowTagAsFailed) {
    UpdaterWindow window;
    window.render(Terminal(FailureCase::VerifyInstallFailed));

    QStringList seen;
    for (auto* label : window.findChildren<QLabel*>())
        seen << label->text();
    EXPECT_TRUE(seen.contains(QStringLiteral("failed")));
    EXPECT_FALSE(seen.contains(QStringLiteral("manual")));
}

TEST_F(UpdaterWindowTest, AmberVariantRendersFailedRowTagAsFailed) {
    UpdaterWindow window;
    window.render(Terminal(FailureCase::InstallFailed));

    QStringList seen;
    for (auto* label : window.findChildren<QLabel*>())
        seen << label->text();
    EXPECT_TRUE(seen.contains(QStringLiteral("failed")));
    EXPECT_FALSE(seen.contains(QStringLiteral("manual")));
}

// Terminal Amber must not repeat the "keep your computer on" note -- that is
// an in-progress-only affordance, and the terminal footer sentence already
// says the current version is safe.
TEST_F(UpdaterWindowTest, TerminalAmberHasNoKeepOnNote) {
    UpdaterWindow window;
    window.render(Terminal(FailureCase::InstallFailed));

    QStringList seen;
    for (auto* label : window.findChildren<QLabel*>())
        seen << label->text();
    for (const QString& text : seen)
        EXPECT_FALSE(text.contains(QStringLiteral("Keep your computer on"))) << text.toStdString();
}

} // namespace
