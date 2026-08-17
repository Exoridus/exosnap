// test_updater_command_policy.cpp -- the updater's precondition table and the
// state it publishes.
//
// The property under test is the one the whole construction exists for: the
// answer Dispatch() gets and the answer availableActions publishes come from the
// SAME predicate. Every command below is checked in a valid state, in an invalid
// one and -- where the product has one -- in a blocked one, and the availability
// list is checked against the verdicts rather than against a second list.

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

#include "UpdaterCommandPolicy.h"

namespace {

using exosnap::control::error_code::kBlocked;
using exosnap::control::error_code::kInvalidState;
using exosnap::update::FailureCase;
using exosnap::update::InstallState;
using exosnap::update::UpdatePhase;
using exosnap::update::UpdaterMode;
using exosnap::update::UpStep;
using exosnap::updater_control::AllCommands;
using exosnap::updater_control::AvailableActions;
using exosnap::updater_control::CommandDescriptor;
using exosnap::updater_control::Evaluate;
using exosnap::updater_control::FindCommand;
using exosnap::updater_control::FlowState;
using exosnap::updater_control::PreconditionVerdict;
using exosnap::updater_control::StateToJson;

FlowState Manual(UpdatePhase phase) {
    FlowState state;
    state.mode = UpdaterMode::Manual;
    state.checks_enabled = true;
    state.phase = phase;
    return state;
}

PreconditionVerdict VerdictFor(const char* name, const FlowState& state) {
    const CommandDescriptor* command = FindCommand(QString::fromLatin1(name));
    EXPECT_NE(command, nullptr) << name;
    return command == nullptr ? PreconditionVerdict{} : Evaluate(*command, state);
}

bool Available(const char* name, const FlowState& state) {
    return AvailableActions(state).contains(QString::fromLatin1(name));
}

// -- the invariant that makes one table worth having -------------------------

TEST(UpdaterCommandPolicy, AvailableActionsAgreesWithEveryVerdict) {
    // Over a spread of states, and for EVERY mutating command: appearing in the
    // list and passing the precondition must be the same statement. A drift here
    // is the defect the shared mechanics exist to make impossible.
    const std::vector<FlowState> states = [] {
        std::vector<FlowState> out;
        for (const UpdatePhase phase :
             {UpdatePhase::Idle, UpdatePhase::Checking, UpdatePhase::UpToDate, UpdatePhase::UpdateAvailable,
              UpdatePhase::Downloading, UpdatePhase::ReadyToApply, UpdatePhase::WaitingForParent, UpdatePhase::Applying,
              UpdatePhase::Verifying, UpdatePhase::Launching, UpdatePhase::RestartPending, UpdatePhase::RebootRequired,
              UpdatePhase::Completed, UpdatePhase::Failed, UpdatePhase::Cancelled}) {
            out.push_back(Manual(phase));
            FlowState handoff = Manual(phase);
            handoff.mode = UpdaterMode::LegacyHandoff;
            out.push_back(handoff);
            FlowState no_checks = Manual(phase);
            no_checks.checks_enabled = false;
            out.push_back(no_checks);
        }
        FlowState retryable = Manual(UpdatePhase::Failed);
        retryable.failure_case = FailureCase::DownloadFailed;
        retryable.retry_entry_step = UpStep::Download;
        out.push_back(retryable);
        return out;
    }();

    for (const FlowState& state : states) {
        const QStringList actions = AvailableActions(state);
        for (const CommandDescriptor& command : AllCommands()) {
            if (!command.mutating)
                continue;
            EXPECT_EQ(actions.contains(command.name), Evaluate(command, state).allowed())
                << qPrintable(command.name) << " in phase " << exosnap::update::UpdatePhaseName(state.phase);
        }
    }
}

TEST(UpdaterCommandPolicy, ReadOnlyCommandsAreNeverListedAsActions) {
    // A list that included every snapshot would be a list of things that are
    // always true.
    const QStringList actions = AvailableActions(Manual(UpdatePhase::Idle));
    EXPECT_FALSE(actions.contains(QStringLiteral("updater.getState")));
    EXPECT_FALSE(actions.contains(QStringLiteral("system.hello")));
}

// -- check -------------------------------------------------------------------

TEST(UpdaterCommandPolicy, CheckIsAvailableFromEveryRestingPhase) {
    for (const UpdatePhase phase :
         {UpdatePhase::Idle, UpdatePhase::UpToDate, UpdatePhase::UpdateAvailable, UpdatePhase::Failed}) {
        EXPECT_TRUE(VerdictFor("updater.check", Manual(phase)).allowed()) << exosnap::update::UpdatePhaseName(phase);
    }
}

TEST(UpdaterCommandPolicy, CheckIsInvalidWhileWorkIsInFlight) {
    const PreconditionVerdict verdict = VerdictFor("updater.check", Manual(UpdatePhase::Downloading));
    EXPECT_EQ(verdict.code, QLatin1String(kInvalidState));
    EXPECT_EQ(verdict.actual.value(QStringLiteral("phase")).toString(), QStringLiteral("downloading"));
}

TEST(UpdaterCommandPolicy, CheckIsBlockedNotInvalidWhenTheBuildMayNotLook) {
    // The phase is right and a product rule refuses anyway: a runner reads
    // `blocked` as "the product said no", which is a result, not a test defect.
    FlowState state = Manual(UpdatePhase::Idle);
    state.checks_enabled = false;
    const PreconditionVerdict verdict = VerdictFor("updater.check", state);
    EXPECT_EQ(verdict.code, QLatin1String(kBlocked));
    EXPECT_EQ(verdict.requirements.value(QStringLiteral("checksEnabled")).toBool(), true);
    EXPECT_EQ(verdict.actual.value(QStringLiteral("checksEnabled")).toBool(), false);
    EXPECT_FALSE(Available("updater.check", state));
}

TEST(UpdaterCommandPolicy, TheManualCommandsDoNotExistForAHandoffRun) {
    // A handoff was started with a confirmation already given; letting a client
    // re-enter its pipeline half-way is the "arm the handoff from outside" hole.
    FlowState state = Manual(UpdatePhase::Idle);
    state.mode = UpdaterMode::LegacyHandoff;
    for (const char* name : {"updater.check", "updater.download", "updater.apply"}) {
        const PreconditionVerdict verdict = VerdictFor(name, state);
        EXPECT_EQ(verdict.code, QLatin1String(kInvalidState)) << name;
        EXPECT_EQ(verdict.actual.value(QStringLiteral("mode")).toString(), QStringLiteral("legacyHandoff")) << name;
    }
}

// -- download / apply ---------------------------------------------------------

TEST(UpdaterCommandPolicy, DownloadOnlyFromUpdateAvailable) {
    EXPECT_TRUE(VerdictFor("updater.download", Manual(UpdatePhase::UpdateAvailable)).allowed());
    EXPECT_EQ(VerdictFor("updater.download", Manual(UpdatePhase::Idle)).code, QLatin1String(kInvalidState));
    EXPECT_EQ(VerdictFor("updater.download", Manual(UpdatePhase::ReadyToApply)).code, QLatin1String(kInvalidState));
}

TEST(UpdaterCommandPolicy, ApplyOnlyFromReadyToApply) {
    // Accepting it earlier would be the false success this contract exists to
    // prevent: there is no verified package, so `ok` would describe an
    // installation that never started.
    EXPECT_TRUE(VerdictFor("updater.apply", Manual(UpdatePhase::ReadyToApply)).allowed());
    for (const UpdatePhase phase :
         {UpdatePhase::Idle, UpdatePhase::UpdateAvailable, UpdatePhase::Downloading, UpdatePhase::Completed}) {
        EXPECT_EQ(VerdictFor("updater.apply", Manual(phase)).code, QLatin1String(kInvalidState))
            << exosnap::update::UpdatePhaseName(phase);
        EXPECT_FALSE(Available("updater.apply", Manual(phase)));
    }
}

// -- retry --------------------------------------------------------------------

TEST(UpdaterCommandPolicy, RetryFollowsThePublishedRetryEntryStep) {
    FlowState offered = Manual(UpdatePhase::Failed);
    offered.failure_case = FailureCase::InstallFailed;
    offered.retry_entry_step = UpStep::Install;
    EXPECT_TRUE(VerdictFor("updater.retry", offered).allowed());
    EXPECT_TRUE(Available("updater.retry", offered));

    // A version-gate refusal offers no retry -- it would fetch the same manifest
    // and be refused again -- and the command must not be advertised either.
    FlowState refused = Manual(UpdatePhase::Failed);
    refused.failure_case = FailureCase::TargetVersionMismatch;
    EXPECT_EQ(VerdictFor("updater.retry", refused).code, QLatin1String(kInvalidState));
    EXPECT_FALSE(Available("updater.retry", refused));
}

// -- cancel -------------------------------------------------------------------

TEST(UpdaterCommandPolicy, CancelIsAllowedOnlyWhereTheEngineHonoursIt) {
    // DownloadToFile is the only operation that checks the flag between chunks.
    EXPECT_TRUE(VerdictFor("updater.cancel", Manual(UpdatePhase::Downloading)).allowed());
    EXPECT_TRUE(Available("updater.cancel", Manual(UpdatePhase::Downloading)));
}

TEST(UpdaterCommandPolicy, CancelIsBlockedWhereItWouldBeAcceptedAndDoNothing) {
    // The correction that matters: FetchReleasesJson and WaitForProcessExit take
    // no cancel parameter at all, so a cancel accepted in those phases would
    // report success for something that never happens -- and a client could not
    // tell the difference.
    for (const UpdatePhase phase : {UpdatePhase::Checking, UpdatePhase::WaitingForParent}) {
        const PreconditionVerdict verdict = VerdictFor("updater.cancel", Manual(phase));
        EXPECT_EQ(verdict.code, QLatin1String(kBlocked)) << exosnap::update::UpdatePhaseName(phase);
        EXPECT_FALSE(Available("updater.cancel", Manual(phase)));
    }
}

TEST(UpdaterCommandPolicy, CancelIsBlockedWhereItWouldRiskTheInstallation) {
    for (const UpdatePhase phase : {UpdatePhase::Applying, UpdatePhase::Verifying, UpdatePhase::Launching}) {
        const PreconditionVerdict verdict = VerdictFor("updater.cancel", Manual(phase));
        EXPECT_EQ(verdict.code, QLatin1String(kBlocked)) << exosnap::update::UpdatePhaseName(phase);
        EXPECT_FALSE(Available("updater.cancel", Manual(phase)));
    }
}

TEST(UpdaterCommandPolicy, CancelWithNothingRunningIsInvalidNotBlocked) {
    const PreconditionVerdict verdict = VerdictFor("updater.cancel", Manual(UpdatePhase::Idle));
    EXPECT_EQ(verdict.code, QLatin1String(kInvalidState));
}

TEST(UpdaterCommandPolicy, ACancelledRunCanStartOverButHasNothingToRetry) {
    // Cancelled is terminal without being a dead end: a manual run offers a new
    // check. It offers no retry, because there is no failure to re-enter.
    const FlowState state = Manual(UpdatePhase::Cancelled);
    EXPECT_TRUE(VerdictFor("updater.check", state).allowed());
    EXPECT_TRUE(VerdictFor("updater.close", state).allowed());
    EXPECT_FALSE(VerdictFor("updater.retry", state).allowed());
    EXPECT_FALSE(VerdictFor("updater.cancel", state).allowed());
}

// -- close --------------------------------------------------------------------

TEST(UpdaterCommandPolicy, CloseFollowsTheWindowsOwnRefusalToExit) {
    for (const UpdatePhase phase : {UpdatePhase::Applying, UpdatePhase::Verifying, UpdatePhase::Launching}) {
        EXPECT_EQ(VerdictFor("updater.close", Manual(phase)).code, QLatin1String(kBlocked))
            << exosnap::update::UpdatePhaseName(phase);
    }
    for (const UpdatePhase phase : {UpdatePhase::Idle, UpdatePhase::Failed, UpdatePhase::Completed}) {
        EXPECT_TRUE(VerdictFor("updater.close", Manual(phase)).allowed()) << exosnap::update::UpdatePhaseName(phase);
    }
}

// -- the published state ------------------------------------------------------

TEST(UpdaterStatePayload, CarriesTheFailureMatrixAsDataNotProse) {
    FlowState state = Manual(UpdatePhase::Failed);
    state.failure_case = FailureCase::RestoreFailed;
    state.retry_entry_step = UpStep::Install;
    state.install_state = InstallState::StrandedInBackup;
    state.current_version = "0.9.0";
    state.target_version = "0.9.1";

    const QJsonObject json = StateToJson(state, 42);
    EXPECT_EQ(json.value(QStringLiteral("phase")).toString(), QStringLiteral("failed"));
    EXPECT_EQ(json.value(QStringLiteral("failureCase")).toString(), QStringLiteral("restoreFailed"));
    EXPECT_EQ(json.value(QStringLiteral("retryEntryStep")).toString(), QStringLiteral("install"));
    EXPECT_EQ(json.value(QStringLiteral("installState")).toString(), QStringLiteral("strandedInBackup"));
    EXPECT_EQ(json.value(QStringLiteral("currentVersion")).toString(), QStringLiteral("0.9.0"));
    EXPECT_EQ(json.value(QStringLiteral("targetVersion")).toString(), QStringLiteral("0.9.1"));
    EXPECT_EQ(json.value(QStringLiteral("stateRevision")).toDouble(), 42.0);
}

TEST(UpdaterStatePayload, AbsentFailureDetailIsNullNotAnEmptyString) {
    // An empty string is a value; null is "there is none". A runner branching on
    // the difference must not have to guess which one it got.
    const QJsonObject json = StateToJson(Manual(UpdatePhase::Idle), 1);
    EXPECT_TRUE(json.value(QStringLiteral("failureCase")).isNull());
    EXPECT_TRUE(json.value(QStringLiteral("retryEntryStep")).isNull());
    EXPECT_TRUE(json.value(QStringLiteral("targetVersion")).isNull());
    EXPECT_EQ(json.value(QStringLiteral("installState")).toString(), QStringLiteral("intact"));
}

TEST(UpdaterStatePayload, RebootRequiredIsSeparateFromTheRestartPendingPhase) {
    // Two different statements with two different follow-up actions: "the app
    // will restart" and "Windows must restart".
    FlowState relaunch = Manual(UpdatePhase::RestartPending);
    relaunch.failure_case = FailureCase::LaunchFailed;
    EXPECT_EQ(StateToJson(relaunch, 1).value(QStringLiteral("phase")).toString(), QStringLiteral("restartPending"));
    EXPECT_FALSE(StateToJson(relaunch, 1).value(QStringLiteral("rebootRequired")).toBool());

    FlowState reboot = Manual(UpdatePhase::RebootRequired);
    reboot.reboot_required = true;
    EXPECT_EQ(StateToJson(reboot, 1).value(QStringLiteral("phase")).toString(), QStringLiteral("rebootRequired"));
    EXPECT_TRUE(StateToJson(reboot, 1).value(QStringLiteral("rebootRequired")).toBool());
}

TEST(UpdaterStatePayload, DownloadBytesAreCarriedSeparately) {
    FlowState state = Manual(UpdatePhase::Downloading);
    state.downloaded_bytes = 1234;
    state.total_bytes = 5678;
    const QJsonObject download = StateToJson(state, 1).value(QStringLiteral("download")).toObject();
    EXPECT_EQ(download.value(QStringLiteral("receivedBytes")).toDouble(), 1234.0);
    EXPECT_EQ(download.value(QStringLiteral("totalBytes")).toDouble(), 5678.0);
}

TEST(UpdaterStatePayload, AvailableActionsIsPartOfTheSnapshot) {
    const QJsonArray actions =
        StateToJson(Manual(UpdatePhase::UpdateAvailable), 1).value(QStringLiteral("availableActions")).toArray();
    QStringList names;
    for (const QJsonValue& value : actions)
        names.append(value.toString());
    EXPECT_TRUE(names.contains(QStringLiteral("updater.download")));
    EXPECT_FALSE(names.contains(QStringLiteral("updater.apply")));
}

} // namespace
