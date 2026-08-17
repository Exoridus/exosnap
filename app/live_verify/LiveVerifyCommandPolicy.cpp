#include "LiveVerifyCommandPolicy.h"

#include <QJsonArray>

#include <algorithm>

namespace exosnap::live_verify {
namespace {

// Allowed() and Refuse() are the shared refusal shape (control/command_policy.h):
// `requires` and `actual` always carry the SAME key, so a runner diffs them
// instead of reading prose.
using exosnap::control::Allowed;
using exosnap::control::Refuse;

QString Text(const char* literal) {
    return QString::fromLatin1(literal);
}

QJsonValue BlockingSurfaceValue(const QString& surface) {
    return surface.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(surface);
}

// The one guard that is shared, because it is one product rule: while a
// recovery / crash-consent / recording-error surface is up, the user has a
// question in front of them that has not been answered, and a page swapped or a
// popup opened behind it is a change they never asked for.
//
// Deliberately NOT applied to the transport (pause/resume/stop): a running
// recording that cannot be stopped is the worse state, and that asymmetry is the
// product's, stated in BlockingSurfaceArbiter.
PreconditionVerdict RefuseUnderBlockingSurface(const AutomationState& state) {
    if (!state.blocked())
        return Allowed();
    return Refuse(
        error_code::kBlocked,
        QStringLiteral("A %1 surface is open; answer it before driving the shell").arg(state.blocking_surface),
        QStringLiteral("blockingSurface"), QJsonValue(QJsonValue::Null), BlockingSurfaceValue(state.blocking_surface));
}

PreconditionVerdict RequireEditSession(const AutomationState& state, const char* command) {
    if (state.edit_session_open)
        return Allowed();
    return Refuse(error_code::kInvalidState, QStringLiteral("%1 requires an open edit session").arg(Text(command)),
                  QStringLiteral("editSession"), QStringLiteral("open"), QStringLiteral("closed"));
}

// --- Per-command preconditions ---------------------------------------------

PreconditionVerdict NoPrecondition(const AutomationState&) {
    return Allowed();
}

PreconditionVerdict CanNavigate(const AutomationState& state) {
    return RefuseUnderBlockingSurface(state);
}

PreconditionVerdict CanScroll(const AutomationState& state) {
    if (const PreconditionVerdict blocked = RefuseUnderBlockingSurface(state); !blocked.allowed())
        return blocked;
    if (IsScrollableSurface(state.page))
        return Allowed();
    return Refuse(error_code::kInvalidState,
                  QStringLiteral("The %1 page has no scrollable surface to address").arg(state.page),
                  QStringLiteral("page"), QStringLiteral("settings|diagnostics|logs"), state.page);
}

// The command whose false success this whole cut exists to remove.
//
// Order matters and is the taxonomy in one function. A blocking surface and a
// diagnostics blocker are `blocked` -- the state would be right and a product
// rule refuses anyway. Everything else is `invalid_state` -- the state is simply
// the wrong one. A runner needs the difference because the first is a product
// behaviour to record and the second is a test that drove the app wrong.
PreconditionVerdict CanStartRecording(const AutomationState& state) {
    if (const PreconditionVerdict blocked = RefuseUnderBlockingSurface(state); !blocked.allowed())
        return blocked;
    if (state.recording_state == QLatin1String("Blocked")) {
        return Refuse(error_code::kBlocked,
                      QStringLiteral("Recording is blocked by a diagnostics blocker; clear it first"),
                      QStringLiteral("recordingState"), QStringLiteral("!Blocked"), state.recording_state);
    }
    if (state.can_start)
        return Allowed();
    return Refuse(error_code::kInvalidState,
                  QStringLiteral("record.start is not available in the %1 state").arg(state.recording_state),
                  QStringLiteral("canStart"), true, false);
}

PreconditionVerdict CanPause(const AutomationState& state) {
    if (state.can_pause)
        return Allowed();
    return Refuse(error_code::kInvalidState,
                  QStringLiteral("record.pause is not available in the %1 state").arg(state.recording_state),
                  QStringLiteral("canPause"), true, false);
}

PreconditionVerdict CanResume(const AutomationState& state) {
    if (state.can_resume)
        return Allowed();
    return Refuse(error_code::kInvalidState,
                  QStringLiteral("record.resume is not available in the %1 state").arg(state.recording_state),
                  QStringLiteral("canResume"), true, false);
}

PreconditionVerdict CanStop(const AutomationState& state) {
    if (state.can_stop)
        return Allowed();
    return Refuse(error_code::kInvalidState,
                  QStringLiteral("record.stop is not available in the %1 state").arg(state.recording_state),
                  QStringLiteral("canStop"), true, false);
}

PreconditionVerdict CanSplit(const AutomationState& state) {
    if (state.can_split)
        return Allowed();
    return Refuse(error_code::kInvalidState,
                  QStringLiteral("record.split is not available in the %1 state").arg(state.recording_state),
                  QStringLiteral("splitEnabled"), true, false);
}

PreconditionVerdict CanCaptureFrame(const AutomationState& state) {
    if (state.can_capture_frame)
        return Allowed();
    return Refuse(error_code::kInvalidState,
                  QStringLiteral("record.captureFrame is not available in the %1 state").arg(state.recording_state),
                  QStringLiteral("captureFrameEnabled"), true, false);
}

PreconditionVerdict CanSelectTarget(const AutomationState& state) {
    if (state.can_select_source)
        return Allowed();
    return Refuse(error_code::kInvalidState,
                  QStringLiteral("The capture source cannot be changed in the %1 state").arg(state.recording_state),
                  QStringLiteral("canSelectSource"), true, false);
}

PreconditionVerdict CanOpenSourcePicker(const AutomationState& state) {
    if (const PreconditionVerdict blocked = RefuseUnderBlockingSurface(state); !blocked.allowed())
        return blocked;
    if (state.page != QLatin1String(page_name::kRecord)) {
        return Refuse(error_code::kInvalidState, QStringLiteral("The source picker belongs to the Record page"),
                      QStringLiteral("page"), Text(page_name::kRecord), state.page);
    }
    return CanSelectTarget(state);
}

PreconditionVerdict CanOpenEdit(const AutomationState& state) {
    if (state.can_open_edit)
        return Allowed();
    return Refuse(error_code::kInvalidState,
                  QStringLiteral("There is no completed recording this process can open in the editor"),
                  QStringLiteral("canOpenEdit"), true, false);
}

PreconditionVerdict CanPlayPause(const AutomationState& state) {
    return RequireEditSession(state, "edit.playPause");
}
PreconditionVerdict CanSeek(const AutomationState& state) {
    return RequireEditSession(state, "edit.seek");
}
PreconditionVerdict CanSetTrimIn(const AutomationState& state) {
    return RequireEditSession(state, "edit.setTrimIn");
}
PreconditionVerdict CanSetTrimOut(const AutomationState& state) {
    return RequireEditSession(state, "edit.setTrimOut");
}
PreconditionVerdict CanTimelineHome(const AutomationState& state) {
    return RequireEditSession(state, "edit.timelineHome");
}
PreconditionVerdict CanTimelineEnd(const AutomationState& state) {
    return RequireEditSession(state, "edit.timelineEnd");
}

PreconditionVerdict CanCloseEdit(const AutomationState& state) {
    if (const PreconditionVerdict open = RequireEditSession(state, "edit.close"); !open.allowed())
        return open;
    if (!state.edit_export_running)
        return Allowed();
    // Not `blocked`: a running export IS the wrong state for a close, and the
    // panel state, the trim range and the remux thread all belong to the clip
    // the export is reading.
    return Refuse(error_code::kInvalidState, QStringLiteral("An export is running; the edit session cannot be closed"),
                  QStringLiteral("exportRunning"), false, true);
}

// --- Update ------------------------------------------------------------------
//
// Both commands read the state the update CARD publishes, not a second derived
// view of the update engine. The card already resolves every rule that decides
// whether its button does anything -- the recording guard, the Scoop opt-out,
// the loop guard, an updater already running -- and re-deriving those here would
// be the drift this file exists to prevent.

PreconditionVerdict RefuseWithUpdateBlocker(const AutomationState& state, const char* command) {
    // `blocked` rather than `invalid_state`: the update area is in a perfectly
    // ordinary state and a product rule refuses anyway. A runner reads this as
    // "the product said no", which is a result to record.
    return Refuse(error_code::kBlocked,
                  QStringLiteral("%1 is refused while %2").arg(Text(command), state.update_blocker),
                  QStringLiteral("updateBlocker"), QJsonValue(QJsonValue::Null), QJsonValue(state.update_blocker));
}

PreconditionVerdict CanUpdateCheck(const AutomationState& state) {
    if (!state.update_blocker.isEmpty())
        return RefuseWithUpdateBlocker(state, "update.check");
    if (state.update_checking) {
        // Single-flight is the engine's own rule (QCR-202); asking again while a
        // check owns the state would be accepted and then discarded.
        return Refuse(error_code::kInvalidState, QStringLiteral("An update check is already running"),
                      QStringLiteral("updateChecking"), false, true);
    }
    return Allowed();
}

PreconditionVerdict CanUpdateApply(const AutomationState& state) {
    if (!state.update_blocker.isEmpty())
        return RefuseWithUpdateBlocker(state, "update.apply");
    // The two card states whose primary action launches the updater. Every other
    // state's action is "check again", and accepting update.apply there would
    // report an update starting when a check started -- the false success this
    // whole protocol version exists to remove.
    const bool offerable =
        state.update_state == QLatin1String("available") || state.update_state == QLatin1String("verify-reinstall");
    if (!offerable) {
        return Refuse(
            error_code::kInvalidState,
            QStringLiteral("update.apply needs an offered update; the card is \"%1\"").arg(state.update_state),
            QStringLiteral("updateState"), QStringLiteral("available|verify-reinstall"), state.update_state);
    }
    if (!state.update_action_enabled) {
        return Refuse(error_code::kBlocked, QStringLiteral("The update action is not currently enabled"),
                      QStringLiteral("updateActionEnabled"), true, false);
    }
    return Allowed();
}

const QStringList& PageValues() {
    static const QStringList values = {Text(page_name::kRecord), Text(page_name::kSettings),
                                       Text(page_name::kDiagnostics), Text(page_name::kLogs), Text(page_name::kAbout)};
    return values;
}

const QStringList& ScrollSurfaceValues() {
    static const QStringList values = {Text(page_name::kSettings), Text(page_name::kDiagnostics),
                                       Text(page_name::kLogs)};
    return values;
}

CommandParameter Param(const char* name, const char* type, bool required, QStringList values = {}) {
    return CommandParameter{Text(name), Text(type), required, std::move(values)};
}

} // namespace

bool IsScrollableSurface(const QString& page) {
    return ScrollSurfaceValues().contains(page);
}

const QVector<CommandDescriptor>& AllCommands() {
    static const QVector<CommandDescriptor> commands = {
        // --- Session and discovery ------------------------------------------
        {QStringLiteral("system.hello"),
         1,
         false,
         true,
         Settle::NotApplicable,
         {Param("runId", "string", true)},
         &NoPrecondition},
        {QStringLiteral("system.capabilities"), 1, false, true, Settle::NotApplicable, {}, &NoPrecondition},
        {QStringLiteral("ipc.describe"), 2, false, true, Settle::NotApplicable, {}, &NoPrecondition},

        // --- Read-only snapshots --------------------------------------------
        {QStringLiteral("system.snapshot"), 1, false, true, Settle::NotApplicable, {}, &NoPrecondition},
        {QStringLiteral("app.snapshot"), 1, false, true, Settle::NotApplicable, {}, &NoPrecondition},
        {QStringLiteral("ui.getState"), 2, false, true, Settle::NotApplicable, {}, &NoPrecondition},
        {QStringLiteral("window.snapshot"), 1, false, true, Settle::NotApplicable, {}, &NoPrecondition},
        {QStringLiteral("preview.snapshot"), 1, false, true, Settle::NotApplicable, {}, &NoPrecondition},
        {QStringLiteral("record.snapshot"), 1, false, true, Settle::NotApplicable, {}, &NoPrecondition},
        {QStringLiteral("record.result"), 1, false, true, Settle::NotApplicable, {}, &NoPrecondition},
        {QStringLiteral("overlay.snapshot"), 1, false, true, Settle::NotApplicable, {}, &NoPrecondition},
        {QStringLiteral("editor.snapshot"), 1, false, true, Settle::NotApplicable, {}, &NoPrecondition},
        {QStringLiteral("diagnostics.snapshot"), 1, false, true, Settle::NotApplicable, {}, &NoPrecondition},

        // --- Window ----------------------------------------------------------
        {QStringLiteral("window.moveToScreen"),
         1,
         true,
         true,
         Settle::Asynchronous,
         {Param("screen", "string", true)},
         &NoPrecondition},

        // --- Record -----------------------------------------------------------
        {QStringLiteral("record.selectTarget"),
         1,
         true,
         true,
         Settle::Synchronous,
         {Param("kind", "enum", true, {QStringLiteral("monitor"), QStringLiteral("window")}),
          Param("titleFilter", "string", false)},
         &CanSelectTarget},
        {QStringLiteral("record.start"), 1, true, false, Settle::Asynchronous, {}, &CanStartRecording},
        {QStringLiteral("record.pause"), 1, true, false, Settle::Asynchronous, {}, &CanPause},
        {QStringLiteral("record.resume"), 1, true, false, Settle::Asynchronous, {}, &CanResume},
        {QStringLiteral("record.stop"), 1, true, false, Settle::Asynchronous, {}, &CanStop},
        {QStringLiteral("record.split"), 1, true, false, Settle::Asynchronous, {}, &CanSplit},
        {QStringLiteral("record.captureFrame"), 1, true, false, Settle::Asynchronous, {}, &CanCaptureFrame},

        // --- Navigation and scrolling ----------------------------------------
        {QStringLiteral("ui.navigate"),
         2,
         true,
         true,
         Settle::Synchronous,
         {Param("page", "enum", true, PageValues())},
         &CanNavigate},
        {QStringLiteral("ui.reveal"),
         2,
         true,
         true,
         Settle::Synchronous,
         {Param("surface", "enum", true, ScrollSurfaceValues()), Param("target", "string", true)},
         &CanScroll},
        {QStringLiteral("ui.scrollHome"),
         2,
         true,
         true,
         Settle::Synchronous,
         {Param("surface", "enum", true, ScrollSurfaceValues())},
         &CanScroll},
        {QStringLiteral("ui.scrollEnd"),
         2,
         true,
         true,
         Settle::Synchronous,
         {Param("surface", "enum", true, ScrollSurfaceValues())},
         &CanScroll},

        // --- Edit --------------------------------------------------------------
        {QStringLiteral("edit.open"), 2, true, true, Settle::Synchronous, {}, &CanOpenEdit},
        // The one deliberately non-idempotent command in the set: it toggles, so
        // sending it twice is two different requests, and ipc.describe says so.
        {QStringLiteral("edit.playPause"), 2, true, false, Settle::Synchronous, {}, &CanPlayPause},
        {QStringLiteral("edit.seek"), 2, true, true, Settle::Synchronous, {Param("positionMs", "int", true)}, &CanSeek},
        {QStringLiteral("edit.setTrimIn"),
         2,
         true,
         true,
         Settle::Synchronous,
         {Param("positionMs", "int", true)},
         &CanSetTrimIn},
        {QStringLiteral("edit.setTrimOut"),
         2,
         true,
         true,
         Settle::Synchronous,
         {Param("positionMs", "int", true)},
         &CanSetTrimOut},
        {QStringLiteral("edit.timelineHome"), 2, true, true, Settle::Synchronous, {}, &CanTimelineHome},
        {QStringLiteral("edit.timelineEnd"), 2, true, true, Settle::Synchronous, {}, &CanTimelineEnd},
        {QStringLiteral("edit.close"), 2, true, true, Settle::Synchronous, {}, &CanCloseEdit},

        // --- Popups ------------------------------------------------------------
        {QStringLiteral("sourcePicker.open"), 2, true, true, Settle::Synchronous, {}, &CanOpenSourcePicker},
        {QStringLiteral("sourcePicker.close"), 2, true, true, Settle::Synchronous, {}, &NoPrecondition},
        {QStringLiteral("notificationHub.open"), 2, true, true, Settle::Synchronous, {}, &NoPrecondition},
        {QStringLiteral("notificationHub.close"), 2, true, true, Settle::Synchronous, {}, &NoPrecondition},
        {QStringLiteral("notification.clearAll"), 2, true, true, Settle::Synchronous, {}, &NoPrecondition},

        // --- Update ------------------------------------------------------------
        // The only product area with its own state machine that the channel did
        // not reach. Both actions are ASYNCHRONOUS: a check answers through the
        // card's next state, and an apply's real completion is a different
        // process entirely.
        {QStringLiteral("update.getState"), 2, false, true, Settle::NotApplicable, {}, &NoPrecondition},
        {QStringLiteral("update.check"), 2, true, true, Settle::Asynchronous, {}, &CanUpdateCheck},
        {QStringLiteral("update.apply"), 2, true, true, Settle::Asynchronous, {}, &CanUpdateApply},
    };
    return commands;
}

// The four functions below are the shared mechanics applied to THIS table. They
// are one-liners on purpose: the moment one of them grows a rule of its own,
// this channel and the updater's stop agreeing about what a command table means.

const CommandDescriptor* FindCommand(const QString& name) {
    return exosnap::control::FindCommandIn(AllCommands(), name);
}

QStringList CommandNamesForProtocol(int protocol) {
    return exosnap::control::CommandNamesForProtocolIn(AllCommands(), protocol);
}

PreconditionVerdict Evaluate(const CommandDescriptor& command, const AutomationState& state) {
    return exosnap::control::EvaluateIn(command, state);
}

QStringList AvailableActions(const AutomationState& state) {
    return exosnap::control::AvailableActionsIn(AllCommands(), state);
}

QJsonObject DescribeCommands(int protocol) {
    return exosnap::control::DescribeCommandsIn(AllCommands(), protocol);
}

QJsonObject StateToJson(const AutomationState& state, std::uint64_t state_revision) {
    QJsonObject json;
    json.insert(QStringLiteral("stateRevision"), static_cast<double>(state_revision));
    json.insert(QStringLiteral("page"), state.page);
    json.insert(QStringLiteral("recordingState"), state.recording_state);
    json.insert(QStringLiteral("editSession"),
                state.edit_session_open ? QStringLiteral("open") : QStringLiteral("closed"));
    json.insert(QStringLiteral("editVisible"), state.edit_visible);
    json.insert(QStringLiteral("editPlayback"), state.edit_playback);
    json.insert(QStringLiteral("exportRunning"), state.edit_export_running);
    json.insert(QStringLiteral("blockingSurface"), BlockingSurfaceValue(state.blocking_surface));
    json.insert(QStringLiteral("sourcePicker"),
                state.source_picker_open ? QStringLiteral("open") : QStringLiteral("closed"));
    json.insert(QStringLiteral("notificationHub"),
                state.notification_hub_open ? QStringLiteral("open") : QStringLiteral("closed"));

    QJsonObject source;
    source.insert(QStringLiteral("name"), state.selected_source_name);
    source.insert(QStringLiteral("kind"), state.selected_source_kind);
    json.insert(QStringLiteral("selectedSource"), source);

    QJsonObject update;
    update.insert(QStringLiteral("state"), state.update_state);
    update.insert(QStringLiteral("channel"), state.update_channel);
    update.insert(QStringLiteral("currentVersion"), state.update_current_version);
    update.insert(QStringLiteral("availableVersion"), state.update_available_version.isEmpty()
                                                          ? QJsonValue(QJsonValue::Null)
                                                          : QJsonValue(state.update_available_version));
    update.insert(QStringLiteral("checking"), state.update_checking);
    update.insert(QStringLiteral("updateAvailable"), state.update_available);
    update.insert(QStringLiteral("actionEnabled"), state.update_action_enabled);
    update.insert(QStringLiteral("blocker"),
                  state.update_blocker.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(state.update_blocker));
    json.insert(QStringLiteral("update"), update);

    QJsonArray actions;
    for (const QString& action : AvailableActions(state))
        actions.append(action);
    json.insert(QStringLiteral("availableActions"), actions);
    return json;
}

} // namespace exosnap::live_verify
