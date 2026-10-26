// test_updater_text_layout.cpp -- the updater's copy and progress semantics,
// asserted on the projection the Quick window renders.
//
// The Widgets window kept arbitrary-length values (release metadata, msiexec
// text, safety copy) inside one-line eliding rows. The Quick window keeps the
// same contract with `elide` plus a tooltip/accessible description; what can be
// asserted headless is that the projection carries the full value and that the
// ring, eyebrow and emphasis rules are the ones the window has always shown.

#include <gtest/gtest.h>

#include <QLocale>
#include <QString>
#include <QTemporaryFile>

#include "UpdaterController.h"
#include "UpdaterViewAdapter.h"

using namespace exosnap::updater;

TEST(UpdaterRepairProjection, RepairActionRoutesOnlyWhenOffered) {
    UpdaterController controller("0.10.1", "0.11.0");
    controller.onFailure(FailureCase::VerifyInstallFailedMsi, {});
    UpdaterViewAdapter adapter;
    adapter.render(controller.state());
    int retries = 0;
    QObject::connect(&adapter, &UpdaterViewAdapter::retryRequested, [&] { ++retries; });
    adapter.activateAction("Repair ExoSnap");
    EXPECT_EQ(retries, 1);
    UpdaterController idle("0.10.1", "0.11.0");
    adapter.render(idle.state());
    adapter.activateAction("Repair ExoSnap");
    EXPECT_EQ(retries, 1);
}

TEST(UpdaterRepairProjection, LogActionDependsOnAnExistingDiagnosticFile) {
    UpdaterViewAdapter adapter;
    EXPECT_FALSE(adapter.logAvailable());
    QTemporaryFile log;
    ASSERT_TRUE(log.open());
    adapter.setLogPath(log.fileName());
    EXPECT_TRUE(adapter.logAvailable());
    ASSERT_TRUE(log.remove());
    EXPECT_FALSE(adapter.logAvailable());
}

TEST(UpdaterRepairProjection, InstallationStepSaysRepairing) {
    UpdaterController controller("0.10.1", "0.11.0");
    controller.onRepairStarted();
    controller.onStepDone(UpStep::CloseApp);
    controller.onStepStarted(UpStep::Install);
    UpdaterViewAdapter adapter;
    adapter.render(controller.state());
    EXPECT_EQ(adapter.panelTitle(), "Repairing ExoSnap");
    EXPECT_FALSE(adapter.closeEnabled());
}

TEST(UpdaterRepairProjection, PreparationBlocksCloseBeforeTheWorkerStarts) {
    UpdaterController controller("0.10.1", "0.11.0");
    controller.onRepairStarted();
    EXPECT_EQ(controller.flowState().phase, exosnap::update::UpdatePhase::WaitingForParent);
    UpdaterViewAdapter adapter;
    adapter.render(controller.state());
    EXPECT_FALSE(adapter.closeEnabled());
    EXPECT_FALSE(adapter.requestClose());
    EXPECT_FALSE(adapter.cancelConfirmationVisible());
}

namespace {

UpdaterUiState DownloadInFlight() {
    UpdaterController c(QStringLiteral("0.8.1"), QStringLiteral("0.9.0"));
    c.onStepStarted(UpStep::Download);
    c.onDownloadProgress(38000000, 100000000);
    return c.state();
}

UpdaterUiState Terminal(FailureCase which, const QString& detail = {}) {
    UpdaterController c(QStringLiteral("0.9.0-rc4"), QStringLiteral("0.9.0-rc5"));
    c.onStepDone(UpStep::Download);
    c.onStepDone(UpStep::CloseApp);
    c.onStepStarted(UpStep::Install);
    c.onFailure(which, detail);
    return c.state();
}

int CountOccurrences(const QStringList& rows, const QString& needle) {
    int count = 0;
    for (const QString& row : rows)
        count += row.count(needle);
    return count;
}

} // namespace

TEST(UpdaterTextLayoutTest, LongVersionsStayCompleteInTheProjection) {
    const QString long_from = QStringLiteral("0.10.0-rc1+20261001.abcdef123456.windows-x64-portable");
    const QString long_to = QStringLiteral("0.10.0-rc2+20261015.fedcba654321.windows-x64-portable");
    UpdaterViewAdapter adapter;
    UpdaterUiState state = DownloadInFlight();
    state.from_version = long_from;
    state.to_version = long_to;
    adapter.render(state);

    EXPECT_EQ(adapter.fromVersion(), long_from);
    EXPECT_EQ(adapter.toVersion(), long_to);
    EXPECT_TRUE(adapter.hasTarget());
}

