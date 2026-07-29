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
#include <QEventLoop>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QStringList>

#include <array>
#include <memory>

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
    UpdaterController c(QStringLiteral("0.9.0-rc4"), QStringLiteral("0.9.0-rc5"));
    c.onFailure(which, QStringLiteral("1603"));
    return c.state();
}

void Settle(UpdaterWindow& window) {
    window.move(-20000, -20000);
    window.show();
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        window.ensurePolished();
    }
}

void ExpectLabelFitsOneLine(const UpdaterWindow& window, const char* object_name) {
    const auto* label = window.findChild<QLabel*>(QString::fromLatin1(object_name));
    ASSERT_NE(label, nullptr) << object_name;
    EXPECT_GE(label->width(), label->fontMetrics().horizontalAdvance(label->text()))
        << object_name << " wrapped or clipped: " << label->text().toStdString();
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
    EXPECT_EQ(buttons[1], QStringLiteral("Close"));
}

TEST_F(UpdaterWindowTest, TerminalCopyStaysInResultCardAndActionsUseFixedExternalRow) {
    UpdaterWindow window;
    window.render(Terminal(FailureCase::DownloadFailed));

    auto* card = window.findChild<QWidget*>(QStringLiteral("updaterResultCard"));
    auto* headline = window.findChild<QLabel*>(QStringLiteral("updaterResultHeadline"));
    auto* detail = window.findChild<QLabel*>(QStringLiteral("updaterResultDetail"));
    auto* safety = window.findChild<QLabel*>(QStringLiteral("updaterSafetyText"));
    auto* retry = window.findChild<QPushButton*>(QStringLiteral("updaterRetryButton"));
    auto* action_row = window.findChild<QWidget*>(QStringLiteral("updaterActionRow"));
    ASSERT_NE(card, nullptr);
    ASSERT_NE(headline, nullptr);
    ASSERT_NE(detail, nullptr);
    ASSERT_NE(safety, nullptr);
    ASSERT_NE(retry, nullptr);
    ASSERT_NE(action_row, nullptr);
    EXPECT_TRUE(card->isAncestorOf(headline));
    EXPECT_TRUE(card->isAncestorOf(detail));
    EXPECT_TRUE(card->isAncestorOf(safety));
    EXPECT_FALSE(card->isAncestorOf(retry));
    EXPECT_TRUE(action_row->isAncestorOf(retry));
    EXPECT_FALSE(retry->icon().isNull());
    EXPECT_EQ(retry->accessibleName(), QStringLiteral("Retry"));
}

TEST_F(UpdaterWindowTest, TitleBarUsesSingleLineExoSnapUpdaterIdentityWithoutStatus) {
    UpdaterWindow window;
    UpdaterUiState state = InstallInFlight();
    state.verification_reinstall = true;
    window.render(state);

    auto* wordmark = window.findChild<QLabel*>(QStringLiteral("updaterWordmark"));
    auto* title = window.findChild<QLabel*>(QStringLiteral("updaterTitle"));
    auto* title_bar = window.findChild<QWidget*>(QStringLiteral("updaterTitleBar"));
    auto* minimize = window.findChild<QPushButton*>(QStringLiteral("updaterMinimizeButton"));
    auto* close = window.findChild<QPushButton*>(QStringLiteral("updaterCloseButton"));
    ASSERT_NE(wordmark, nullptr);
    ASSERT_NE(title, nullptr);
    ASSERT_NE(title_bar, nullptr);
    ASSERT_NE(minimize, nullptr);
    ASSERT_NE(close, nullptr);
    EXPECT_TRUE(wordmark->text().contains(QStringLiteral("exo")));
    EXPECT_TRUE(wordmark->text().contains(QStringLiteral("snap")));
    EXPECT_EQ(title->text(), QStringLiteral("Updater"));
    EXPECT_EQ(title_bar->height(), 56);
    EXPECT_EQ(minimize->size(), QSize(46, 56));
    EXPECT_EQ(close->size(), QSize(46, 56));
    EXPECT_EQ(window.findChild<QPushButton*>(QStringLiteral("updaterMaximizeButton")), nullptr);
    EXPECT_EQ(window.findChild<QLabel*>(QStringLiteral("updaterTitleStatus")), nullptr);
    EXPECT_EQ(window.findChild<QLabel*>(QStringLiteral("updaterTitleDetail")), nullptr);
    EXPECT_EQ(window.findChild<QLabel*>(QStringLiteral("updaterVerifyTag")), nullptr);
}

TEST_F(UpdaterWindowTest, WindowDimensionsStayFixedAcrossWorkingAndTerminalStates) {
    UpdaterWindow working;
    working.render(InstallInFlight());
    const QSize expected = working.size();

    UpdaterWindow warning;
    warning.render(Terminal(FailureCase::InstallFailed));
    UpdaterWindow error;
    error.render(Terminal(FailureCase::VerifyInstallFailed));
    UpdaterWindow completed;
    completed.render(Terminal(FailureCase::LaunchFailed));

    EXPECT_EQ(expected, QSize(520, 680));
    EXPECT_EQ(working.minimumSize(), expected);
    EXPECT_EQ(working.maximumSize(), expected);
    EXPECT_EQ(warning.size(), expected);
    EXPECT_EQ(error.size(), expected);
    EXPECT_EQ(completed.size(), expected);
}

