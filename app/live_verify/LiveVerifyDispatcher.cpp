#include "LiveVerifyDispatcher.h"

#include "LiveVerifyCommandPolicy.h"
#include "LiveVerifySource.h"

#include <QJsonArray>
#include <QJsonValue>

#include <optional>
#include <utility>

namespace exosnap::live_verify {
namespace {

QString Text(const char* literal) {
    return QString::fromLatin1(literal);
}

// Outcome / Succeeded / Failed are the shared execution result
// (control/session.h): what a command produced, before anything decides which
// fields this protocol version is allowed to carry.
using exosnap::control::Failed;
using exosnap::control::Outcome;
using exosnap::control::Succeeded;

// An intent that passed its precondition and still came back false. Rather than
// invent a second explanation, the policy is asked again against the state as it
// is NOW: if the product refused because something changed between the check and
// the call, the client gets that reason in the same structured form it would
// have got a moment earlier. Only when the state still permits the command is
// this an operational failure.
Outcome IntentRefused(const CommandDescriptor& command, const LiveVerifySource& source, const QString& error) {
    const PreconditionVerdict verdict = Evaluate(command, source.State());
    if (!verdict.allowed())
        return Failed(verdict.code, verdict.message, verdict.requirements, verdict.actual);
    return Failed(error_code::kOperationFailed, error);
}

QString ParamString(const QJsonObject& params, const char* name) {
    return params.value(Text(name)).toString();
}

qint64 ParamInt(const QJsonObject& params, const char* name) {
    return static_cast<qint64>(params.value(Text(name)).toDouble());
}

// --- Command execution ------------------------------------------------------

// The update area's own snapshot, plus what the last apply actually launched.
// The update fields are lifted out of the SAME StateToJson the shell snapshot
// publishes rather than assembled a second time, so update.getState and
// ui.getState can never describe different updates.
QJsonObject UpdatePayload(const LiveVerifySource& source) {
    QJsonObject result = StateToJson(source.State(), source.StateRevision()).value(QStringLiteral("update")).toObject();
    // Empty before the first apply. Carrying it here is what removes discovery
    // from the client: which child, at which endpoint, pinned to which version
    // is decided by the launch and reported by the launch.
    result.insert(QStringLiteral("updaterLaunch"), source.UpdaterLaunchSnapshot());
    return result;
}

Outcome ExecuteReadOnly(const QString& command, const QJsonObject& params, LiveVerifySource& source,
                        const QJsonObject& capabilities, const QJsonObject& described) {
    // system.capabilities and ipc.describe are assembled by the shared session
    // from this process's own command table and event list -- one payload
    // builder, so the two endpoints cannot describe themselves differently.
    if (command == QLatin1String("system.capabilities"))
        return Succeeded(capabilities);
    if (command == QLatin1String("ipc.describe"))
        return Succeeded(described);
    if (command == QLatin1String("ui.getState"))
        return Succeeded(StateToJson(source.State(), source.StateRevision()));
    if (command == QLatin1String("update.getState"))
        return Succeeded(UpdatePayload(source));
    if (command == QLatin1String("system.snapshot"))
        return Succeeded(source.SystemSnapshot());
    if (command == QLatin1String("app.snapshot"))
        return Succeeded(source.AppSnapshot());
    if (command == QLatin1String("window.snapshot"))
        return Succeeded(source.WindowSnapshot());
    if (command == QLatin1String("preview.snapshot"))
        return Succeeded(source.PreviewSnapshot());
    if (command == QLatin1String("record.snapshot"))
        return Succeeded(source.RecordSnapshot());
    if (command == QLatin1String("record.result"))
        return Succeeded(source.RecordResult());
    if (command == QLatin1String("overlay.snapshot"))
        return Succeeded(source.OverlaySnapshot());
    if (command == QLatin1String("editor.snapshot"))
        return Succeeded(source.EditorSnapshot());
    if (command == QLatin1String("diagnostics.snapshot"))
        return Succeeded(source.DiagnosticsSnapshot());

    // --- Observability ------------------------------------------------------
    // app.identity answers the SAME object system.hello carries. Deliberately
    // not a second identity: a handshake is a one-shot, and a client that wants
    // to re-read the build it is talking to mid-run had no way to.
    if (command == QLatin1String("app.identity"))
        return Succeeded(source.Identity());
    if (command == QLatin1String("pipeline.snapshot"))
        return Succeeded(source.PipelineSnapshot());
    if (command == QLatin1String("settings.snapshot"))
        return Succeeded(source.SettingsSnapshot());
    if (command == QLatin1String("diagnostics.results"))
        return Succeeded(source.DiagnosticsResults());
    if (command == QLatin1String("environment.snapshot"))
        return Succeeded(source.EnvironmentSnapshot());
    if (command == QLatin1String("windows.snapshot"))
        return Succeeded(source.WindowsSnapshot());
    if (command == QLatin1String("events.recent")) {
        QString error;
        const QJsonObject events = source.RecentEvents(params, &error);
        // A malformed filter is a client error, not an empty result: silently
        // widening a mistyped severity to "everything" is how a check that
        // filters for errors passes on a stream of Info records.
        if (!error.isEmpty())
            return Failed(error_code::kInvalidParams, error);
        return Succeeded(events);
    }
    if (command == QLatin1String("session.latest"))
        return Succeeded(source.SessionReport(QString()));
    if (command == QLatin1String("session.get")) {
        const QString id = ParamString(params, "recordingSessionId");
        if (id.isEmpty())
            return Failed(error_code::kInvalidParams, QStringLiteral("recordingSessionId must not be empty"));
        return Succeeded(source.SessionReport(id));
    }

    // Reachable only if a read-only command is added to the policy table without
    // a branch here; loud rather than silently answering nothing.
    return Failed(error_code::kUnknownCommand, QStringLiteral("Command %1 is listed but not implemented").arg(command));
}

// The reveal/scroll surface has to be the page the user is on. The pages stay
// resident after their first visit (QCR-602), so a Settings section IS still
// addressable from the Logs page -- and scrolling a page nobody is looking at,
// then reporting where it landed, is evidence of nothing.
//
// Enforced here rather than in the precondition table because it depends on a
// PARAMETER, and the table's predicates read only the state. It is still
// `invalid_state` with the same requires/actual shape a precondition produces,
// so a client sees no seam.
Outcome RefuseUnlessCurrentPage(const QString& surface, const LiveVerifySource& source) {
    const AutomationState state = source.State();
    if (state.page == surface)
        return Succeeded({});
    QJsonObject requirements;
    QJsonObject actual;
    requirements.insert(QStringLiteral("page"), surface);
    actual.insert(QStringLiteral("page"), state.page);
    return Failed(error_code::kInvalidState,
                  QStringLiteral("The %1 surface can only be addressed while it is the current page").arg(surface),
                  requirements, actual);
}

QJsonObject PopupResult(const char* key, bool open) {
    QJsonObject result;
    result.insert(Text(key), open ? QStringLiteral("open") : QStringLiteral("closed"));
    return result;
}

// A synchronous command whose postcondition did not hold when the state was read
// back. Declared synchronous means observable in this very response; if it is
// not, the honest answer is a failure, not `settled:false`.
Outcome PostconditionMissed(const QString& command, const QString& expectation) {
    return Failed(error_code::kOperationFailed,
                  QStringLiteral("%1 was accepted but %2 did not hold").arg(command, expectation));
}

Outcome ExecuteMutating(const CommandDescriptor& command, const ParsedRequest& request, LiveVerifySource& source) {
    QString error;
    const QJsonObject& params = request.params;

    if (command.name == QLatin1String("window.moveToScreen")) {
        if (!source.MoveWindowToScreen(ParamString(params, "screen"), &error))
            return IntentRefused(command, source, error);
        return Succeeded(source.WindowSnapshot(), /*settled=*/false);
    }

    if (command.name == QLatin1String("record.selectTarget")) {
        if (!source.SelectRecordTarget(ParamString(params, "kind"), ParamString(params, "titleFilter"), &error))
            return IntentRefused(command, source, error);
        return Succeeded(source.RecordSnapshot());
    }

    // The six transport intents. Asynchronous by nature: the returned snapshot
    // describes the state at the moment the intent was accepted, and the
    // authoritative confirmation is the record.stateChanged event or the next
    // stateRevision.
    const auto transport = [&](bool (LiveVerifySource::*intent)(QString*)) {
        if (!(source.*intent)(&error))
            return IntentRefused(command, source, error);
        return Succeeded(source.RecordSnapshot(), /*settled=*/false);
    };
    if (command.name == QLatin1String("record.start"))
        return transport(&LiveVerifySource::RecordStart);
    if (command.name == QLatin1String("record.pause"))
        return transport(&LiveVerifySource::RecordPause);
    if (command.name == QLatin1String("record.resume"))
        return transport(&LiveVerifySource::RecordResume);
    if (command.name == QLatin1String("record.stop"))
        return transport(&LiveVerifySource::RecordStop);
    if (command.name == QLatin1String("record.split"))
        return transport(&LiveVerifySource::RecordSplit);
    if (command.name == QLatin1String("record.captureFrame"))
        return transport(&LiveVerifySource::RecordCaptureFrame);

    if (command.name == QLatin1String("ui.navigate")) {
        const QString page = ParamString(params, "page");
        if (!source.Navigate(page, &error))
            return IntentRefused(command, source, error);
        // The RESULTING page, not the requested one. Navigating to the page you
        // are already on is a successful no-op, which is the same answer.
        const AutomationState after = source.State();
        if (after.page != page)
            return PostconditionMissed(command.name, QStringLiteral("the shell reaching \"%1\"").arg(page));
        QJsonObject result;
        result.insert(QStringLiteral("page"), after.page);
        return Succeeded(result);
    }

    if (command.name == QLatin1String("ui.reveal")) {
        const QString surface = ParamString(params, "surface");
        const QString target = ParamString(params, "target");
        if (const Outcome wrong = RefuseUnlessCurrentPage(surface, source); !wrong.ok)
            return wrong;
        switch (source.Reveal(surface, target, &error)) {
        case LiveVerifySource::RevealOutcome::UnknownTarget:
            return Failed(error_code::kInvalidParams, error);
        case LiveVerifySource::RevealOutcome::Failed:
            return IntentRefused(command, source, error);
        case LiveVerifySource::RevealOutcome::Revealed:
            break;
        }
        QJsonObject result;
        result.insert(QStringLiteral("surface"), surface);
        result.insert(QStringLiteral("target"), target);
        result.insert(QStringLiteral("revealed"), true);
        return Succeeded(result);
    }

    if (command.name == QLatin1String("ui.scrollHome") || command.name == QLatin1String("ui.scrollEnd")) {
        const bool to_end = command.name.endsWith(QLatin1String("End"));
        const QString surface = ParamString(params, "surface");
        if (const Outcome wrong = RefuseUnlessCurrentPage(surface, source); !wrong.ok)
            return wrong;
        const bool moved = to_end ? source.ScrollEnd(surface, &error) : source.ScrollHome(surface, &error);
        if (!moved)
            return IntentRefused(command, source, error);
        QJsonObject result;
        result.insert(QStringLiteral("surface"), surface);
        result.insert(to_end ? QStringLiteral("atEnd") : QStringLiteral("atHome"), true);
        return Succeeded(result);
    }

    if (command.name == QLatin1String("edit.open")) {
        if (!source.EditOpen(&error))
            return IntentRefused(command, source, error);
        if (!source.State().edit_session_open)
            return PostconditionMissed(command.name, QStringLiteral("an open edit session"));
        return Succeeded(source.EditorSnapshot());
    }

    if (command.name == QLatin1String("edit.close")) {
        if (!source.EditClose(&error))
            return IntentRefused(command, source, error);
        if (source.State().edit_session_open)
            return PostconditionMissed(command.name, QStringLiteral("a closed edit session"));
        return Succeeded(source.EditorSnapshot());
    }

    const auto edit_intent = [&](bool (LiveVerifySource::*intent)(QString*)) {
        if (!(source.*intent)(&error))
            return IntentRefused(command, source, error);
        // The editor snapshot IS the postcondition: it carries the clamped
        // position, the ordered trim range and the playback flag the adapter
        // published while answering.
        return Succeeded(source.EditorSnapshot());
    };
    if (command.name == QLatin1String("edit.playPause"))
        return edit_intent(&LiveVerifySource::EditPlayPause);
    if (command.name == QLatin1String("edit.timelineHome"))
        return edit_intent(&LiveVerifySource::EditTimelineHome);
    if (command.name == QLatin1String("edit.timelineEnd"))
        return edit_intent(&LiveVerifySource::EditTimelineEnd);

    const auto edit_position = [&](bool (LiveVerifySource::*intent)(qint64, QString*)) {
        if (!(source.*intent)(ParamInt(params, "positionMs"), &error))
            return IntentRefused(command, source, error);
        return Succeeded(source.EditorSnapshot());
    };
    if (command.name == QLatin1String("edit.seek"))
        return edit_position(&LiveVerifySource::EditSeek);
    if (command.name == QLatin1String("edit.setTrimIn"))
        return edit_position(&LiveVerifySource::EditSetTrimIn);
    if (command.name == QLatin1String("edit.setTrimOut"))
        return edit_position(&LiveVerifySource::EditSetTrimOut);

    if (command.name == QLatin1String("sourcePicker.open") || command.name == QLatin1String("sourcePicker.close")) {
        const bool open = command.name.endsWith(QLatin1String("open"));
        if (!source.SetSourcePickerOpen(open, &error))
            return IntentRefused(command, source, error);
        if (source.State().source_picker_open != open)
            return PostconditionMissed(command.name, QStringLiteral("the picker changing state"));
        return Succeeded(PopupResult("sourcePicker", open));
    }

    if (command.name == QLatin1String("notificationHub.open") ||
        command.name == QLatin1String("notificationHub.close")) {
        const bool open = command.name.endsWith(QLatin1String("open"));
        if (!source.SetNotificationHubOpen(open, &error))
            return IntentRefused(command, source, error);
        if (source.State().notification_hub_open != open)
            return PostconditionMissed(command.name, QStringLiteral("the hub changing state"));
        return Succeeded(PopupResult("notificationHub", open));
    }

    if (command.name == QLatin1String("update.check")) {
        if (!source.UpdateCheck(&error))
            return IntentRefused(command, source, error);
        // Accepted, not answered. The check runs on a pool thread and reports
        // through the card's next state, which is a stateRevision advance.
        return Succeeded(UpdatePayload(source), /*settled=*/false);
    }

    if (command.name == QLatin1String("update.apply")) {
        if (!source.UpdateApply(&error))
            return IntentRefused(command, source, error);
        // The updater process has been started by the time this returns
        // (LaunchUpdater stages and spawns synchronously), so the response
        // already carries the child's pid, its staged binary and the endpoint it
        // was given. The UPDATE, of course, has not happened -- hence
        // settled:false and a completion that lives in another process.
        return Succeeded(UpdatePayload(source), /*settled=*/false);
    }

    if (command.name == QLatin1String("notification.clearAll")) {
        if (!source.ClearNotifications(&error))
            return IntentRefused(command, source, error);
        QJsonObject result;
        result.insert(QStringLiteral("cleared"), true);
        return Succeeded(result);
    }

    return Failed(error_code::kUnknownCommand,
                  QStringLiteral("Command %1 is listed but not implemented").arg(command.name));
}

} // namespace

LiveVerifyDispatcher::LiveVerifyDispatcher(LiveVerifySource* source, QString run_id)
    : exosnap::control::ControlSession<AutomationState>(std::move(run_id)), source_(source) {
}

QStringList LiveVerifyDispatcher::CommandNames(int protocol) {
    return CommandNamesForProtocol(protocol);
}

QStringList LiveVerifyDispatcher::EventNames(int protocol) {
    QStringList names = {QStringLiteral("app.ready"), QStringLiteral("record.resultReady"),
                         QStringLiteral("record.stateChanged"), QStringLiteral("window.screenChanged")};
    // Protocol 2's general-purpose settle signal: it fires whenever the
    // observable product state changes, which is what lets a client wait on an
    // asynchronous command without knowing which domain-specific event covers
    // it -- or poll for one that does not exist, as the blocking surfaces had no
    // event at all.
    if (protocol >= 2) {
        names.append(QStringLiteral("ui.stateChanged"));
        // The one update event that is not a state change: it carries the CHILD
        // process's identity and endpoint, which no snapshot of this process's
        // state could report. A runner waits on it instead of watching for a
        // pipe to appear.
        names.append(QStringLiteral("update.updaterLaunched"));
    }
    names.sort();
    return names;
}

QJsonObject LiveVerifyDispatcher::Identity() const {
    return source_ != nullptr ? source_->Identity() : QJsonObject{};
}

const exosnap::control::CommandTable<AutomationState>& LiveVerifyDispatcher::Commands() const {
    return AllCommands();
}

QStringList LiveVerifyDispatcher::EventNamesFor(int protocol) const {
    return EventNames(protocol);
}

bool LiveVerifyDispatcher::HasState() const {
    return source_ != nullptr;
}

AutomationState LiveVerifyDispatcher::StateValue() const {
    return source_ != nullptr ? source_->State() : AutomationState{};
}

std::uint64_t LiveVerifyDispatcher::Revision() const {
    return source_ != nullptr ? source_->StateRevision() : 0;
}

QJsonObject LiveVerifyDispatcher::StateJson(const AutomationState& state, std::uint64_t revision) const {
    return StateToJson(state, revision);
}

exosnap::control::Outcome LiveVerifyDispatcher::Execute(const CommandDescriptor& command,
                                                        const ParsedRequest& request) {
    if (command.mutating)
        return ExecuteMutating(command, request, *source_);
    return ExecuteReadOnly(command.name, request.params, *source_, CapabilitiesPayload(request.protocol),
                           DescribePayload(request.protocol));
}

} // namespace exosnap::live_verify