TEST(UpdaterTextLayoutTest, ShortVersionsAreNotShortenedAtAll) {
    UpdaterViewAdapter adapter;
    adapter.render(DownloadInFlight());
    EXPECT_EQ(adapter.fromVersion(), QStringLiteral("0.8.1"));
    EXPECT_EQ(adapter.toVersion(), QStringLiteral("0.9.0"));
}

TEST(UpdaterTextLayoutTest, LongMsiDetailStaysCompleteInTheProjection) {
    const QString long_detail =
        QStringLiteral("Windows Installer returned 1603: the installation failed because a required file could not "
                       "be replaced while another process still held it open; close every ExoSnap window and try "
                       "again.");
    UpdaterViewAdapter adapter;
    UpdaterUiState state = Terminal(FailureCase::InstallFailed);
    // The controller keeps raw technical detail out of the UI copy; this seam is
    // the same one a long msiexec message arrives through in production.
    state.headline = QStringLiteral("Update didn't complete");
    state.detail_text = long_detail;
    adapter.render(state);
    EXPECT_EQ(adapter.panelDetail(), long_detail);
}

TEST(UpdaterTextLayoutTest, PreFlightShowsNoPercentageAndOneLabelledIndicator) {
    UpdaterController c(QStringLiteral("0.9.0-rc4"), QStringLiteral("0.9.0-rc5"));
    c.onStepStarted(UpStep::Download);
    UpdaterViewAdapter adapter;
    adapter.render(c.state());

    EXPECT_TRUE(adapter.indeterminate());
    EXPECT_TRUE(adapter.ringGlyph().isEmpty());
    EXPECT_EQ(adapter.ringPercent(), 0);
    EXPECT_EQ(adapter.ringDescription(), QStringLiteral("Preparing update, progress not measurable yet"));
}

TEST(UpdaterTextLayoutTest, MeasuredDownloadProgressIsDeterminate) {
    UpdaterViewAdapter adapter;
    adapter.render(DownloadInFlight());
    EXPECT_FALSE(adapter.indeterminate());
    EXPECT_TRUE(adapter.ringGlyph().isEmpty());
    // The ring reports the whole run's progress, not the download band's: the
    // download band ends at 55 %, so 38 % of it is 21 %.
    EXPECT_EQ(adapter.ringPercent(), 21);
    EXPECT_EQ(adapter.ringDescription(), QStringLiteral("21 percent"));
}

TEST(UpdaterTextLayoutTest, SuccessSaysTheAppIsRelaunchingOnlyOnce) {
    UpdaterController c(QStringLiteral("0.9.0-rc4"), QStringLiteral("0.9.0-rc5"));
    c.onStepDone(UpStep::Download);
    c.onStepDone(UpStep::CloseApp);
    c.onStepDone(UpStep::Install);
    c.onStepDone(UpStep::Verify);
    c.onStepDone(UpStep::Launch);
    c.onAllDone();
    UpdaterViewAdapter adapter;
    adapter.render(c.state());

    const QStringList copy = {adapter.panelTitle(),     adapter.panelDetail(), adapter.panelSafety(),
                              adapter.statusHeadline(), adapter.statusLine(),  adapter.hint()};
    EXPECT_EQ(CountOccurrences(copy, QStringLiteral("starting automatically")), 1);
}

TEST(UpdaterTextLayoutTest, EyebrowNamesTheRun) {
    UpdaterViewAdapter adapter;

    UpdaterController failed(QStringLiteral("0.9.0-rc4"), QStringLiteral("0.9.0-rc5"));
    failed.onFailure(FailureCase::InstallFailed, QString());
    adapter.render(failed.state());
    EXPECT_EQ(adapter.eyebrow(), QStringLiteral("EXOSNAP WAS NOT UPDATED"));

    UpdaterController reinstall(QStringLiteral("0.9.0-rc4"), QStringLiteral("0.9.0-rc5"));
    reinstall.setVerificationReinstall(true);
    reinstall.onFailure(FailureCase::InstallFailed, QString());
    adapter.render(reinstall.state());
    EXPECT_EQ(adapter.eyebrow(), QStringLiteral("EXOSNAP WAS NOT REINSTALLED"));

    UpdaterController manual(QStringLiteral("0.9.0-rc4"), QStringLiteral("0.9.0-rc5"));
    manual.setMode(exosnap::update::UpdaterMode::Manual);
    manual.onIdle();
    adapter.render(manual.state());
    EXPECT_EQ(adapter.eyebrow(), QStringLiteral("EXOSNAP UPDATER"));

    manual.onUpToDate();
    adapter.render(manual.state());
    EXPECT_EQ(adapter.eyebrow(), QStringLiteral("EXOSNAP IS UP TO DATE"));
}

