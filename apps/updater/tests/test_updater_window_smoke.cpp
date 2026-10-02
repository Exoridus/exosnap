// test_updater_window_smoke.cpp -- the updater frontend's visible contract.
//
// The window is Qt Quick now, rendered from the same UpdaterController state as
// before through UpdaterViewAdapter. The projection is asserted directly on the
// adapter (step labels, footer captions, close policy, terminal copy) and the
// shipped executable is launched once per canned preview state to prove the real
// Quick window loads, renders and exits cleanly -- the guarantee the Widgets
// smoke test gave.
//
// The process runs are offscreen and use Qt's software renderer on purpose: the
// updater must stay readable in restricted graphics environments, so the smoke
// must not depend on a shader-only path.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include "UpdaterArgs.h"
#include "UpdaterController.h"
#include "UpdaterExePath.h"
#include "UpdaterViewAdapter.h"

using namespace exosnap::updater;

namespace {

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

bool RunPreviewSmoke(const QString& state) {
    QProcess process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    env.insert(QStringLiteral("QT_QUICK_BACKEND"), QStringLiteral("software"));
    process.setProcessEnvironment(env);
    process.start(QString::fromUtf8(EXOSNAP_UPDATER_EXE),
                  {QStringLiteral("--preview-state"), state, QStringLiteral("--preview-smoke")});
    if (!process.waitForStarted(10000))
        return false;
    if (!process.waitForFinished(30000)) {
        process.kill();
        process.waitForFinished(5000);
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

} // namespace

TEST(UpdaterViewAdapterTest, StepLabelsAreTheFiveFixedCanonStrings) {
    const QStringList labels = UpdaterViewAdapter::stepLabels();
    ASSERT_EQ(labels.size(), 5);
    EXPECT_EQ(labels[0], QStringLiteral("Downloading update"));
    EXPECT_EQ(labels[1], QStringLiteral("Closing previous version"));
    EXPECT_EQ(labels[2], QStringLiteral("Installing new files"));
    EXPECT_EQ(labels[3], QStringLiteral("Verifying installation"));
    EXPECT_EQ(labels[4], QStringLiteral("Launching ExoSnap"));
}

TEST(UpdaterViewAdapterTest, RenderShowsEveryStepLabelAndState) {
    UpdaterViewAdapter adapter;
    adapter.render(InstallInFlight());

    ASSERT_EQ(adapter.stepRows().size(), 5);
    const QStringList labels = UpdaterViewAdapter::stepLabels();
    for (int i = 0; i < labels.size(); ++i)
        EXPECT_EQ(adapter.stepRows().at(i).toMap().value(QStringLiteral("label")).toString(), labels.at(i));
    EXPECT_EQ(adapter.stepRows().at(0).toMap().value(QStringLiteral("status")).toString(), QStringLiteral("done"));
    EXPECT_EQ(adapter.stepRows().at(2).toMap().value(QStringLiteral("status")).toString(), QStringLiteral("working"));
    EXPECT_EQ(adapter.stepRows().at(3).toMap().value(QStringLiteral("status")).toString(), QStringLiteral("queued"));
}

TEST(UpdaterViewAdapterTest, CloseIsBlockedWhileInstallVerifyOrLaunchIsWorking) {
    UpdaterViewAdapter adapter;
    adapter.render(InstallInFlight());
    EXPECT_FALSE(adapter.closeEnabled());
    EXPECT_FALSE(adapter.requestClose());

    adapter.render(VerifyInFlight());
    EXPECT_FALSE(adapter.closeEnabled());
    EXPECT_FALSE(adapter.requestClose());

    adapter.render(LaunchInFlight());
    EXPECT_FALSE(adapter.closeEnabled());
    EXPECT_FALSE(adapter.requestClose());
}

TEST(UpdaterViewAdapterTest, CloseIsAllowedOnTerminalStates) {
    UpdaterViewAdapter adapter;
    int closed = 0;
    QObject::connect(&adapter, &UpdaterViewAdapter::closeRequested, &adapter, [&closed]() { ++closed; });

    adapter.render(Terminal(FailureCase::InstallFailed));
    EXPECT_TRUE(adapter.closeEnabled());
    EXPECT_TRUE(adapter.requestClose());
    EXPECT_EQ(closed, 1);
}

TEST(UpdaterViewAdapterTest, SafeWorkingCloseAsksInsteadOfClosing) {
    UpdaterViewAdapter adapter;
    int closed = 0;
    int confirmations = 0;
    QObject::connect(&adapter, &UpdaterViewAdapter::closeRequested, &adapter, [&closed]() { ++closed; });
    QObject::connect(&adapter, &UpdaterViewAdapter::cancelConfirmationRequested, &adapter,
                     [&confirmations]() { ++confirmations; });

    UpdaterController c(QStringLiteral("0.9.0-rc4"), QStringLiteral("0.9.0-rc5"));
    c.onStepStarted(UpStep::Download);
    adapter.render(c.state());

    EXPECT_TRUE(adapter.closeEnabled());
    EXPECT_FALSE(adapter.requestClose());
    EXPECT_EQ(closed, 0);
    EXPECT_EQ(confirmations, 1);
    EXPECT_TRUE(adapter.cancelConfirmationVisible());

    adapter.confirmCancelAndClose();
    EXPECT_EQ(closed, 1);
    EXPECT_FALSE(adapter.cancelConfirmationVisible());
}

TEST(UpdaterViewAdapterTest, RedVariantFooterButtons) {
    UpdaterViewAdapter adapter;
    adapter.render(Terminal(FailureCase::VerifyInstallFailed));
    EXPECT_EQ(adapter.footerButtonLabels(),
              QStringList({QStringLiteral("Retry"), QStringLiteral("Close")}));
}

TEST(UpdaterViewAdapterTest, GreenVariantFooterButtons) {
    UpdaterUiState state = Terminal(FailureCase::LaunchFailed);
    ASSERT_EQ(state.variant, TerminalVariant::Green);
    UpdaterViewAdapter adapter;
    adapter.render(state);
    EXPECT_TRUE(adapter.footerButtonLabels().contains(QStringLiteral("Open ExoSnap")));
}

TEST(UpdaterViewAdapterTest, MsiRebootRequiredHasSingleCloseButtonAndRestartHeadline) {
    UpdaterViewAdapter adapter;
    adapter.render(Terminal(FailureCase::MsiRebootRequired));
    EXPECT_EQ(adapter.statusHeadline(), QStringLiteral("Update installed — restart Windows to finish"));
    EXPECT_EQ(adapter.footerButtonLabels(), QStringList({QStringLiteral("Close")}));
}

TEST(UpdaterViewAdapterTest, MsiVerifyFailureDoesNotClaimAConfirmedRollback) {
    UpdaterViewAdapter adapter;
    adapter.render(Terminal(FailureCase::VerifyInstallFailedMsi));
    // The MSI path cannot confirm the post-install state; the copy must say so
    // rather than claim the previous version was restored.
    EXPECT_TRUE(adapter.panelSafety().contains(QStringLiteral("Windows Installer"))
                || adapter.panelDetail().contains(QStringLiteral("Windows Installer"))
                || adapter.panelSafety().contains(QStringLiteral("could not be confirmed"))
                || adapter.panelDetail().contains(QStringLiteral("could not be confirmed")));
    EXPECT_FALSE(adapter.panelTitle().contains(QStringLiteral("restored")));
}

TEST(UpdaterViewAdapterTest, CriticalInProgressKeepsDisabledCloseAction) {
    UpdaterViewAdapter adapter;
    adapter.render(InstallInFlight());
    EXPECT_TRUE(adapter.closeActionVisible());
    EXPECT_FALSE(adapter.closeActionEnabled());
    EXPECT_EQ(adapter.closeActionLabel(), QStringLiteral("Close"));
    EXPECT_EQ(adapter.hint(), QStringLiteral("This phase cannot be interrupted."));
}

TEST(UpdaterViewAdapterTest, GreenVariantRendersLaunchRowTagAsManual) {
    UpdaterViewAdapter adapter;
    adapter.render(Terminal(FailureCase::LaunchFailed));
    EXPECT_EQ(adapter.stepRows().at(4).toMap().value(QStringLiteral("tag")).toString(), QStringLiteral("manual"));
}

TEST(UpdaterViewAdapterTest, RedAndAmberVariantsRenderFailedRowTagAsFailed) {
    UpdaterViewAdapter adapter;
    adapter.render(Terminal(FailureCase::VerifyInstallFailed));
    EXPECT_EQ(adapter.stepRows().at(3).toMap().value(QStringLiteral("tag")).toString(), QStringLiteral("failed"));

    adapter.render(Terminal(FailureCase::InstallFailed));
    EXPECT_EQ(adapter.stepRows().at(2).toMap().value(QStringLiteral("tag")).toString(), QStringLiteral("failed"));
}

TEST(UpdaterViewAdapterTest, TerminalAmberHasNoKeepOnNote) {
    UpdaterViewAdapter adapter;
    adapter.render(Terminal(FailureCase::InstallFailed));
    EXPECT_FALSE(adapter.hint().contains(QStringLiteral("Cancelling discards")));
    EXPECT_TRUE(adapter.hint().isEmpty());
}

TEST(UpdaterViewAdapterTest, AppWontCloseNamesTheActionTheModeOffers) {
    UpdaterController controller(QStringLiteral("0.9.0-rc4"), QStringLiteral("0.9.0-rc5"));
    controller.setMode(exosnap::update::UpdaterMode::Manual);
    controller.onStepDone(UpStep::Download);
    controller.onFailure(FailureCase::AppWontClose, QString());
    UpdaterViewAdapter adapter;
    adapter.render(controller.state());
    EXPECT_FALSE(controller.state().secondary_action.isEmpty());
    EXPECT_EQ(adapter.secondaryAction(), controller.state().secondary_action);
}

// The real Quick window, one per canned state. This is the smoke that proves
// the QML module, the shared theme and the bundled fonts all load and render in
// the shipped executable.
TEST(UpdaterQuickWindowSmoke, EveryPreviewStateStartsAndRendersOffscreen) {
    ASSERT_FALSE(PreviewStateNames().isEmpty());
    for (const QString& state : PreviewStateNames())
        EXPECT_TRUE(RunPreviewSmoke(state)) << "preview state: " << state.toStdString();
}
