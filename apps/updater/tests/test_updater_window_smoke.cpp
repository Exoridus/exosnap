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
#include <QCoreApplication>
#include <QLabel>
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

UpdaterUiState Terminal(FailureCase which) {
    UpdaterController c(QStringLiteral("0.8.1"), QStringLiteral("0.9.0"));
    c.onFailure(which, QStringLiteral("1603"));
    return c.state();
}

class UpdaterWindowTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() { EnsureApplication(); }
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

TEST_F(UpdaterWindowTest, CloseIsAllowedOnTerminalStates) {
    UpdaterWindow window;
    window.render(Terminal(FailureCase::VerifyInstallFailed));
    EXPECT_TRUE(window.closeEnabled());
}

TEST_F(UpdaterWindowTest, RedVariantFooterButtons) {
    UpdaterWindow window;
    window.render(Terminal(FailureCase::VerifyInstallFailed));
    const QStringList buttons = window.footerButtonLabels();
    ASSERT_EQ(buttons.size(), 2);
    EXPECT_EQ(buttons[0], QStringLiteral("Retry"));
    EXPECT_EQ(buttons[1], QStringLiteral("Open current version"));
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
        EXPECT_FALSE(text.contains(QStringLiteral("Keep your computer on")))
            << text.toStdString();
}

} // namespace