TEST_F(UpdaterWindowTest, StatePanelAndActionRowKeepTheSameGeometryAcrossStates) {
    struct Geometry {
        QRect panel;
        QRect actions;
    };
    const auto geometryFor = [](const UpdaterUiState& state) {
        auto window = std::make_unique<UpdaterWindow>();
        window->render(state);
        Settle(*window);
        auto* panel = window->findChild<QWidget*>(QStringLiteral("updaterWorkingPanel"));
        if (panel == nullptr)
            panel = window->findChild<QWidget*>(QStringLiteral("updaterResultCard"));
        auto* actions = window->findChild<QWidget*>(QStringLiteral("updaterActionRow"));
        EXPECT_NE(panel, nullptr);
        EXPECT_NE(actions, nullptr);
        return Geometry{panel != nullptr ? panel->geometry() : QRect{},
                        actions != nullptr ? actions->geometry() : QRect{}};
    };

    const Geometry working = geometryFor(InstallInFlight());
    const Geometry warning = geometryFor(Terminal(FailureCase::InstallFailed));
    const Geometry success = geometryFor(Terminal(FailureCase::LaunchFailed));
    EXPECT_EQ(working.panel, warning.panel);
    EXPECT_EQ(working.panel, success.panel);
    EXPECT_EQ(working.actions, warning.actions);
    EXPECT_EQ(working.actions, success.actions);
    EXPECT_EQ(working.panel.height(), 110);
    EXPECT_EQ(working.actions.height(), 36);
}

TEST_F(UpdaterWindowTest, EveryTerminalResultKeepsItsThreeCopyRowsOnOneLine) {
    constexpr std::array<FailureCase, 12> failures = {
        FailureCase::DownloadFailed,
        FailureCase::VerifyDownloadFailed,
        FailureCase::VerifyReinstallMismatch,
        FailureCase::AppWontClose,
        FailureCase::InstallFailed,
        FailureCase::VerifyInstallFailed,
        FailureCase::RestoreFailed,
        FailureCase::VerifyInstallFailedMsi,
        FailureCase::LaunchFailed,
        FailureCase::UacDeclined,
        FailureCase::MsiFailed,
        FailureCase::MsiRebootRequired,
    };

    for (const FailureCase failure : failures) {
        UpdaterWindow window;
        window.render(Terminal(failure));
        Settle(window);
        ExpectLabelFitsOneLine(window, "updaterResultHeadline");
        ExpectLabelFitsOneLine(window, "updaterResultDetail");
        ExpectLabelFitsOneLine(window, "updaterSafetyText");
    }
}

TEST_F(UpdaterWindowTest, WorkingPanelKeepsAllThreeCopyRowsOnOneLine) {
    UpdaterWindow window;
    window.render(InstallInFlight());
    Settle(window);
    ExpectLabelFitsOneLine(window, "updaterWorkingTitle");
    ExpectLabelFitsOneLine(window, "updaterWorkingDetail");
    ExpectLabelFitsOneLine(window, "updaterWorkingSafety");
}

TEST_F(UpdaterWindowTest, SafeWorkingPhasesExposeCancelSemanticsThroughCloseControl) {
    UpdaterController controller(QStringLiteral("0.8.1"), QStringLiteral("0.9.0"));
    controller.onStepStarted(UpStep::Download);
    UpdaterWindow window;
    window.render(controller.state());

    EXPECT_TRUE(window.closeEnabled());
    EXPECT_TRUE(window.footerButtonLabels().contains(QStringLiteral("Cancel update")));
    auto* cancel = window.findChild<QPushButton*>(QStringLiteral("updaterCancelButton"));
    ASSERT_NE(cancel, nullptr);
    Settle(window);
    cancel->click();
    EXPECT_TRUE(window.cancelConfirmationVisible());
    auto* dialog = window.findChild<QWidget*>(QStringLiteral("updaterCancelDialog"));
    ASSERT_NE(dialog, nullptr);
    EXPECT_TRUE(dialog->isVisibleTo(&window));
}

TEST_F(UpdaterWindowTest, SafeWorkingCloseEventShowsConfirmationInsteadOfClosing) {
    UpdaterController controller(QStringLiteral("0.8.1"), QStringLiteral("0.9.0"));
    controller.onStepStarted(UpStep::Download);
    UpdaterWindow window;
    window.render(controller.state());
    Settle(window);

    QCloseEvent event;
    QCoreApplication::sendEvent(&window, &event);
    EXPECT_FALSE(event.isAccepted());
    EXPECT_TRUE(window.cancelConfirmationVisible());
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
    EXPECT_EQ(buttons[1], QStringLiteral("Close"));
}

TEST_F(UpdaterWindowTest, CriticalInProgressStateKeepsDisabledCloseInFixedActionRow) {
    UpdaterWindow window;
    window.render(InstallInFlight());
    EXPECT_EQ(window.footerButtonLabels(), QStringList{QStringLiteral("Close")});
    auto* action_row = window.findChild<QWidget*>(QStringLiteral("updaterActionRow"));
    ASSERT_NE(action_row, nullptr);
    const auto buttons = action_row->findChildren<QPushButton*>();
    ASSERT_EQ(buttons.size(), 1);
    EXPECT_FALSE(buttons.front()->isEnabled());
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
