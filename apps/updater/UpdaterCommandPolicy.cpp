#include "UpdaterCommandPolicy.h"

#include <QJsonArray>
#include <QJsonValue>

namespace exosnap::updater_control {
namespace {

using exosnap::control::Allowed;
using exosnap::control::Refuse;
using exosnap::update::UpdatePhase;
using exosnap::update::UpdaterMode;

namespace ec = exosnap::control::error_code;

QString PhaseName(UpdatePhase phase) {
    return QString::fromLatin1(exosnap::update::UpdatePhaseName(phase));
}

PreconditionVerdict NoPrecondition(const FlowState&) {
    return Allowed();
}

// Manual mode is not a mood: it decides which commands EXIST for this process.
// A handoff run was started by the application with a confirmation already
// given, and its pipeline is not something an external client gets to re-enter
// half-way -- that would be the "set the handoff from outside" hole.
PreconditionVerdict RequireManualMode(const FlowState& state, const char* command) {
    if (state.mode == UpdaterMode::Manual)
        return Allowed();
    return Refuse(ec::kInvalidState,
                  QStringLiteral("%1 belongs to the manual flow; this process is running a handoff")
                      .arg(QString::fromLatin1(command)),
                  QStringLiteral("mode"), QStringLiteral("manual"),
                  QString::fromLatin1(exosnap::update::UpdaterModeName(state.mode)));
}

PreconditionVerdict RequirePhase(const FlowState& state, UpdatePhase required, const char* command) {
    if (state.phase == required)
        return Allowed();
    return Refuse(ec::kInvalidState,
                  QStringLiteral("%1 requires the %2 phase").arg(QString::fromLatin1(command), PhaseName(required)),
                  QStringLiteral("phase"), PhaseName(required), PhaseName(state.phase));
}

// A check is a fresh start, so it is allowed from every resting phase -- but not
// from a phase with work in flight, where a second resolution would race the
// first.
PreconditionVerdict CanCheck(const FlowState& state) {
    if (const PreconditionVerdict manual = RequireManualMode(state, "updater.check"); !manual.allowed())
        return manual;
    if (!state.checks_enabled) {
        // `blocked`, not `invalid_state`: the phase is right and a product rule
        // refuses anyway. A runner reads this as "the product said no", which is
        // a result rather than a test defect.
        return Refuse(ec::kBlocked,
                      QStringLiteral("This build does not check for updates; start it with --base-url to "
                                     "check against a specific feed"),
                      QStringLiteral("checksEnabled"), true, false);
    }
    switch (state.phase) {
    case UpdatePhase::Idle:
    case UpdatePhase::UpToDate:
    case UpdatePhase::UpdateAvailable:
    case UpdatePhase::Failed:
    case UpdatePhase::Cancelled:
        return Allowed();
    default:
        break;
    }
    return Refuse(ec::kInvalidState, QStringLiteral("updater.check cannot start while %1").arg(PhaseName(state.phase)),
                  QStringLiteral("phase"), QStringLiteral("idle|upToDate|updateAvailable|failed|cancelled"),
                  PhaseName(state.phase));
}

PreconditionVerdict CanDownload(const FlowState& state) {
    if (const PreconditionVerdict manual = RequireManualMode(state, "updater.download"); !manual.allowed())
        return manual;
    return RequirePhase(state, UpdatePhase::UpdateAvailable, "updater.download");
}

PreconditionVerdict CanApply(const FlowState& state) {
    if (const PreconditionVerdict manual = RequireManualMode(state, "updater.apply"); !manual.allowed())
        return manual;
    // Only from readyToApply. Accepting it earlier would be the false success
    // this contract exists to prevent: there would be no verified package to
    // install, and `ok:true` would describe an installation that never started.
    return RequirePhase(state, UpdatePhase::ReadyToApply, "updater.apply");
}

// Retry reads exactly the field the product publishes. When the failure card
// offers no retry -- a version-gate refusal that would fetch the same manifest
// again, a Windows Installer outcome that is not this process's to repeat -- the
// state carries no retry entry step, and the command is not available either.
PreconditionVerdict CanRetry(const FlowState& state) {
    if (state.retry_entry_step.has_value())
        return Allowed();
    return Refuse(ec::kInvalidState, QStringLiteral("There is no failure this run can re-enter"),
                  QStringLiteral("retryEntryStep"), QJsonValue(QJsonValue::Null), QJsonValue(QJsonValue::Null));
}

// The honest cancel, and it is narrower than it first looks. Exactly ONE phase
// honours cancellation in a way a client can rely on:
//
//   * downloading      -- DownloadToFile checks the flag between chunks, deletes
//                         its partial file and returns. Genuinely cancellable.
//   * checking         -- FetchReleasesJson takes no cancel flag at all. The
//                         request runs to completion (or to its own timeout)
//                         regardless, so accepting a cancel here would report
//                         success for something that does not happen.
//   * waitingForParent -- WaitForProcessExit takes no cancel flag either.
//   * applying         -- portable: the staged rename must not be torn apart.
//                         MSI: the wait IS cancellable, but cancelling it only
//                         stops this process from WATCHING an elevated msiexec
//                         that keeps installing. Reporting "cancelled,
//                         installation intact" there would be a guess about
//                         another process's transaction.
//   * verifying,
//     launching        -- nothing observes the flag, and interrupting the
//                         relaunch health check risks the restore it guards.
//
// The window disables its own close X during applying/verifying/launching for
// the same reason, so a client and a user are told the same thing.
PreconditionVerdict CanCancel(const FlowState& state) {
    switch (state.phase) {
    case UpdatePhase::Downloading:
        return Allowed();
    case UpdatePhase::Applying:
    case UpdatePhase::Verifying:
    case UpdatePhase::Launching:
        return Refuse(ec::kBlocked,
                      QStringLiteral("The %1 phase cannot be interrupted without risking the installation")
                          .arg(PhaseName(state.phase)),
                      QStringLiteral("phase"), QStringLiteral("downloading"), PhaseName(state.phase));
    case UpdatePhase::Checking:
    case UpdatePhase::WaitingForParent:
        return Refuse(ec::kBlocked,
                      QStringLiteral("The %1 phase does not observe cancellation; the request would be accepted "
                                     "and have no effect")
                          .arg(PhaseName(state.phase)),
                      QStringLiteral("phase"), QStringLiteral("downloading"), PhaseName(state.phase));
    default:
        break;
    }
    return Refuse(ec::kInvalidState, QStringLiteral("Nothing is in flight to cancel"), QStringLiteral("phase"),
                  QStringLiteral("downloading"), PhaseName(state.phase));
}

// Same rule as the window's close X, from the same three phases -- so a client
// and a user are told the same thing about when the process may end.
PreconditionVerdict CanClose(const FlowState& state) {
    switch (state.phase) {
    case UpdatePhase::Applying:
    case UpdatePhase::Verifying:
    case UpdatePhase::Launching:
        return Refuse(ec::kBlocked, QStringLiteral("The updater refuses to exit during %1").arg(PhaseName(state.phase)),
                      QStringLiteral("phase"), QStringLiteral("!applying|verifying|launching"), PhaseName(state.phase));
    default:
        break;
    }
    return Allowed();
}

QJsonValue OptionalString(const std::string& value) {
    return value.empty() ? QJsonValue(QJsonValue::Null) : QJsonValue(QString::fromStdString(value));
}

} // namespace

const exosnap::control::CommandTable<FlowState>& AllCommands() {
    static const exosnap::control::CommandTable<FlowState> commands = {
        // --- Session and discovery ------------------------------------------
        {QStringLiteral("system.hello"),
         1,
         false,
         true,
         Settle::NotApplicable,
         {CommandParameter{QStringLiteral("runId"), QStringLiteral("string"), true, {}}},
         &NoPrecondition},
        {QStringLiteral("system.capabilities"), 1, false, true, Settle::NotApplicable, {}, &NoPrecondition},
        {QStringLiteral("ipc.describe"), 2, false, true, Settle::NotApplicable, {}, &NoPrecondition},

        // --- Read-only --------------------------------------------------------
        {QStringLiteral("updater.getState"), 2, false, true, Settle::NotApplicable, {}, &NoPrecondition},

        // --- Product actions --------------------------------------------------
        // Every one of them is asynchronous. Accepting an intent is not
        // completing it: the response carries settled:false and the client waits
        // for the phase to move, with its own timeout.
        {QStringLiteral("updater.check"), 2, true, true, Settle::Asynchronous, {}, &CanCheck},
        {QStringLiteral("updater.download"), 2, true, true, Settle::Asynchronous, {}, &CanDownload},
        {QStringLiteral("updater.apply"), 2, true, true, Settle::Asynchronous, {}, &CanApply},
        {QStringLiteral("updater.retry"), 2, true, true, Settle::Asynchronous, {}, &CanRetry},
        {QStringLiteral("updater.cancel"), 2, true, true, Settle::Asynchronous, {}, &CanCancel},
        {QStringLiteral("updater.close"), 2, true, true, Settle::Asynchronous, {}, &CanClose},
    };
    return commands;
}

const CommandDescriptor* FindCommand(const QString& name) {
    return exosnap::control::FindCommandIn(AllCommands(), name);
}

QStringList CommandNamesForProtocol(int protocol) {
    return exosnap::control::CommandNamesForProtocolIn(AllCommands(), protocol);
}

PreconditionVerdict Evaluate(const CommandDescriptor& command, const FlowState& state) {
    return exosnap::control::EvaluateIn(command, state);
}

QStringList AvailableActions(const FlowState& state) {
    return exosnap::control::AvailableActionsIn(AllCommands(), state);
}

QJsonObject DescribeCommands(int protocol) {
    return exosnap::control::DescribeCommandsIn(AllCommands(), protocol);
}

QJsonObject StateToJson(const FlowState& state, std::uint64_t state_revision) {
    QJsonObject json;
    json.insert(QStringLiteral("stateRevision"), static_cast<double>(state_revision));
    json.insert(QStringLiteral("mode"), QString::fromLatin1(exosnap::update::UpdaterModeName(state.mode)));
    json.insert(QStringLiteral("phase"), PhaseName(state.phase));
    json.insert(QStringLiteral("installMode"), state.install_mode == exosnap::update::InstallMode::Installed
                                                   ? QStringLiteral("installed")
                                                   : QStringLiteral("portable"));
    json.insert(QStringLiteral("checksEnabled"), state.checks_enabled);

    // The failure detail, as data. Automation must never parse UI copy, and this
    // is the whole reason the matrix is exported instead of folded into prose.
    json.insert(QStringLiteral("failureCase"),
                state.failure_case.has_value()
                    ? QJsonValue(QString::fromLatin1(exosnap::update::FailureCaseName(*state.failure_case)))
                    : QJsonValue(QJsonValue::Null));
    json.insert(QStringLiteral("retryEntryStep"),
                state.retry_entry_step.has_value()
                    ? QJsonValue(QString::fromLatin1(exosnap::update::UpStepName(*state.retry_entry_step)))
                    : QJsonValue(QJsonValue::Null));
    // The most valuable assertion an update test can make: "the existing
    // installation is unharmed". `unknown` is a real answer here, not a gap --
    // see InstallState in update_flow_state.h.
    json.insert(QStringLiteral("installState"),
                QString::fromLatin1(exosnap::update::InstallStateName(state.install_state)));
    // A WINDOWS restart, kept apart from the restartPending phase (which is the
    // app relaunch). They call for different actions.
    json.insert(QStringLiteral("rebootRequired"), state.reboot_required);

    json.insert(QStringLiteral("currentVersion"), OptionalString(state.current_version));
    json.insert(QStringLiteral("targetVersion"), OptionalString(state.target_version));
    // Machine-readable, not a log line: this is how an observer proves that the
    // operation the application started is the operation this process is
    // running. Null in manual mode, where there is no operation to correlate.
    json.insert(QStringLiteral("updateTransactionId"), OptionalString(state.update_transaction_id));

    // High-frequency and deliberately NOT revision-bearing: see
    // UpdaterAutomation.h. A client polls this for a progress bar; it waits on
    // the revision for a state change.
    QJsonObject download;
    download.insert(QStringLiteral("receivedBytes"), static_cast<double>(state.downloaded_bytes));
    download.insert(QStringLiteral("totalBytes"), static_cast<double>(state.total_bytes));
    json.insert(QStringLiteral("download"), download);

    QJsonArray actions;
    for (const QString& action : AvailableActions(state))
        actions.append(action);
    json.insert(QStringLiteral("availableActions"), actions);
    return json;
}

} // namespace exosnap::updater_control