TEST(UpdaterTextLayoutTest, TerminalFailureMovesTheEmphasisToTheInstalledVersion) {
    UpdaterViewAdapter adapter;
    adapter.render(Terminal(FailureCase::InstallFailed));
    EXPECT_TRUE(adapter.failedTerminal());

    adapter.render(Terminal(FailureCase::VerifyInstallFailed));
    EXPECT_TRUE(adapter.failedTerminal());
}

TEST(UpdaterTextLayoutTest, SoftSuccessKeepsTheTargetEmphasisBecauseTheUpdateDidApply) {
    UpdaterViewAdapter adapter;
    adapter.render(Terminal(FailureCase::LaunchFailed));
    EXPECT_FALSE(adapter.failedTerminal());
}

TEST(UpdaterTextLayoutTest, ProgressRingExposesTheOutcomeSemantics) {
    UpdaterViewAdapter adapter;
    adapter.render(Terminal(FailureCase::InstallFailed));
    EXPECT_EQ(adapter.ringGlyph(), QStringLiteral("warning"));
    EXPECT_EQ(adapter.ringDescription(), QStringLiteral("Update didn't complete"));

    adapter.render(Terminal(FailureCase::VerifyInstallFailed));
    EXPECT_EQ(adapter.ringGlyph(), QStringLiteral("cross"));
    EXPECT_EQ(adapter.ringDescription(), QStringLiteral("Update failed"));

    adapter.render(Terminal(FailureCase::MsiRebootRequired));
    EXPECT_EQ(adapter.ringGlyph(), QStringLiteral("check"));
    EXPECT_EQ(adapter.ringDescription(), QStringLiteral("Update complete"));
}

TEST(UpdaterTextLayoutTest, StepListExposesEveryPhaseAndItsStatus) {
    UpdaterController c(QStringLiteral("0.9.0-rc4"), QStringLiteral("0.9.0-rc5"));
    c.onStepDone(UpStep::Download);
    c.onStepDone(UpStep::CloseApp);
    c.onStepStarted(UpStep::Install);
    UpdaterViewAdapter adapter;
    adapter.render(c.state());

    ASSERT_EQ(adapter.stepRows().size(), 5);
    const QVariantList rows = adapter.stepRows();
    for (int i = 0; i < rows.size(); ++i) {
        const QVariantMap row = rows.at(i).toMap();
        EXPECT_TRUE(row.contains(QStringLiteral("label")));
        EXPECT_TRUE(row.contains(QStringLiteral("status")));
        EXPECT_TRUE(row.contains(QStringLiteral("tag")));
        EXPECT_TRUE(row.contains(QStringLiteral("accessible")));
        EXPECT_TRUE(
            row.value(QStringLiteral("accessible")).toString().contains(row.value(QStringLiteral("label")).toString()));
        EXPECT_TRUE(
            row.value(QStringLiteral("accessible")).toString().contains(row.value(QStringLiteral("tag")).toString()));
    }
}

TEST(UpdaterTextLayoutTest, CompactProgressUsesMeasuredStateAndPreservesFullDetails) {
    UpdaterViewAdapter adapter;
    adapter.render(DownloadInFlight());
    EXPECT_TRUE(adapter.progressVisible());
    EXPECT_EQ(adapter.versionText(), QStringLiteral("0.8.1 → 0.9.0"));
    const QLocale locale;
    EXPECT_EQ(adapter.compactDetail(),
              QStringLiteral("%1 of %2 MB").arg(locale.toString(38.0, 'f', 1), locale.toString(100.0, 'f', 1)));
    EXPECT_TRUE(adapter.detailsText().contains(adapter.panelSafety()));

    adapter.render(Terminal(FailureCase::VerifyInstallFailed));
    EXPECT_FALSE(adapter.progressVisible());
    EXPECT_EQ(adapter.compactDetail(), adapter.panelSafety());
    EXPECT_TRUE(adapter.detailsText().contains(adapter.panelSafety()));
    EXPECT_TRUE(adapter.detailsText().contains(adapter.panelDetail()));
}

