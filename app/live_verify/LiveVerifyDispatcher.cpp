#include "LiveVerifyDispatcher.h"

#include "LiveVerifyCommandPolicy.h"
#include "LiveVerifySource.h"

#include <QJsonArray>
#include <QJsonValue>

#include <optional>

namespace exosnap::live_verify {
namespace {

const QString kHello = QStringLiteral("system.hello");

QJsonArray ToArray(const QStringList& values) {
    QJsonArray array;
    for (const QString& value : values)
        array.append(value);
    return array;
}

QString Text(const char* literal) {
    return QString::fromLatin1(literal);
}

// What executing a command produced. Kept separate from the wire envelope so
// the execution below never has to think about protocol versions or about which
// fields a v1 response is allowed to carry.
struct Outcome {
    bool ok = true;
    QJsonObject result;
    // Meaningful only for mutating commands; read-only ones leave `settled`
    // absent from the response entirely.
    bool settled = true;
    QString code;
    QString message;
    QJsonObject requirements;
    QJsonObject actual;
};

Outcome Succeeded(QJsonObject result, bool settled = true) {
    Outcome outcome;
    outcome.result = std::move(result);
    outcome.settled = settled;
    return outcome;
}

Outcome Failed(QString code, QString message, QJsonObject requirements = {}, QJsonObject actual = {}) {
    Outcome outcome;
    outcome.ok = false;
    outcome.settled = false;
    outcome.code = std::move(code);
    outcome.message = std::move(message);
    outcome.requirements = std::move(requirements);
    outcome.actual = std::move(actual);
    return outcome;
}

Outcome Failed(const char* code, QString message, QJsonObject requirements = {}, QJsonObject actual = {}) {
    return Failed(Text(code), std::move(message), std::move(requirements), std::move(actual));
}

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

// One validator for every command, driven by the same parameter descriptions
// ipc.describe publishes. A hand-written check per command is how a documented
// parameter set and an enforced one come apart.
std::optional<QString> ValidateParams(const CommandDescriptor& command, const QJsonObject& params) {
    for (const CommandParameter& parameter : command.parameters) {
        const QJsonValue value = params.value(parameter.name);
        const bool absent = value.isUndefined() || value.isNull();
        if (absent) {
            if (!parameter.required)
                continue;
            return QStringLiteral("%1 requires a \"%2\" parameter").arg(command.name, parameter.name);
        }
        if (parameter.type == QLatin1String("string")) {
            if (!value.isString() || value.toString().isEmpty())
                return QStringLiteral("\"%1\" must be a non-empty string").arg(parameter.name);
        } else if (parameter.type == QLatin1String("int")) {
            if (!value.isDouble())
                return QStringLiteral("\"%1\" must be a number").arg(parameter.name);
        } else if (parameter.type == QLatin1String("bool")) {
            if (!value.isBool())
                return QStringLiteral("\"%1\" must be a boolean").arg(parameter.name);
        } else if (parameter.type == QLatin1String("enum")) {
            if (!value.isString() || !parameter.values.contains(value.toString())) {
                return QStringLiteral("\"%1\" must be one of: %2")
                    .arg(parameter.name, parameter.values.join(QStringLiteral(", ")));
            }
        }
    }
    return std::nullopt;
}

QString ParamString(const QJsonObject& params, const char* name) {
    return params.value(Text(name)).toString();
}

qint64 ParamInt(const QJsonObject& params, const char* name) {
    return static_cast<qint64>(params.value(Text(name)).toDouble());
}

// --- Command execution ------------------------------------------------------

Outcome ExecuteReadOnly(const QString& command, LiveVerifySource& source, int protocol) {
    if (command == QLatin1String("system.capabilities")) {
        QJsonObject result;
        result.insert(QStringLiteral("protocol"), protocol);
        result.insert(QStringLiteral("commands"), ToArray(LiveVerifyDispatcher::CommandNames(protocol)));
        result.insert(QStringLiteral("events"), ToArray(LiveVerifyDispatcher::EventNames(protocol)));
        // Additive, and protocol 2 only: a v1 client's capabilities payload is
        // byte-identical to the one it has always received.
        if (protocol >= 2) {
            result.insert(QStringLiteral("errorCodes"), ToArray(AllErrorCodes()));
            QJsonArray supported;
            for (int version = kMinimumProtocolVersion; version <= kLatestProtocolVersion; ++version)
                supported.append(version);
            result.insert(QStringLiteral("supportedProtocols"), supported);
        }
        return Succeeded(result);
    }
    if (command == QLatin1String("ipc.describe")) {
        QJsonObject result = DescribeCommands(protocol);
        result.insert(QStringLiteral("events"), ToArray(LiveVerifyDispatcher::EventNames(protocol)));
        return Succeeded(result);
    }
    if (command == QLatin1String("ui.getState"))
        return Succeeded(StateToJson(source.State(), source.StateRevision()));
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
    : source_(source), run_id_(std::move(run_id)) {
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
    if (protocol >= 2)
        names.append(QStringLiteral("ui.stateChanged"));
    names.sort();
    return names;
}

void LiveVerifyDispatcher::ResetSession() {
    handshake_complete_ = false;
    poisoned_ = false;
    negotiated_protocol_ = kMinimumProtocolVersion;
}

QJsonObject LiveVerifyDispatcher::HandleHello(const ParsedRequest& request) {
    if (handshake_complete_) {
        return MakeErrorResponse({request.protocol, request.id, Text(error_code::kAlreadyHandshaken),
                                  QStringLiteral("This connection has already completed its handshake")});
    }

    const QJsonValue run_id = request.params.value(QStringLiteral("runId"));
    if (!run_id.isString()) {
        return MakeErrorResponse({request.protocol, request.id, Text(error_code::kInvalidParams),
                                  QStringLiteral("system.hello requires a string \"runId\" parameter")});
    }
    if (run_id.toString() != run_id_) {
        // Fatal on purpose. The run id is the connection credential; a client
        // that guessed wrong is not given a second guess on a live application.
        poisoned_ = true;
        return MakeErrorResponse({request.protocol, request.id, Text(error_code::kRunIdMismatch),
                                  QStringLiteral("Run id does not match this process")});
    }

    handshake_complete_ = true;
    negotiated_protocol_ = request.protocol;

    QJsonObject result = source_ != nullptr ? source_->Identity() : QJsonObject{};
    result.insert(QStringLiteral("protocol"), request.protocol);
    result.insert(QStringLiteral("runId"), run_id_);
    result.insert(QStringLiteral("commands"), ToArray(CommandNames(request.protocol)));
    result.insert(QStringLiteral("events"), ToArray(EventNames(request.protocol)));
    if (request.protocol >= 2) {
        result.insert(QStringLiteral("errorCodes"), ToArray(AllErrorCodes()));
        QJsonArray supported;
        for (int version = kMinimumProtocolVersion; version <= kLatestProtocolVersion; ++version)
            supported.append(version);
        result.insert(QStringLiteral("supportedProtocols"), supported);
    }

    SuccessEnvelope envelope;
    envelope.protocol = request.protocol;
    envelope.id = request.id;
    envelope.result = result;
    if (request.protocol >= 2 && source_ != nullptr)
        envelope.state_revision = source_->StateRevision();
    return MakeSuccessResponse(envelope);
}

QJsonObject LiveVerifyDispatcher::Dispatch(const ParsedRequest& request) {
    if (request.command == kHello)
        return HandleHello(request);

    if (!handshake_complete_) {
        return MakeErrorResponse({request.protocol, request.id, Text(error_code::kHandshakeRequired),
                                  QStringLiteral("system.hello must be the first command on a connection")});
    }

    if (request.protocol != negotiated_protocol_) {
        // The handshake fixed the dialect. A client that switches mid-connection
        // is either two clients on one credential or a bug that would otherwise
        // surface as a field quietly missing from half the transcript.
        poisoned_ = true;
        return MakeErrorResponse(
            {negotiated_protocol_, request.id, Text(error_code::kProtocolVersionMismatch),
             QStringLiteral("This connection negotiated protocol %1 at its handshake").arg(negotiated_protocol_)});
    }

    const CommandDescriptor* command = FindCommand(request.command);
    if (command == nullptr || command->minimum_protocol > request.protocol) {
        // Fail closed. No prefix matching, no "did you mean", no reflection --
        // and a protocol-2 command asked for over protocol 1 is answered exactly
        // as the build that predates it would have answered.
        return MakeErrorResponse({request.protocol, request.id, Text(error_code::kUnknownCommand),
                                  QStringLiteral("Unknown command: %1").arg(request.command)});
    }

    if (source_ == nullptr) {
        return MakeErrorResponse({request.protocol, request.id, Text(error_code::kUnavailable),
                                  QStringLiteral("No application state is bound to this server")});
    }

    if (const std::optional<QString> invalid = ValidateParams(*command, request.params); invalid.has_value()) {
        return MakeErrorResponse(
            {request.protocol, request.id, Text(error_code::kInvalidParams), *invalid, {}, {}, std::nullopt});
    }

    const AutomationState before = source_->State();
    if (const PreconditionVerdict verdict = Evaluate(*command, before); !verdict.allowed()) {
        ErrorEnvelope envelope;
        envelope.protocol = request.protocol;
        envelope.id = request.id;
        envelope.code = verdict.code;
        envelope.message = verdict.message;
        envelope.requirements = verdict.requirements;
        envelope.actual = verdict.actual;
        if (request.protocol >= 2)
            envelope.state_revision = source_->StateRevision();
        return MakeErrorResponse(envelope);
    }

    const Outcome outcome = command->mutating ? ExecuteMutating(*command, request, *source_)
                                              : ExecuteReadOnly(command->name, *source_, request.protocol);

    const std::optional<std::uint64_t> revision =
        request.protocol >= 2 ? std::optional<std::uint64_t>(source_->StateRevision()) : std::nullopt;

    if (!outcome.ok) {
        ErrorEnvelope envelope;
        envelope.protocol = request.protocol;
        envelope.id = request.id;
        envelope.code = outcome.code;
        envelope.message = outcome.message;
        envelope.requirements = outcome.requirements;
        envelope.actual = outcome.actual;
        envelope.state_revision = revision;
        return MakeErrorResponse(envelope);
    }

    SuccessEnvelope envelope;
    envelope.protocol = request.protocol;
    envelope.id = request.id;
    envelope.result = outcome.result;
    envelope.state_revision = revision;
    if (request.protocol >= 2 && command->settle != Settle::NotApplicable)
        envelope.settled = outcome.settled;
    if (request.include_state)
        envelope.state = StateToJson(source_->State(), source_->StateRevision());
    return MakeSuccessResponse(envelope);
}

} // namespace exosnap::live_verify