TEST(UpdaterTextLayoutTest, CompactSuccessKeepsAutomaticLaunchVisible) {
    UpdaterController controller(QStringLiteral("0.8.1"), QStringLiteral("0.9.0"));
    controller.onAllDone();
    UpdaterViewAdapter adapter;
    adapter.render(controller.state());
    EXPECT_FALSE(adapter.progressVisible());
    EXPECT_EQ(adapter.compactDetail(), QStringLiteral("ExoSnap is starting automatically."));
    EXPECT_TRUE(adapter.primaryAction().isEmpty());
}
TEST(UpdaterTextLayoutTest, CompactTerminalSafetyRemainsVisibleForEveryFailureCase) {
    for (const auto failure :
         {FailureCase::DownloadFailed, FailureCase::VerifyDownloadFailed, FailureCase::VerifyReinstallMismatch,
          FailureCase::TargetVersionMismatch, FailureCase::HandoffRejected, FailureCase::AppWontClose,
          FailureCase::InstallFailed, FailureCase::VerifyInstallFailed, FailureCase::RestoreFailed,
          FailureCase::VerifyInstallFailedMsi, FailureCase::LaunchFailed, FailureCase::UacDeclined,
          FailureCase::MsiFailed, FailureCase::MsiRebootRequired}) {
        UpdaterViewAdapter adapter;
        adapter.render(Terminal(failure));
        EXPECT_FALSE(adapter.panelSafety().isEmpty()) << static_cast<int>(failure);
        EXPECT_EQ(adapter.compactDetail(), adapter.panelSafety()) << static_cast<int>(failure);
        EXPECT_TRUE(adapter.detailsText().contains(adapter.panelDetail())) << static_cast<int>(failure);
    }
}

TEST(UpdaterTextLayoutTest, CompactSafetyDistinguishesRestoredBackupAndUnknownInstallation) {
    UpdaterViewAdapter adapter;
    adapter.render(Terminal(FailureCase::VerifyInstallFailed));
    EXPECT_TRUE(adapter.compactDetail().contains(QStringLiteral("was restored")));
    adapter.render(Terminal(FailureCase::RestoreFailed));
    EXPECT_TRUE(adapter.compactDetail().contains(QStringLiteral("backup folder")));
    EXPECT_TRUE(adapter.compactDetail().contains(QStringLiteral("isn't ready to run")));
    adapter.render(Terminal(FailureCase::VerifyInstallFailedMsi));
    EXPECT_TRUE(adapter.compactDetail().contains(QStringLiteral("didn't confirm")));
    EXPECT_TRUE(adapter.compactDetail().contains(QStringLiteral("Repair ExoSnap")));
    adapter.render(Terminal(FailureCase::MsiRebootRequired));
    EXPECT_TRUE(adapter.compactDetail().contains(QStringLiteral("keeps working until you restart Windows")));
}

TEST(UpdaterTextLayoutTest, CompactDownloadUsesLocaleDecimalMegabytes) {
    const QLocale previous;
    QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
    UpdaterController controller(QStringLiteral("0.8.1"), QStringLiteral("0.9.0"));
    controller.onStepStarted(UpStep::Download);
    controller.onDownloadProgress(1260000, 42500000);
    UpdaterViewAdapter adapter;
    adapter.render(controller.state());
    EXPECT_EQ(adapter.compactDetail(), QStringLiteral("1,3 of 42,5 MB"));
    EXPECT_EQ(controller.state().downloaded_bytes, 1260000u);
    EXPECT_EQ(controller.state().total_bytes, 42500000u);
    QLocale::setDefault(previous);
}

TEST(UpdaterTextLayoutTest, CompactDownloadDoesNotInventUnknownSizeOrEtaAndResetsOnRetry) {
    UpdaterController controller(QStringLiteral("0.8.1"), QStringLiteral("0.9.0"));
    controller.onStepStarted(UpStep::Download);
    controller.onDownloadProgress(1250000, 42500000);
    controller.onStepStarted(UpStep::Download);
    EXPECT_EQ(controller.state().downloaded_bytes, 0u);
    EXPECT_EQ(controller.state().total_bytes, 0u);
    EXPECT_EQ(controller.flowState().downloaded_bytes, 0u);
    EXPECT_EQ(controller.flowState().total_bytes, 0u);
    EXPECT_FALSE(controller.state().determinate);
    UpdaterViewAdapter adapter;
    adapter.render(controller.state());
    EXPECT_EQ(adapter.compactDetail(), adapter.panelDetail());
    controller.onDownloadProgress(1250000, 0);
    adapter.render(controller.state());
    EXPECT_EQ(adapter.compactDetail(), adapter.panelDetail());
    EXPECT_FALSE(adapter.compactDetail().contains(QStringLiteral("MB")));
    EXPECT_FALSE(adapter.compactDetail().contains(QStringLiteral("ETA")));
}

TEST(UpdaterTextLayoutTest, CompactProtectedPhaseStatesInterruptionRule) {
    UpdaterController controller(QStringLiteral("0.8.1"), QStringLiteral("0.9.0"));
    for (const auto step : {UpStep::Install, UpStep::Verify, UpStep::Launch}) {
        controller.onStepStarted(step);
        UpdaterViewAdapter adapter;
        adapter.render(controller.state());
        EXPECT_EQ(adapter.compactDetail(), QStringLiteral("This step can't be interrupted."));
    }
}
